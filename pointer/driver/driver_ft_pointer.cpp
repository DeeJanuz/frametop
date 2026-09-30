// ft_pointer: a virtual SteamVR controller that the universal 3D mouse drives.
// The cursor is a point anchored in the room: `distance` metres from where the
// head was at the last recenter, in the yaw/pitch direction the mouse steers.
// The ray starts at the eye (the HMD origin) and aims at that point, so
// SteamVR's laser is seen end-on and only its hit dot shows. The device uses an
// invisible render model. The input relay (input/input-relay.py)
// drives it over a datagram socket.
//
// Control socket: abstract unix datagram "@ft_pointer", text commands:
//   recenter                     anchor the origin at the head and aim along the gaze
//   move <dyaw> <dpitch>         rotate the ray (degrees; +yaw turns left, +pitch up)
//   aim <yaw_deg> <pitch_deg>   absolute direction (yaw 0 = -Z, the SteamVR forward)
//   gaze                         follow the head (origin and direction) again
//   distance <metres>            cursor distance from the anchor (default 1.5)
//   pose <x> <y> <z> <yaw> <pitch>  exact pose, sent every frame by the ft-pointer helper
//   posq <x> <y> <z> <qw> <qx> <qy> <qz>  exact pose with a full rotation (tilting a panel while moving it)
//   btn <name> <0|1>             name: trigger, b, x, system, joystick, a (a = claim the laser, no click)
//   scroll <x> <y>               joystick deflection -1..1
//   show | hide                  connect (take the hand role) or disconnect (give it back)
//   role right|left|stylus|treadmill  which role to hint while connected, from now on (the
//                                helper sends POINTER_ROLE). A Frame controller in your hand
//                                takes its hand's role back (it counts as used while held), so a
//                                controller used beside the pointer needs the pointer on the
//                                other hand, or on no hand at all. Treadmill is no hand: its
//                                bindings are under /user/treadmill (docs/gaze-first.md, test 1)
//
// The device starts disconnected, so it never holds a hand role at boot (holding
// the right hand while SteamVR started left the Steam UI stuck loading). It
// connects only while the mouse is in use, so the last used device wins. Its
// role hint follows: the configured hand while connected, OptOut while not.
// SteamVR keeps a hand role reserved for a disconnected device that still hints
// that hand, so the real controller would never get it back otherwise.
//
// Settings (steamvr.vrsettings section "driver_ft_pointer"): role (int, 2 = right hand, 4 = treadmill,
// 5 = stylus).
#include <openvr_driver.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace vr;

namespace {

struct State {
    std::mutex lock;
    bool gaze = true;
    bool visible = false;  // disconnected until "show"
    bool recenter = false;  // applied on the next frame, which has the head pose
    bool anchored = false;
    float anchor[3] = {};
    bool explicitPose = false;  // set by "pose": the helper drives position and direction
    float pos[3] = {};
    bool hasQuat = false;  // set by "posq": use quat instead of yaw/pitch
    float quat[4] = {1, 0, 0, 0};

    float yaw = 0.f, pitch = 0.f;  // degrees
    bool buttons[6] = {};
    int32_t role = 0;  // "role": the hint while connected; 0 = the configured one
    float distance = 1.5f;
    float scrollX = 0.f, scrollY = 0.f;
};

const int kButtons = 6;
const char *kButtonNames[kButtons] = {"trigger", "b", "x", "system", "joystick", "a"};

HmdQuaternion_t QuatFromYawPitch(float yawDeg, float pitchDeg) {
    // Yaw about +Y, then pitch about +X. SteamVR forward is -Z.
    const float y = yawDeg * float(M_PI) / 360.f, p = pitchDeg * float(M_PI) / 360.f;
    const float cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
    return {cy * cp, cy * sp, sy * cp, -sy * sp};
}

// Rotation part of a 3x4 pose matrix as a quaternion (all four branches).
HmdQuaternion_t QuatFromMatrix(const float (&m)[3][4]) {
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0) {
        const float s = 0.5f / std::sqrt(trace + 1.f);
        return {0.25f / s, (m[2][1] - m[1][2]) * s, (m[0][2] - m[2][0]) * s, (m[1][0] - m[0][1]) * s};
    }
    if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = 2.f * std::sqrt(1.f + m[0][0] - m[1][1] - m[2][2]);
        return {(m[2][1] - m[1][2]) / s, 0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    }
    if (m[1][1] > m[2][2]) {
        const float s = 2.f * std::sqrt(1.f + m[1][1] - m[0][0] - m[2][2]);
        return {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s};
    }
    const float s = 2.f * std::sqrt(1.f + m[2][2] - m[0][0] - m[1][1]);
    return {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s};
}

class PointerDevice : public ITrackedDeviceServerDriver {
public:
    explicit PointerDevice(State *state) : state_(state) {}

    EVRInitError Activate(uint32_t objectId) override {
        objectId_ = objectId;
        auto props = VRProperties();
        const PropertyContainerHandle_t c = props->TrackedDeviceToPropertyContainer(objectId);
        container_ = c;
        EVRSettingsError err;
        const int32_t configured = VRSettings()->GetInt32("driver_ft_pointer", "role", &err);
        if (err == VRSettingsError_None && configured > 0) role_ = configured;

        props->SetStringProperty(c, Prop_ModelNumber_String, "ft_pointer");
        props->SetStringProperty(c, Prop_ManufacturerName_String, "Frametop");
        props->SetStringProperty(c, Prop_ControllerType_String, "ft_pointer");
        props->SetStringProperty(c, Prop_InputProfilePath_String, "{ft_pointer}/input/ft_pointer_profile.json");
        props->SetStringProperty(c, Prop_RenderModelName_String, "{ft_pointer}/rendermodels/ft_pointer_invisible");
        props->SetInt32Property(c, Prop_ControllerRoleHint_Int32, TrackedControllerRole_OptOut);  // until "show"
        props->SetInt32Property(c, Prop_DeviceClass_Int32, TrackedDeviceClass_Controller);
        props->SetBoolProperty(c, Prop_NeverTracked_Bool, false);

        auto input = VRDriverInput();
        input->CreateBooleanComponent(c, "/input/trigger/click", &buttons_[0]);
        input->CreateBooleanComponent(c, "/input/b/click", &buttons_[1]);
        input->CreateBooleanComponent(c, "/input/x/click", &buttons_[2]);
        input->CreateBooleanComponent(c, "/input/system/click", &buttons_[3]);
        input->CreateBooleanComponent(c, "/input/joystick/click", &buttons_[4]);
        input->CreateBooleanComponent(c, "/input/a/click", &buttons_[5]);
        input->CreateScalarComponent(c, "/input/joystick/x", &scrollX_, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);
        input->CreateScalarComponent(c, "/input/joystick/y", &scrollY_, VRScalarType_Absolute, VRScalarUnits_NormalizedTwoSided);

        VRDriverLog()->Log("ft_pointer: activated");
        return VRInitError_None;
    }

    void Deactivate() override { objectId_ = k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override {}
    void *GetComponent(const char *) override { return nullptr; }
    void DebugRequest(const char *, char *response, uint32_t size) override {
        if (size) response[0] = 0;
    }
    DriverPose_t GetPose() override { return pose_; }

    void RunFrame() {
        if (objectId_ == k_unTrackedDeviceIndexInvalid) return;
        TrackedDevicePose_t hmd{};
        VRServerDriverHost()->GetRawTrackedDevicePoses(0.f, &hmd, 1);

        State snapshot;
        {
            std::lock_guard<std::mutex> guard(state_->lock);
            if (state_->recenter || (!state_->gaze && !state_->anchored)) {
                const auto &h = hmd.mDeviceToAbsoluteTracking.m;
                if (hmd.bPoseIsValid) {
                    state_->anchor[0] = h[0][3];
                    state_->anchor[1] = h[1][3];
                    state_->anchor[2] = h[2][3];
                    state_->anchored = true;
                    if (state_->recenter) {
                        const float fx = -h[0][2], fy = -h[1][2], fz = -h[2][2];
                        // double-precision libm: the float versions are GLIBC_2.43 in the build container.
                        state_->yaw = float(std::atan2(double(-fx), double(-fz)) * 180.0 / M_PI);
                        state_->pitch = float(std::asin(double(fy)) * 180.0 / M_PI);
                        state_->gaze = false;
                        state_->recenter = false;
                    }
                }
            }
            snapshot.gaze = state_->gaze;
            std::memcpy(snapshot.anchor, state_->anchor, sizeof snapshot.anchor);
            snapshot.distance = state_->distance;
            snapshot.explicitPose = state_->explicitPose;
            std::memcpy(snapshot.pos, state_->pos, sizeof snapshot.pos);
            snapshot.hasQuat = state_->hasQuat;
            std::memcpy(snapshot.quat, state_->quat, sizeof snapshot.quat);
            snapshot.visible = state_->visible;
            snapshot.yaw = state_->yaw;
            snapshot.pitch = state_->pitch;
            std::memcpy(snapshot.buttons, state_->buttons, sizeof snapshot.buttons);
            snapshot.scrollX = state_->scrollX;
            snapshot.scrollY = state_->scrollY;
            snapshot.role = state_->role;
        }

        DriverPose_t pose{};
        pose.qWorldFromDriverRotation.w = 1.f;
        pose.qDriverFromHeadRotation.w = 1.f;
        const auto &m = hmd.mDeviceToAbsoluteTracking.m;
        if (snapshot.explicitPose) {
            for (int i = 0; i < 3; ++i) pose.vecPosition[i] = snapshot.pos[i];
            pose.qRotation = snapshot.hasQuat
                                 ? HmdQuaternion_t{snapshot.quat[0], snapshot.quat[1], snapshot.quat[2], snapshot.quat[3]}
                                 : QuatFromYawPitch(snapshot.yaw, snapshot.pitch);
        } else if (snapshot.gaze) {
            pose.vecPosition[0] = m[0][3];
            pose.vecPosition[1] = m[1][3] - 0.05f;  // just below the eyes
            pose.vecPosition[2] = m[2][3];
            pose.qRotation = QuatFromMatrix(m);
        } else {
            // Cursor point P = anchor + distance * dir(yaw, pitch). Aim from the eye at P.
            const double yr = snapshot.yaw * M_PI / 180.0, pr = snapshot.pitch * M_PI / 180.0;
            const double p[3] = {snapshot.anchor[0] - snapshot.distance * std::sin(yr) * std::cos(pr),
                                 snapshot.anchor[1] + snapshot.distance * std::sin(pr),
                                 snapshot.anchor[2] - snapshot.distance * std::cos(yr) * std::cos(pr)};
            const double eye[3] = {m[0][3], m[1][3], m[2][3]};
            const double d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
            const double horizontal = std::sqrt(d[0] * d[0] + d[2] * d[2]);
            for (int i = 0; i < 3; ++i) pose.vecPosition[i] = eye[i];
            pose.qRotation = QuatFromYawPitch(float(std::atan2(-d[0], -d[2]) * 180.0 / M_PI),
                                              float(std::atan2(d[1], horizontal) * 180.0 / M_PI));
        }
        if (snapshot.role > 0 && snapshot.role != role_) {
            role_ = snapshot.role;
            if (hinted_) VRProperties()->SetInt32Property(container_, Prop_ControllerRoleHint_Int32, role_);
        }
        if (snapshot.visible != hinted_) {
            // Claim the hand before connecting; give it up when disconnecting.
            VRProperties()->SetInt32Property(container_, Prop_ControllerRoleHint_Int32,
                                             snapshot.visible ? role_ : int32_t(TrackedControllerRole_OptOut));
            hinted_ = snapshot.visible;
        }
        const bool ok = hmd.bPoseIsValid && snapshot.visible;
        pose.poseIsValid = ok;
        pose.result = ok ? TrackingResult_Running_OK : TrackingResult_Uninitialized;
        pose.deviceIsConnected = snapshot.visible;
        pose_ = pose;
        VRServerDriverHost()->TrackedDevicePoseUpdated(objectId_, pose_, sizeof(DriverPose_t));

        auto input = VRDriverInput();
        for (int i = 0; i < kButtons; ++i) input->UpdateBooleanComponent(buttons_[i], snapshot.buttons[i], 0);
        input->UpdateScalarComponent(scrollX_, snapshot.scrollX, 0);
        input->UpdateScalarComponent(scrollY_, snapshot.scrollY, 0);
    }

private:
    State *state_;
    uint32_t objectId_ = k_unTrackedDeviceIndexInvalid;
    PropertyContainerHandle_t container_ = k_ulInvalidPropertyContainer;
    int32_t role_ = TrackedControllerRole_RightHand;
    bool hinted_ = false;  // whether the role hint currently claims role_
    DriverPose_t pose_{};
    VRInputComponentHandle_t buttons_[kButtons] = {};
    VRInputComponentHandle_t scrollX_ = 0, scrollY_ = 0;
};

class Provider : public IServerTrackedDeviceProvider {
public:
    EVRInitError Init(IVRDriverContext *context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        device_ = new PointerDevice(&state_);
        VRServerDriverHost()->TrackedDeviceAdded("ft_pointer_0", TrackedDeviceClass_Controller, device_);
        running_ = true;
        listener_ = std::thread([this] { Listen(); });
        return VRInitError_None;
    }

    void Cleanup() override {
        running_ = false;
        if (sock_ >= 0) shutdown(sock_, SHUT_RDWR);
        if (listener_.joinable()) listener_.join();
        if (sock_ >= 0) close(sock_);
        VR_CLEANUP_SERVER_DRIVER_CONTEXT();
    }

    const char *const *GetInterfaceVersions() override { return k_InterfaceVersions; }
    void RunFrame() override {
        if (device_) device_->RunFrame();
    }
    bool ShouldBlockStandbyMode() override { return false; }
    void EnterStandby() override {}
    void LeaveStandby() override {}

private:
    void Listen() {
        sock_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const char name[] = "ft_pointer";
        std::memcpy(addr.sun_path + 1, name, sizeof name - 1);  // abstract namespace
        const socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + sizeof name - 1;
        if (bind(sock_, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
            VRDriverLog()->Log("ft_pointer: cannot bind control socket");
            return;
        }
        timeval tv{0, 200000};
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char buf[256];
        while (running_) {
            const ssize_t n = recv(sock_, buf, sizeof buf - 1, 0);
            if (n <= 0) continue;
            buf[n] = 0;
            Handle(buf);
        }
    }

    void Handle(const char *cmd) {
        std::lock_guard<std::mutex> guard(state_.lock);
        char name[32];
        float a, b;
        int v;
        float x, y, z, qw, qx, qy, qz;
        if (std::sscanf(cmd, "posq %f %f %f %f %f %f %f", &x, &y, &z, &qw, &qx, &qy, &qz) == 7) {
            state_.explicitPose = true;
            state_.hasQuat = true;
            state_.gaze = false;
            state_.pos[0] = x;
            state_.pos[1] = y;
            state_.pos[2] = z;
            state_.quat[0] = qw;
            state_.quat[1] = qx;
            state_.quat[2] = qy;
            state_.quat[3] = qz;
        } else if (std::sscanf(cmd, "pose %f %f %f %f %f", &x, &y, &z, &a, &b) == 5) {
            state_.explicitPose = true;
            state_.hasQuat = false;
            state_.gaze = false;
            state_.pos[0] = x;
            state_.pos[1] = y;
            state_.pos[2] = z;
            state_.yaw = a;
            state_.pitch = b;
        } else if (std::sscanf(cmd, "move %f %f", &a, &b) == 2) {
            state_.explicitPose = false;
            if (state_.gaze) state_.recenter = true;  // first move starts from the gaze
            state_.yaw += a;  // wrap by hand: libm remainder() is GLIBC_2.43 in the build container
            while (state_.yaw > 180.f) state_.yaw -= 360.f;
            while (state_.yaw < -180.f) state_.yaw += 360.f;
            state_.pitch = std::fmax(-85.f, std::fmin(85.f, state_.pitch + b));
        } else if (std::strncmp(cmd, "recenter", 8) == 0) {
            state_.explicitPose = false;
            state_.recenter = true;
        } else if (std::sscanf(cmd, "aim %f %f", &a, &b) == 2) {
            state_.gaze = false;
            state_.yaw = a;
            state_.pitch = b;
        } else if (std::strncmp(cmd, "gaze", 4) == 0) {
            state_.gaze = true;
        } else if (std::strncmp(cmd, "hide", 4) == 0) {
            state_.visible = false;
        } else if (std::strncmp(cmd, "show", 4) == 0) {
            state_.visible = true;
        } else if (std::sscanf(cmd, "role %31s", name) == 1) {
            state_.role = !std::strcmp(name, "left")     ? TrackedControllerRole_LeftHand
                          : !std::strcmp(name, "right")  ? TrackedControllerRole_RightHand
                          : !std::strcmp(name, "stylus") ? TrackedControllerRole_Stylus
                          : !std::strcmp(name, "treadmill") ? TrackedControllerRole_Treadmill
                                                            : state_.role;
        } else if (std::sscanf(cmd, "btn %31s %d", name, &v) == 2) {
            for (int i = 0; i < kButtons; ++i)
                if (std::strcmp(name, kButtonNames[i]) == 0) state_.buttons[i] = v != 0;
        } else if (std::sscanf(cmd, "distance %f", &a) == 1) {
            state_.distance = std::fmax(0.3f, std::fmin(10.f, a));
        } else if (std::sscanf(cmd, "scroll %f %f", &a, &b) == 2) {
            state_.scrollX = a;
            state_.scrollY = b;
        }
    }

    State state_;
    PointerDevice *device_ = nullptr;
    std::thread listener_;
    std::atomic<bool> running_{false};
    int sock_ = -1;
};

Provider g_provider;

}  // namespace

extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *interfaceName, int *returnCode) {
    if (std::strcmp(interfaceName, IServerTrackedDeviceProvider_Version) == 0) return &g_provider;
    if (returnCode) *returnCode = VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
