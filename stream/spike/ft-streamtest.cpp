// ft-streamtest: spike S3 for remote displays (docs/remote-displays.md). A Moonlight client
// for one display: it pairs with a Sunshine-family host (Vibepollo), launches an app or a
// Vibepollo Remote Monitor, and shows the stream in a SteamVR overlay through the iris
// decoder and S1's GPU pass (iris.h). Each process is one client with its own certificate
// (--id), because Vibepollo ties a Remote Monitor to the client that opened it.
//
//   ft-streamtest pair HOST --id NAME [--token FILE]
//   ft-streamtest grant HOST --id NAME [--token FILE]   (permissions for a client paired by PIN)
//   ft-streamtest unpair HOST --id NAME
//   ft-streamtest monitors HOST [--token FILE]           (the host's monitors, as Vibepollo lists them)
//   ft-streamtest apps HOST --id NAME
//   ft-streamtest stream HOST --id NAME --app primary|monitor|display:DEVICE|ID|NAME [--size 2560x1440] [--fps 60]
//                 [--bitrate kbps] [--seconds s] [--hide-at s] [--show-at s] [--encrypt-video]
//                 [--distance m] [--width m] [--side m] [--up m] [--keep] [--no-vr] [--input-log]
//   ft-streamtest release HOST --id NAME    (Vibepollo's "Disconnect Monitor" for this client)
//   ft-streamtest quit HOST --id NAME       (ends the host's running app)
//
// Keys live in ~/.local/share/frametop-stream/NAME. "pair" uses the host's Web UI API with a
// token made by make-frametop-token on the host (default ~/.local/share/frametop-stream/
// hosts/HOST.token): it sends its own PIN through /api/pin, then lets the new client launch
// apps and use the mouse and keyboard, which Vibepollo doesn't give a new client. Without a
// token it prints the PIN to type into the Web UI instead. --app primary is the app named "Frametop primary
// display" (the host's real primary monitor); monitor is Vibepollo's Remote Monitor, a
// virtual display at --size and --fps. display:DEVICE (frametop-vibepollo only) streams an existing
// display named by its device id from "monitors", scaled to --size. At the end the stream's app is quit, or the Remote
// Monitor released, unless --keep.
//
// --hide-at and --show-at test keyframe sampling for a panel out of sight: from --hide-at,
// every frame that isn't an IDR is dropped before the decoder and an IDR is requested once
// a second; from --show-at, one more IDR is requested and everything is decoded again.
// --no-vr leaves SteamVR out: everything up to the RGBA ring runs, nothing is shown.
//
// The panel takes the 3D mouse and the controllers' lasers and sends the host absolute cursor
// positions, buttons and wheel notches. A drag can cross into another ft-streamtest's panel
// (see Drag). --input-log prints every input event as it arrives, with where the panel's own
// hit test puts the laser.
//
// Every 2 s it prints what arrived and what was shown, the bitrate, where the time went (host
// capture to encode as the host reports it, half the round trip, receiving the frame,
// decoding, converting), the stream's packet losses and this process's CPU use. Build:
// stream/build.sh; run in the dev container.
#include "iris.h"

#include <arpa/inet.h>
#include <sys/eventfd.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include <atomic>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

extern "C" {
#include "client.h"
#include "errors.h"
#include "http.h"
void http_cleanup(void);  // in http.c, not in http.h
}

namespace {

constexpr int kRemoteMonitor = 2147483505, kDisconnectMonitor = 2147483502;  // remote_session.h
// frametop-vibepollo: stream an existing display, named by the launch argument frametopDisplay.
constexpr int kFrametopDisplay = 2147483521;
std::string g_display;  // --app display:ID
std::string g_keyDir;   // this client's key directory, for re-initialising libgamestream

// Percent-encodes a query value (device ids are {GUID}, display names \\.\DISPLAYn).
std::string UrlEncode(const std::string &v) {
    std::string out;
    for (unsigned char c : v) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); out += b; }
    }
    return out;
}
constexpr const char *kPrimaryApp = "Frametop primary display";

// One frame from moonlight-common-c's decoder thread, waiting for the main loop.
struct Unit {
    std::vector<uint8_t> data;
    bool idr = false;
    int number = 0;
    int64_t submitNs = 0;    // when moonlight handed it over, on MonoNs()
    int64_t receivedNs = 0;  // first packet in, converted to MonoNs()
    double hostMs = 0;       // capture to encoded, as the host reports it (0: not given)
};

// Statistics for one report interval; the decoder thread adds to them under g_mu.
struct Interval {
    int received = 0, idrs = 0, skipped = 0, shown = 0;
    uint64_t bytes = 0, idrBytes = 0;
    double hostSum = 0;
    int hostN = 0;
    double receiveSum = 0, queueSum = 0;  // first packet to assembled, assembled to handed over
    double decodeSum = 0, decodeMax = 0, totalSum = 0, totalMax = 0;
};

std::mutex g_mu;
std::deque<Unit> g_units;
Interval g_iv;
int g_wake = -1;
std::atomic<bool> g_hidden{false}, g_waitIdr{false};
std::atomic<int> g_ended{0};  // connection terminated: error code + 1
std::atomic<int64_t> g_idrAskedNs{0};

// IDR requests while hidden: how long until the frame arrived and until it was on the panel.
struct Sample {
    double arriveMs, shownMs;
    size_t bytes;
};
std::vector<Sample> g_samples;
std::map<int, std::pair<int64_t, size_t>> g_pendingIdr;  // frame number -> (asked at, bytes)

int Submit(PDECODE_UNIT du) {
    const int64_t nowNs = MonoNs();
    const uint64_t nowUs = LiGetMicroseconds();
    const bool idr = du->frameType == FRAME_TYPE_IDR;
    std::lock_guard<std::mutex> lock(g_mu);
    ++g_iv.received;
    g_iv.bytes += uint64_t(du->fullLength);
    if (idr) ++g_iv.idrs, g_iv.idrBytes += uint64_t(du->fullLength);
    if (idr) g_waitIdr = false;
    if (!idr && (g_hidden || g_waitIdr)) {
        ++g_iv.skipped;
        return DR_OK;
    }
    if (g_units.size() >= 16) {  // the main loop fell far behind: start over from an IDR
        g_units.clear();
        g_waitIdr = true;
        return DR_NEED_IDR;
    }
    Unit u;
    u.data.resize(size_t(du->fullLength));
    size_t at = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) std::memcpy(&u.data[at], e->data, size_t(e->length)), at += size_t(e->length);
    u.idr = idr;
    u.number = du->frameNumber;
    u.submitNs = nowNs;
    u.receivedNs = nowNs - int64_t(nowUs - du->receiveTimeUs) * 1000;
    u.hostMs = du->frameHostProcessingLatency / 10.0;
    if (u.hostMs > 0) g_iv.hostSum += u.hostMs, ++g_iv.hostN;
    g_iv.receiveSum += (du->enqueueTimeUs - du->receiveTimeUs) / 1e3;
    g_iv.queueSum += (nowUs - du->enqueueTimeUs) / 1e3;
    if (idr && g_hidden) {
        const int64_t asked = g_idrAskedNs.exchange(0);
        if (asked) g_pendingIdr[du->frameNumber] = {asked, size_t(du->fullLength)};
    }
    g_units.push_back(std::move(u));
    const uint64_t one = 1;
    if (write(g_wake, &one, sizeof one) < 0) {}
    return DR_OK;
}

int Setup(int format, int width, int height, int fps, void *, int) {
    std::printf("stream: %s %dx%d at %d fps\n", (format & VIDEO_FORMAT_MASK_H265) ? "HEVC" : "H.264", width, height, fps);
    return (format & VIDEO_FORMAT_MASK_H265) ? 0 : -1;
}

void Log(const char *format, ...) {
    va_list a;
    va_start(a, format);
    std::printf("moonlight: ");
    std::vprintf(format, a);
    va_end(a);
    std::fflush(stdout);
}

void StageFailed(int stage, int error) { std::printf("stream: %s failed (%d)\n", LiGetStageName(stage), error); }

void Terminated(int error) {
    std::printf("stream: connection ended (%d)\n", error);
    g_ended = error + 1;
    g_stop = 1;
}

void Status(int status) { std::printf("stream: connection %s\n", status == CONN_STATUS_POOR ? "poor" : "okay"); }

std::string KeyDir(const std::string &id) {
    const char *home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.local/share/frametop-stream/" + id;
}

std::string ReadUniqueId(const std::string &dir) {
    char buf[17] = "0123456789ABCDEF";
    FILE *f = std::fopen((dir + "/uniqueid.dat").c_str(), "rb");
    if (f) {
        if (std::fread(buf, 1, 16, f) != 16) std::strcpy(buf, "0123456789ABCDEF");
        std::fclose(f);
    }
    return buf;
}

// libgamestream gives every client the same id unless uniqueid.dat exists; hosts track
// pairing by it, so each client gets a random one first.
void NewUniqueId(const std::string &dir) {
    const std::string path = dir + "/uniqueid.dat";
    if (ReadUniqueId(dir) != "0123456789ABCDEF") return;
    for (size_t at = dir.find('/', 1); ; at = dir.find('/', at + 1)) {
        mkdir(dir.substr(0, at).c_str(), 0700);
        if (at == std::string::npos) break;
    }
    std::random_device rd;
    char id[17];
    std::snprintf(id, sizeof id, "%08X%08X", rd(), rd());
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f) std::fwrite(id, 1, 16, f), std::fclose(f);
}

// The attribute `name` of the response's root element.
std::string Attribute(const char *xml, size_t len, const char *name) {
    const std::string s(xml, len), key = std::string(name) + "=\"";
    const size_t at = s.find(key);
    if (at == std::string::npos) return "";
    const size_t end = s.find('"', at + key.size());
    return s.substr(at + key.size(), end - at - key.size());
}

// libgamestream keeps one curl handle whose connections stay open (it forbids reuse only
// on FreeBSD). Vibepollo closes an HTTPS connection with a blocking TLS shutdown on its only
// HTTPS thread, so a connection left idle here freezes its API for every client until this
// process exits. So every request is followed by dropping the handle, and with it the
// connection.
void DropConnection() {
    http_cleanup();
    http_init(g_keyDir.c_str(), 0);
}

// /launch (or /resume) for appId, as gs_start_app does it, but without its mode check and
// with the status code kept: Vibepollo answers 503 while a Remote Monitor is being set up.
int Launch(SERVER_DATA &server, STREAM_CONFIGURATION &cfg, int appId, const std::string &uniqueId, bool resume,
           std::string *message) {
    std::random_device rd;
    for (auto &b : cfg.remoteInputAesKey) b = char(rd());
    std::memset(cfg.remoteInputAesIv, 0, sizeof cfg.remoteInputAesIv);
    uint32_t rikeyid = rd();
    std::memcpy(cfg.remoteInputAesIv, &rikeyid, sizeof rikeyid);
    char rikey[33];
    for (int i = 0; i < 16; ++i) std::snprintf(rikey + 2 * i, 3, "%02x", uint8_t(cfg.remoteInputAesKey[i]));
    char url[4096];
    std::snprintf(url, sizeof url,
                  "https://%s:%u/%s?uniqueid=%s&uuid=%08x-0000-4000-8000-%012x&appid=%d&mode=%dx%dx%d&additionalStates=1"
                  "&sops=0&rikey=%s&rikeyid=%d&localAudioPlayMode=1&surroundAudioInfo=%d&remoteControllersBitmap=0&gcmap=0%s%s",
                  server.serverInfo.address, server.httpsPort, resume ? "resume" : "launch", uniqueId.c_str(), rd(), rd(),
                  appId, cfg.width, cfg.height, cfg.fps, rikey, int(htonl(rikeyid)),
                  SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(cfg.audioConfiguration), LiGetLaunchUrlQueryParameters(),
                  appId == kFrametopDisplay ? ("&frametopDisplay=" + UrlEncode(g_display)).c_str() : "");
    PHTTP_DATA data = http_create_data();
    if (!data) return -1;
    int code = -1;
    if (http_request(url, data) == GS_OK) {
        const std::string c = Attribute(data->memory, data->size, "status_code");
        code = c.empty() ? -1 : std::atoi(c.c_str());
        *message = Attribute(data->memory, data->size, "status_message");
        char *session = nullptr;
        if (code == 200 && xml_search(data->memory, data->size, const_cast<char *>("sessionUrl0"), &session) == GS_OK)
            server.serverInfo.rtspSessionUrl = session;
    } else {
        *message = gs_error ? gs_error : "request failed";
    }
    http_free_data(data);
    DropConnection();
    return code;
}

// What a Frametop client may do on a Vibepollo host: list apps, view and launch them, and
// send mouse and keyboard input (crypto::PERM in Vibepollo's crypto.h).
constexpr uint32_t kPermMouse = 1u << 11, kPermKeyboard = 1u << 12;
constexpr uint32_t kPermList = 1u << 24, kPermView = 1u << 25, kPermLaunch = 1u << 26;
constexpr uint32_t kFrametopPerm = kPermList | kPermView | kPermLaunch | kPermMouse | kPermKeyboard;

size_t Append(char *data, size_t size, size_t n, void *out) {
    static_cast<std::string *>(out)->append(data, size * n);
    return size * n;
}

// The host's Web UI API (port 47990), with a scoped token.
struct WebApi {
    std::string base, token;

    bool Load(const std::string &host, const std::string &file) {
        std::ifstream f(file);
        if (!(f >> token)) return false;
        base = "https://" + host + ":47990";
        return true;
    }

    // The HTTP status (or -1), with the response's JSON in *out.
    long Call(const char *method, const std::string &path, const nlohmann::json *body, nlohmann::json *out) {
        CURL *c = curl_easy_init();
        if (!c) return -1;
        std::string text;
        const std::string payload = body ? body->dump() : "";
        curl_slist *headers = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
        if (body) headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_URL, (base + path).c_str());
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, payload.c_str());
        // The Web UI's certificate is self-signed. ft-stream would pin the one it saw first.
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, Append);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &text);
        long code = -1;
        if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(c);
        *out = nlohmann::json::parse(text, nullptr, false);
        return code;
    }

    // Paired clients by host id: name and permissions.
    bool Clients(std::map<std::string, std::pair<std::string, uint32_t>> *clients) {
        nlohmann::json r;
        const long code = Call("GET", "/api/clients/list", nullptr, &r);
        if (code != 200 || !r.is_object() || !r.contains("named_certs"))
            return std::fprintf(stderr, "web api: clients/list: %ld\n", code), false;
        for (const auto &c : r["named_certs"]) (*clients)[c.value("uuid", "")] = {c.value("name", ""), c.value("perm", 0u)};
        return true;
    }

    // Vibepollo's update replaces the whole record, so every field it takes is given.
    bool Grant(const std::string &uuid, const std::string &name) {
        const nlohmann::json body = {{"uuid", uuid},
                                     {"name", name},
                                     {"perm", kFrametopPerm},
                                     {"display_mode", ""},
                                     {"output_name_override", ""},
                                     {"virtual_display_mode", ""},
                                     {"virtual_display_layout", ""},
                                     {"always_use_virtual_display", false},
                                     {"enable_legacy_ordering", true},
                                     {"allow_client_commands", false},
                                     {"do", nlohmann::json::array()},
                                     {"undo", nlohmann::json::array()}};
        nlohmann::json r;
        const long code = Call("POST", "/api/clients/update", &body, &r);
        if (code != 200 || !r.is_object() || !r.value("status", false))
            return std::fprintf(stderr, "web api: clients/update: %ld\n", code), false;
        return true;
    }
};

std::string TokenFile(const std::string &host) { return KeyDir("hosts") + "/" + host + ".token"; }

int FindApp(SERVER_DATA &server, const std::string &want) {
    if (want == "monitor") return kRemoteMonitor;
    if (want.rfind("display:", 0) == 0) return g_display = want.substr(8), kFrametopDisplay;
    if (!want.empty() && std::all_of(want.begin(), want.end(), ::isdigit)) return std::atoi(want.c_str());
    const std::string name = want == "primary" ? kPrimaryApp : want;
    PAPP_LIST list = nullptr;
    const int listed = gs_applist(&server, &list);
    DropConnection();
    if (listed != GS_OK) return std::fprintf(stderr, "can't list the host's apps: %s\n", gs_error), 0;
    for (PAPP_LIST a = list; a; a = a->next)
        if (name == a->name) return a->id;
    std::fprintf(stderr, "the host has no app named \"%s\"\n", name.c_str());
    return 0;
}

double Ms(int64_t ns) { return ns / 1e6; }

// A drag that crosses panels. SteamVR keeps sending a held button's moves to the panel the
// press was on, with coordinates outside it once the laser has left, and the panel now under
// the laser hears nothing. Each panel streams a different display of the host, so only the
// panel under the laser can put the host's cursor there: the panel with the press publishes
// which device holds the button, and every other panel hit-tests that device's ray against
// itself. Frametop's own client will route this inside its one process; this spike's
// processes share the state through a small file in /dev/shm.
struct Drag {
    std::atomic<uint32_t> device;  // the laser holding a button
    std::atomic<int32_t> owner;    // pid of the panel the press was on, 0 for none
};

Drag *SharedDrag() {
    const int fd = open("/dev/shm/frametop-streamtest-drag", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return nullptr;
    void *p = ftruncate(fd, sizeof(Drag)) == 0 ? mmap(nullptr, sizeof(Drag), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                                               : MAP_FAILED;
    close(fd);
    return p == MAP_FAILED ? nullptr : static_cast<Drag *>(p);
}

vr::HmdMatrix34_t Mul(const vr::HmdMatrix34_t &a, const vr::HmdMatrix34_t &b) {
    vr::HmdMatrix34_t r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0);
    return r;
}

// SteamVR's laser starts at the render model's "tip" component, which on the Frame's
// controllers points 40 degrees below the pose's -Z (as in ft-screens' LaserPose,
// screens/vr.cpp). The 3D mouse's device has no tip and aims along its pose. A model that
// isn't loaded yet is asked again next time.
vr::HmdMatrix34_t TipOffset(vr::TrackedDeviceIndex_t device) {
    static std::map<vr::TrackedDeviceIndex_t, std::pair<std::string, vr::HmdMatrix34_t>> found;
    char model[256] = "";
    vr::VRSystem()->GetStringTrackedDeviceProperty(device, vr::Prop_RenderModelName_String, model, sizeof model);
    auto it = found.find(device);
    if (it != found.end() && it->second.first == model) return it->second.second;
    vr::RenderModel_ControllerMode_State_t mode{};
    vr::RenderModel_ComponentState_t state{};
    vr::VRControllerState_t buttons{};
    if (model[0] && vr::VRRenderModels()->GetComponentState(model, vr::k_pch_Controller_Component_Tip, &buttons, &mode, &state)) {
        found[device] = {model, state.mTrackingToComponentLocal};
        return state.mTrackingToComponentLocal;
    }
    return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
}

// Where `device`'s laser meets the panel, in stream pixels from the top left.
bool HitPanel(vr::VROverlayHandle_t ov, vr::TrackedDeviceIndex_t device, int width, int height, int *x, int *y) {
    if (device >= vr::k_unMaxTrackedDeviceCount) return false;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    if (!poses[device].bPoseIsValid) return false;
    const vr::HmdMatrix34_t m = Mul(poses[device].mDeviceToAbsoluteTracking, TipOffset(device));
    vr::VROverlayIntersectionParams_t params = {};
    params.eOrigin = vr::TrackingUniverseStanding;
    params.vSource = {{m.m[0][3], m.m[1][3], m.m[2][3]}};
    params.vDirection = {{-m.m[0][2], -m.m[1][2], -m.m[2][2]}};
    vr::VROverlayIntersectionResults_t r;
    if (!vr::VROverlay()->ComputeOverlayIntersection(ov, &params, &r)) return false;
    // The UVs count from the bottom left, like the overlay's mouse events.
    *x = std::clamp(int(r.vUVs.v[0] * width), 0, width - 1);
    *y = std::clamp(int((1 - r.vUVs.v[1]) * height), 0, height - 1);
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ft-streamtest pair|grant|unpair|monitors|apps|stream|release|quit HOST --id NAME [options]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    std::string host = argv[2], id, app = "primary", tokenFile;
    int width = 2560, height = 1440, fps = 60, bitrate = 0;
    double seconds = 0, hideAt = 0, showAt = 0, distance = 1.3, panel = 1.2, side = 0, up = 0;
    bool keep = false, encryptVideo = false, noVr = false, inputLog = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--id") id = v, ++i;
        else if (a == "--token") tokenFile = v, ++i;
        else if (a == "--app") app = v, ++i;
        else if (a == "--size") std::sscanf(v, "%dx%d", &width, &height), ++i;
        else if (a == "--fps") fps = std::atoi(v), ++i;
        else if (a == "--bitrate") bitrate = std::atoi(v), ++i;
        else if (a == "--seconds") seconds = std::atof(v), ++i;
        else if (a == "--hide-at") hideAt = std::atof(v), ++i;
        else if (a == "--show-at") showAt = std::atof(v), ++i;
        else if (a == "--distance") distance = std::atof(v), ++i;
        else if (a == "--width") panel = std::atof(v), ++i;
        else if (a == "--side") side = std::atof(v), ++i;
        else if (a == "--up") up = std::atof(v), ++i;
        else if (a == "--keep") keep = true;
        else if (a == "--encrypt-video") encryptVideo = true;
        else if (a == "--no-vr") noVr = true;
        else if (a == "--input-log") inputLog = true;
        else return std::fprintf(stderr, "unknown option %s\n", argv[i]), 2;
    }
    if (tokenFile.empty()) tokenFile = TokenFile(host);
    WebApi api;
    const bool haveToken = api.Load(host, tokenFile);
    if (cmd == "monitors") {
        if (!haveToken) return std::fprintf(stderr, "no token in %s\n", tokenFile.c_str()), 1;
        nlohmann::json r;
        const long code = api.Call("GET", "/api/display-devices?detail=full", nullptr, &r);
        if (code != 200) return std::fprintf(stderr, "display-devices: %ld\n", code), 1;
        std::printf("%s\n", r.dump(2).c_str());
        return 0;
    }
    if (id.empty()) return std::fprintf(stderr, "--id NAME is required: each display is its own client\n"), 2;
    // Moonlight's rule of thumb, about 20 Mbit/s for 1080p60, scaled by pixels and fps.
    if (!bitrate) bitrate = int(std::min(150000.0, 20000.0 * width * height * fps / (1920.0 * 1080 * 60)));

    const std::string dir = KeyDir(id);
    umask(077);  // the client's private key
    NewUniqueId(dir);
    SERVER_DATA server{};
    g_keyDir = dir;
    if (gs_init(&server, host.data(), 0, dir.c_str(), 0, true) != GS_OK)
        return std::fprintf(stderr, "can't reach %s: %s\n", host.c_str(), gs_error ? gs_error : "?"), 1;
    DropConnection();
    std::printf("host %s: %s, %s, %s, current app %d\n", host.c_str(), server.serverInfo.serverInfoAppVersion,
                server.gpuType ? server.gpuType : "?", server.paired ? "paired" : "not paired", server.currentGame);

    if (cmd == "pair") {
        if (server.paired) return std::printf("%s is already paired as %s\n", host.c_str(), id.c_str()), 0;
        std::random_device rd;
        char pin[5];
        std::snprintf(pin, sizeof pin, "%04u", rd() % 10000);
        if (!haveToken) {
            std::printf("PIN %s: enter it in the host's Web UI (PIN pairing), with the device name %s\n", pin, id.c_str());
            std::fflush(stdout);
            if (gs_pair(&server, pin) != GS_OK) return std::fprintf(stderr, "pairing failed: %s\n", gs_error ? gs_error : "?"), 1;
            std::printf("paired. Now allow it to launch apps and use the mouse and keyboard in the Web UI (Clients).\n");
            return 0;
        }
        std::map<std::string, std::pair<std::string, uint32_t>> before, after;
        if (!api.Clients(&before)) return 1;
        // gs_pair waits for the PIN, so it runs beside the request that gives it.
        std::atomic<int> result{GS_FAILED};
        std::atomic<bool> done{false};
        std::thread pairing([&] { result = gs_pair(&server, pin), done = true; });
        bool given = false;
        for (int i = 0; i < 40 && !given && !done; ++i) {
            usleep(250'000);
            const nlohmann::json body = {{"pin", pin}, {"name", id}};
            nlohmann::json r;
            given = api.Call("POST", "/api/pin", &body, &r) == 200 && r.is_object() && r.value("status", false);
        }
        if (!given) {
            std::fprintf(stderr, "the host took no PIN for this pairing (token scopes?)\n");
            std::fflush(stderr);
            std::_Exit(1);  // gs_pair is still waiting
        }
        pairing.join();
        if (result != GS_OK) return std::fprintf(stderr, "pairing failed: %s\n", gs_error ? gs_error : "?"), 1;
        if (!api.Clients(&after)) return 1;
        std::string uuid;
        for (const auto &[u, c] : after)
            if (!before.count(u) && c.first == id) uuid = u;
        if (uuid.empty()) return std::fprintf(stderr, "paired, but the host lists no new client named %s\n", id.c_str()), 1;
        if (!api.Grant(uuid, id)) return 1;
        std::printf("paired as %s (host id %s), allowed to launch and to use the mouse and keyboard\n", id.c_str(), uuid.c_str());
        return 0;
    }
    if (cmd == "grant") {
        std::map<std::string, std::pair<std::string, uint32_t>> clients;
        if (!haveToken) return std::fprintf(stderr, "no token in %s\n", tokenFile.c_str()), 1;
        if (!api.Clients(&clients)) return 1;
        std::vector<std::string> named;
        for (const auto &[u, c] : clients)
            if (c.first == id) named.push_back(u);
        if (named.size() != 1) return std::fprintf(stderr, "the host has %zu clients named %s\n", named.size(), id.c_str()), 1;
        if (!api.Grant(named[0], id)) return 1;
        std::printf("%s may now launch and use the mouse and keyboard (perm was %u)\n", id.c_str(), clients[named[0]].second);
        return 0;
    }
    if (cmd == "unpair") {
        const int r = gs_unpair(&server);
        std::printf("unpair: %s\n", r == GS_OK ? "done" : gs_error ? gs_error : "failed");
        return r == GS_OK ? 0 : 1;
    }
    if (!server.paired) return std::fprintf(stderr, "not paired as %s: run ft-streamtest pair %s --id %s\n", id.c_str(), host.c_str(), id.c_str()), 1;
    const std::string uniqueId = ReadUniqueId(dir);

    if (cmd == "apps") {
        PAPP_LIST list = nullptr;
        const int listed = gs_applist(&server, &list);
        DropConnection();
        if (listed != GS_OK) return std::fprintf(stderr, "can't list apps: %s\n", gs_error), 1;
        for (PAPP_LIST a = list; a; a = a->next) std::printf("%11d  %s\n", a->id, a->name);
        return 0;
    }
    STREAM_CONFIGURATION cfg;
    LiInitializeStreamConfiguration(&cfg);
    cfg.width = width, cfg.height = height, cfg.fps = fps, cfg.bitrate = bitrate;
    cfg.packetSize = 1392;
    cfg.streamingRemotely = STREAM_CFG_LOCAL;
    cfg.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg.supportedVideoFormats = VIDEO_FORMAT_H265;
    cfg.clientRefreshRateX100 = fps * 100;
    cfg.colorSpace = COLORSPACE_REC_709;
    cfg.colorRange = COLOR_RANGE_LIMITED;
    cfg.encryptionFlags = encryptVideo ? ENCFLG_ALL : ENCFLG_AUDIO;
    std::string message;
    if (cmd == "release" || cmd == "quit") {
        int code;
        if (cmd == "quit") code = gs_quit_app(&server) == GS_OK ? 200 : -1, message = gs_error ? gs_error : "";
        else code = Launch(server, cfg, kDisconnectMonitor, uniqueId, false, &message);
        std::printf("%s: %d %s\n", cmd.c_str(), code, message.c_str());
        return code == 200 || code == 410 ? 0 : 1;
    }
    if (cmd != "stream") return std::fprintf(stderr, "unknown command %s\n", cmd.c_str()), 2;

    const int appId = FindApp(server, app);
    if (!appId) return 1;
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    vr::EVRInitError err = vr::VRInitError_None;
    if (!noVr) vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) return std::fprintf(stderr, "openvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err)), 1;
    Decoder dec;
    Converter conv;
    dec.toSteamVR = false;
    conv.toSteamVR = !noVr;
    if (!dec.Open("/dev/video-dec0", 6) || !conv.Init()) return 1;

    vr::VROverlayHandle_t ov = vr::k_ulOverlayHandleInvalid;
    if (!noVr) {
        const std::string key = "frametop.streamtest." + id;
        if (vr::VROverlay()->CreateOverlay(key.c_str(), ("Stream test " + id).c_str(), &ov) != vr::VROverlayError_None)
            return std::fprintf(stderr, "can't create the overlay (already running?)\n"), 1;
        vr::VROverlay()->SetOverlayWidthInMeters(ov, float(panel));
        vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_IgnoreTextureAlpha, true);
        // The 3D mouse and the controllers' lasers, as on Frametop's own panels (screens/vr.cpp).
        vr::VROverlay()->SetOverlayInputMethod(ov, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
        vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
        const vr::HmdVector2_t scale = {float(width), float(height)};
        vr::VROverlay()->SetOverlayMouseScale(ov, &scale);
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        vr::HmdMatrix34_t pose = Ahead(poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking, distance, up);
        for (int r = 0; r < 3; ++r) pose.m[r][3] += float(side) * pose.m[r][0];
        vr::VROverlay()->SetOverlayTransformAbsolute(ov, vr::TrackingUniverseStanding, &pose);
    }

    // Remote Monitors take a moment to appear; Vibepollo says 503 until then.
    const bool resume = appId != kRemoteMonitor && server.currentGame == appId;
    int code = 0;
    const int64_t launchAt = MonoNs();
    for (int tries = 0; tries < 60 && !g_stop; ++tries) {
        code = Launch(server, cfg, appId, uniqueId, resume, &message);
        if (code != 503) break;
        if (tries == 0) std::printf("launch: %d %s (retrying)\n", code, message.c_str());
        usleep(500'000);
    }
    std::printf("launch took %.1f s\n", Ms(MonoNs() - launchAt) / 1e3);
    std::printf("%s app %d: %d %s\n", resume ? "resume" : "launch", appId, code, message.c_str());
    if (code != 200) return 1;

    g_wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    DECODER_RENDERER_CALLBACKS dr;
    LiInitializeVideoCallbacks(&dr);
    dr.setup = Setup;
    dr.submitDecodeUnit = Submit;
    CONNECTION_LISTENER_CALLBACKS cl;
    LiInitializeConnectionCallbacks(&cl);
    cl.logMessage = Log;
    cl.stageFailed = StageFailed;
    cl.connectionTerminated = Terminated;
    cl.connectionStatusUpdate = Status;
    const int64_t connectAt = MonoNs();
    if (LiStartConnection(&server.serverInfo, &cfg, &cl, &dr, nullptr, nullptr, 0, nullptr, 0) != 0)
        return std::fprintf(stderr, "stream: can't connect\n"), 1;
    std::printf("stream: connected in %.0f ms, %d kbit/s asked, video %sencrypted asked\n", Ms(MonoNs() - connectAt), bitrate,
                encryptVideo ? "" : "not ");

    const int64_t start = MonoNs();
    int64_t lastReport = start, cpu0 = CpuNs(), nextIdr = 0, showAskedNs = 0;
    double hiddenCpu = 0, hiddenSecs = 0, shownCpu = 0, shownSecs = 0;
    uint64_t seq = 0;
    int moves = 0, buttons = 0, scrolls = 0, hits = 0, strays = 0;  // input sent to the host, per report
    Drag *drag = ov != vr::k_ulOverlayHandleInvalid ? SharedDrag() : nullptr;
    if (ov != vr::k_ulOverlayHandleInvalid && !drag) std::fprintf(stderr, "no shared drag state: drags stay on this panel\n");
    const int32_t me = getpid();
    uint32_t held = 0;          // buttons pressed on this panel, by bit
    int hitX = -1, hitY = -1;   // where this panel's hit test last put the host's cursor
    std::map<uint64_t, Unit> inside;  // fed to the decoder, by sequence number (its timestamp)
    vr::SharedTextureHandle_t shownTex = 0;
    vr::Texture_t tex = {&shownTex, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    bool visible = false, hidden = false, shownOnce = false;
    while (!g_stop) {
        const int64_t now = MonoNs();
        const double t = (now - start) / 1e9;
        if (seconds > 0 && t > seconds) break;
        if (hideAt > 0 && !hidden && t >= hideAt && (showAt <= hideAt || t < showAt)) {
            hidden = true, g_hidden = true, nextIdr = now;
            std::printf("%3.0f s: hidden: decoding only IDRs, asking for one a second\n", t);
        }
        if (hidden && showAt > hideAt && t >= showAt) {
            hidden = false, g_hidden = false, g_waitIdr = true, showAskedNs = now;
            LiRequestIdrFrame();
            std::printf("%3.0f s: shown again: asked for an IDR\n", t);
        }
        if (hidden && now >= nextIdr) {
            g_idrAskedNs = now;
            LiRequestIdrFrame();
            nextIdr += 1'000'000'000;
        }
        pollfd p[2] = {{dec.fd, POLLIN | POLLPRI | POLLOUT, 0}, {g_wake, POLLIN, 0}};
        if (poll(p, 2, 20) > 0 && (p[0].revents & POLLERR)) usleep(1000);
        uint64_t n;
        if (read(g_wake, &n, sizeof n) < 0) {}
        if (dec.Events() && !conv.Setup(dec)) break;
        dec.Reclaim();
        for (;;) {
            Unit u;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                if (g_units.empty()) break;
                if (!dec.Feed(g_units.front().data.data(), g_units.front().data.size(), seq)) break;
                u = std::move(g_units.front());
                g_units.pop_front();
            }
            u.data.clear();
            inside[seq++] = std::move(u);
        }
        uint64_t frame = 0;
        bool last = false;
        int i;
        while ((i = dec.Decoded(&frame, &last)) >= 0) {
            const int64_t decodedNs = MonoNs();
            shownTex = conv.Convert(i);
            dec.Requeue(i);
            if (ov != vr::k_ulOverlayHandleInvalid) {
                vr::VROverlay()->SetOverlayTexture(ov, &tex);
                if (!visible) vr::VROverlay()->ShowOverlay(ov), visible = true;
            }
            const int64_t shownNs = MonoNs();
            auto it = inside.find(frame);
            if (it == inside.end()) continue;
            const Unit &u = it->second;
            if (!shownOnce) std::printf("stream: first picture %.0f ms after connecting\n", Ms(shownNs - start)), shownOnce = true;
            std::lock_guard<std::mutex> lock(g_mu);
            ++g_iv.shown;
            const double dms = Ms(decodedNs - u.submitNs), tms = Ms(shownNs - u.receivedNs);
            g_iv.decodeSum += dms, g_iv.decodeMax = std::max(g_iv.decodeMax, dms);
            g_iv.totalSum += tms, g_iv.totalMax = std::max(g_iv.totalMax, tms);
            auto q = g_pendingIdr.find(u.number);
            if (q != g_pendingIdr.end()) {
                g_samples.push_back({Ms(u.receivedNs - q->second.first), Ms(shownNs - q->second.first), q->second.second});
                g_pendingIdr.erase(q);
            }
            if (showAskedNs && u.idr) {
                std::printf("stream: full rate again %.0f ms after asking\n", Ms(shownNs - showAskedNs));
                showAskedNs = 0;
            }
            inside.erase(inside.begin(), std::next(it));
        }
        // Input from the panel to the host: absolute positions in stream pixels (SteamVR's
        // mouse y counts from the bottom), buttons, and wheel notches.
        if (ov != vr::k_ulOverlayHandleInvalid) {
            vr::VREvent_t ev;
            while (vr::VROverlay()->PollNextOverlayEvent(ov, &ev, sizeof ev)) {
                const bool mouse = ev.eventType == vr::VREvent_MouseMove || ev.eventType == vr::VREvent_MouseButtonDown ||
                                   ev.eventType == vr::VREvent_MouseButtonUp;
                // While a button is held, moves keep coming here with coordinates off the panel
                // once the laser has left it: those belong to the panel under the laser now.
                const bool onPanel = mouse && ev.data.mouse.x >= 0 && ev.data.mouse.x < width && ev.data.mouse.y >= 0 &&
                                     ev.data.mouse.y < height;
                const int x = int(ev.data.mouse.x), y = int(height - ev.data.mouse.y);
                if (inputLog) {
                    int hx = -1, hy = -1;
                    if (mouse) HitPanel(ov, ev.trackedDeviceIndex, width, height, &hx, &hy);
                    std::printf("%8.3f %s device %u", (MonoNs() - start) / 1e9,
                                vr::VRSystem()->GetEventTypeNameFromEnum(vr::EVREventType(ev.eventType)), ev.trackedDeviceIndex);
                    if (mouse) std::printf(" at %d,%d%s, hit test %d,%d", x, y, onPanel ? "" : " (off the panel)", hx, hy);
                    std::printf("\n");
                    std::fflush(stdout);
                }
                switch (ev.eventType) {
                    case vr::VREvent_MouseMove:
                        if (onPanel) LiSendMousePositionEvent(short(x), short(y), short(width), short(height)), ++moves;
                        else ++strays;
                        break;
                    case vr::VREvent_MouseButtonDown:
                    case vr::VREvent_MouseButtonUp: {
                        const int button = ev.data.mouse.button == vr::VRMouseButton_Right    ? BUTTON_RIGHT
                                           : ev.data.mouse.button == vr::VRMouseButton_Middle ? BUTTON_MIDDLE
                                                                                               : BUTTON_LEFT;
                        const bool down = ev.eventType == vr::VREvent_MouseButtonDown;
                        // The release also comes to the panel the press was on. Moving the cursor
                        // back here would drop a drag on the wrong display.
                        if (onPanel) LiSendMousePositionEvent(short(x), short(y), short(width), short(height));
                        LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button);
                        held = down ? held | 1u << button : held & ~(1u << button);
                        if (drag && down && held == 1u << button) {
                            drag->device = ev.trackedDeviceIndex;
                            drag->owner = me;
                        } else if (drag && !held) {
                            int32_t mine = me;
                            drag->owner.compare_exchange_strong(mine, 0);
                        }
                        ++buttons;
                        std::printf("input: button %d %s at %d,%d%s\n", button, down ? "down" : "up", x, y,
                                    onPanel ? "" : " (off the panel)");
                        break;
                    }
                    case vr::VREvent_ScrollDiscrete:
                        LiSendHighResScrollEvent(short(ev.data.scroll.ydelta * 120));
                        ++scrolls;
                        break;
                    default:
                        break;
                }
            }
            // Another panel's drag: follow its laser while it's over this panel.
            int32_t owner = drag ? drag->owner.load() : 0;
            if (owner && owner != me && kill(owner, 0) != 0 && errno == ESRCH)
                drag->owner.compare_exchange_strong(owner, 0), owner = 0;  // it died holding the button
            int hx, hy;
            if (owner && owner != me && HitPanel(ov, drag->device, width, height, &hx, &hy)) {
                if (hx != hitX || hy != hitY) {
                    LiSendMousePositionEvent(short(hx), short(hy), short(width), short(height));
                    ++hits;
                    if (inputLog) std::printf("%8.3f drag from %d: hit test %d,%d\n", (MonoNs() - start) / 1e9, owner, hx, hy), std::fflush(stdout);
                }
                hitX = hx, hitY = hy;
            } else {
                hitX = hitY = -1;  // the next hit sends, even at the same spot
            }
        }
        if (now - lastReport >= 2'000'000'000) {
            const double secs = (now - lastReport) / 1e9, cpu = (CpuNs() - cpu0) / 1e9;
            (hidden ? hiddenCpu : shownCpu) += cpu, (hidden ? hiddenSecs : shownSecs) += secs;
            Interval v;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                v = g_iv, g_iv = Interval{};
            }
            uint32_t rtt = 0, rttVar = 0;
            LiGetEstimatedRttInfo(&rtt, &rttVar);
            const RTP_VIDEO_STATS *rs = LiGetRTPVideoStats();
            const int s = std::max(1, v.shown), r = std::max(1, v.received);
            std::printf("%3.0f s: %s in %4.1f fps (%d IDR, %d skipped), shown %4.1f fps, %5.1f Mbit/s | host %.1f ms, "
                        "rtt/2 %.1f ms, receive %.1f, queue %.1f, decode %.1f (worst %.1f), first packet to panel %.1f ms "
                        "(worst %.1f) | lost: %u FEC failed, %u OOS | CPU %.1f%% of a core\n",
                        t, hidden ? "HIDDEN" : "shown ", v.received / secs, v.idrs, v.skipped, v.shown / secs,
                        v.bytes * 8 / secs / 1e6, v.hostN ? v.hostSum / v.hostN : 0.0, rtt / 2.0, v.receiveSum / r,
                        v.queueSum / r, v.decodeSum / s, v.decodeMax, v.totalSum / s, v.totalMax,
                        rs ? rs->packetCountFecFailed : 0, rs ? rs->packetCountOOS : 0, 100 * cpu / secs);
            if (v.idrs) std::printf("      IDR average %.0f KB\n", v.idrBytes / 1024.0 / v.idrs);
            if (moves || buttons || scrolls || hits || strays)
                std::printf("      input: %d moves, %d buttons, %d scrolls; drags: %d moves from the hit test, %d off the panel dropped\n",
                            moves, buttons, scrolls, hits, strays);
            moves = buttons = scrolls = hits = strays = 0;
            std::fflush(stdout);
            lastReport = now, cpu0 = CpuNs();
        }
    }
    const int ended = g_ended;
    g_stop = 1;
    if (drag) {
        int32_t mine = me;
        drag->owner.compare_exchange_strong(mine, 0);
    }
    LiStopConnection();
    if (!g_samples.empty()) {
        double a = 0, aMax = 0, sh = 0, shMax = 0, kb = 0, kbMax = 0;
        for (const Sample &x : g_samples) {
            a += x.arriveMs, aMax = std::max(aMax, x.arriveMs), sh += x.shownMs, shMax = std::max(shMax, x.shownMs);
            kb += x.bytes / 1024.0, kbMax = std::max(kbMax, x.bytes / 1024.0);
        }
        const double k = double(g_samples.size());
        std::printf("hidden: %zu IDR requests answered; request to first packet %.0f ms (worst %.0f), to panel %.0f ms "
                    "(worst %.0f); IDR %.0f KB (largest %.0f)\n",
                    g_samples.size(), a / k, aMax, sh / k, shMax, kb / k, kbMax);
    }
    if (shownSecs > 0) std::printf("CPU shown: %.1f%% of a core\n", 100 * shownCpu / shownSecs);
    if (hiddenSecs > 0) std::printf("CPU hidden: %.1f%% of a core\n", 100 * hiddenCpu / hiddenSecs);
    if (ov != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(ov);
    dec.Close();
    if (!noVr) vr::VR_Shutdown();
    if (!keep && appId != kFrametopDisplay) {  // a display stream holds nothing on the host
        if (appId == kRemoteMonitor) code = Launch(server, cfg, kDisconnectMonitor, uniqueId, false, &message);
        else code = gs_quit_app(&server) == GS_OK ? 200 : -1, message = gs_error ? gs_error : "";
        std::printf("%s: %d %s\n", appId == kRemoteMonitor ? "release monitor" : "quit app", code, message.c_str());
    }
    return ended > 1 ? 1 : 0;
}
