// Hand cutouts: where a tracked hand is between an eye and a screen, that eye sees the
// room (Room View) through the screen instead of the screen drawn over the hand.
//
// ft-hands (hands/) publishes the hands it sees with the headset's cameras to
// /run/user/UID/frametop-hands/hands (hands/include/fh_hands.h):
// capsules (finger bones, palm, forearm) in the head frame at capture time. Hands turns
// them into the room with the head pose at that time. Project() finds where each eye
// sees them on a panel, and Renderer draws the panel's client buffer into a side-by-side
// buffer (left eye | right eye) with those spots transparent. A panel shows that buffer,
// with the overlay's SideBySide_Parallel flag, only while a hand is in front of it.
//
// The hands arrive some 30-60 ms after the cameras saw them, and show up on the displays
// later still, so Hands moves each hand ahead along its velocity in the room to where it
// will be when the frame reaches the eyes.
#pragma once

#include "vr.h"

#include <openvr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace handcut {

using Mat = vr::HmdMatrix34_t;

struct Capsule {         // in the room (standing universe), metres
    float a[3], b[3];
    float ra, rb;
};

struct Capsule2D {       // on a panel's texture, pixels from the top left
    float ax, ay, bx, by;
    float ra, rb;
};

// A panel in the room: pose of its centre (+x right, +y up, +z out of its front),
// size in metres, cylinder radius (0: flat), and its texture size in pixels.
struct Panel {
    Mat pose;
    double width, height, curve;
    int pxWidth, pxHeight;
};

class Hands {
public:
    // Once per tick: the head pose now, CLOCK_MONOTONIC ns. Re-reads the hands file when
    // it changed. Returns true while fresh hands are known.
    bool Update(const Mat &head, int64_t nowNs);
    // Where the hands will be `lead` after now (see SetPrediction).
    const std::vector<Capsule> &capsules() const { return world_; }
    // Predict the hands' motion (on by default) to now + leadMs: about how long a frame
    // takes from here to the displays.
    void SetPrediction(bool on, double leadMs);
    bool predicting() const { return predict_; }
    double leadMs() const { return leadNs_ / 1e6; }

private:
    bool Read();
    Mat HeadAt(int64_t ns) const;
    struct Past { int64_t ns; Mat head; };
    struct Motion { int64_t ns = 0; double palm[3]{}, v[3]{}; };   // a hand's palm, in the room
    std::vector<Past> history_;   // the last second of head poses
    std::vector<Capsule> base_;   // the capsules at capture time, in the room
    std::vector<int> owner_;      // each capsule's hand (index into ids_), or -1
    std::vector<uint32_t> ids_;   // the hands in the file
    std::map<uint32_t, Motion> motion_;
    std::vector<Capsule> world_;  // base_, moved ahead
    bool predict_ = true;
    int64_t leadNs_ = 36'000'000;   // 25 ms to the displays, plus the tick a cutout buffer waits for its fence
    int fd_ = -1;
    const void *map_ = nullptr;
    uint64_t seq_ = 0;
    int64_t captureNs_ = 0, publishNs_ = 0, lastOpenTry_ = 0;
};

// Where each eye sees the capsules on the panel, for those in front of it. Eyes are
// positions in the room. False if no capsule reaches the panel for either eye.
bool Project(const Panel &p, const std::vector<Capsule> &caps, const double eyes[2][3],
             std::vector<Capsule2D> out[2]);

// The eye positions in the room for a head pose.
void EyePositions(const Mat &head, double out[2][3]);

// An output buffer: the dmabuf SteamVR imports (see vr.cpp), stable while it exists.
struct Output {
    ft_dmabuf buf{};
    void *bo = nullptr;
    unsigned fbo = 0, rb = 0;
    void *image = nullptr;
    // What's drawn in it: the client buffer and its frame, and the cutouts (a later draw
    // with the same frame only redraws around the old and new cutouts).
    void *fence = nullptr;        // the GPU is still drawing it
    const void *key = nullptr;
    uint64_t serial = 0;
    bool drawn = false;
    std::vector<Capsule2D> spots[2];
};

// Composite's counts since the last TakeStats.
struct CutStats {
    int draws = 0, partial = 0;   // buffers drawn, of them only around the cutouts
    int same = 0;                 // nothing changed: the newest buffer stays
    int busy = 0;                 // the GPU hadn't finished the last one: drawn next tick
    int waits = 0;                // a panel's first buffer, waited for
    double cpuMs = 0, worstMs = 0;
};

class Renderer {
public:
    ~Renderer();
    // modifiers: what SteamVR takes for DRM_FORMAT_ABGR8888, the outputs' format.
    // released: an output is about to be freed (drop its SteamVR import).
    bool Init(const std::vector<uint64_t> &modifiers, std::function<void(const Output *)> released);
    // Draw client buffer `src` (identified by `key`; `serial` counts its frames) into a
    // buffer of panel `panel`, both eyes, cutting out `eyes`. Returns the newest buffer the
    // GPU has finished, or null.
    //
    // It doesn't wait for the GPU: a buffer is drawn, fenced, and returned from a later call
    // once the fence has passed, so what SteamVR shows is a tick behind. Each panel has three
    // buffers: the one shown, the one shown before it (SteamVR may still be reading it), and
    // the one being drawn. Nothing is drawn when the frame and the cutouts are what the newest
    // buffer has, and when only the cutouts moved, a buffer that holds the same client frame
    // is drawn again only around them. The first call after a pause (no call for 30 ms, about
    // 3 ticks: the panel showed its client buffer meanwhile) waits for its buffer, so a stale
    // one never shows.
    const Output *Composite(int panel, const void *key, uint64_t serial, const ft_dmabuf &src,
                            const std::vector<Capsule2D> eyes[2]);
    // A client buffer is going away.
    void Forget(const void *key);
    // A panel is gone: drop its outputs.
    void DropPanel(int panel);
    // How long the last Composite took on the CPU, ms.
    double lastMs() const { return lastMs_; }
    CutStats TakeStats() { CutStats s = stats_; stats_ = {}; return s; }

private:
    unsigned Texture(const void *key, const ft_dmabuf &src);
    bool MakeOutput(Output &o, int w, int h);
    void FreeOutput(Output &o);
    bool Passed(Output &o, int64_t timeoutNs);
    void Draw(Output &o, unsigned tex, int w, int h, const std::vector<Capsule2D> eyes[2], bool partial);
    bool ready_ = false;
    int drm_ = -1;
    void *gbm_ = nullptr, *dpy_ = nullptr, *ctx_ = nullptr;
    unsigned copyProg_ = 0, cutProg_ = 0, vbo_ = 0;
    std::vector<uint64_t> modifiers_;
    std::function<void(const Output *)> released_;
    struct Imported { void *image; unsigned tex; };
    std::map<const void *, Imported> imported_;
    struct Ring {
        Output out[3];
        int shown = -1, before = -1, drawing = -1;   // indices into out
        int w = 0, h = 0;
        int64_t lastCall = 0;                        // steady clock ns
    };
    std::map<int, Ring> rings_;
    double lastMs_ = 0;
    CutStats stats_;
};

}  // namespace handcut
