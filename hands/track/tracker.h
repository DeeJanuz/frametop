// Multi-camera hand tracking (a port of frame-hands' Python prototype; the scheduling is
// described in hands/README.md). All 3D is metres in the head frame.
#pragma once

#include "calib.h"
#include "nets.h"

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Runs batches of jobs on a few threads, each pinned to a core.
class Pool {
public:
    // One thread per entry of cpus, pinned there (round-robin if threads > cpus).
    Pool(int threads, const std::vector<int> &cpus);
    ~Pool();
    void run(std::vector<std::function<void()>> &jobs);

private:
    void loop(int cpu);
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable wake_, done_;
    std::vector<std::function<void()>> *jobs_ = nullptr;
    size_t next_ = 0, finished_ = 0;
    bool stop_ = false;
};

struct Hand {
    int id = 0;
    V3 pts[21]{};               // as measured this frame; the tracker steers crops by these
    V3 smooth[21]{};            // filtered (One Euro, see Tracker::smooth): publish these
    bool has_pts = false;
    double residual = -1;       // rms ray distance of the triangulation (m); -1: one view
    int nviews = 0;
    double right_score = 0.5;   // the model's right-hand score (these images aren't mirrored)
    double scale = 1.0;         // this user's hand size / the model's world landmarks
    int64_t seen_ns = 0;
    int frames = 0;
    double speed = 0;           // palm centre, m/s, smoothed
    int64_t last_ns = 0;
    V3 last_palm{};
    V3 dpalm{};                 // the filter's palm velocity, m/s
    int64_t smooth_ns = 0;
    bool right() const { return right_score > 0.5; }
};

struct Stats {
    int palm_calls = 0, hand_calls = 0, sets = 0;
    double palm_ms = 0, hand_ms = 0, step_ms = 0;   // summed batch times
    int palm_batches = 0, hand_batches = 0;
    // why views and hands come and go
    int lost = 0;         // a tracked view's landmarks fell below min presence
    int handoff_miss = 0; // a view projected from the hand's 3D (new camera or retry) found no hand
    int dups = 0;         // the same hand twice in one camera
    int splits = 0;       // a hand's views disagreed in 3D and were split
    int created = 0, merged = 0, forgotten = 0;   // merged: views re-paired across cameras
    // diagnostics: on stereo frames, each view's single-view palm distance / the stereo one
    std::vector<std::pair<int, double>> mono_ratio;   // (hand id, ratio)
};

// A hand the landmark model found in one camera (Tracker::views_now, Tracker::exhaustive).
struct Seen {
    std::string cam;
    int hand = 0;           // the tracker's hand; 0 in exhaustive()
    Roi roi;
    Landmarks lm;
    V3 wrist{};             // exhaustive(): single-view 3D guess at the model's hand size
};

class Tracker {
public:
    Tracker(const std::map<std::string, Camera> &cams, const Nets &nets, Pool &pool, int max_views = 2);
    // images: calibration name -> frame. Returns the hands seen in this set.
    std::vector<const Hand *> step(const std::map<std::string, Image> &images, int64_t t_ns);
    // Seconds until the next frame set is worth processing (30 Hz fast hands, 15 Hz slow, 5 Hz none).
    double interval() const;
    Stats stats;
    size_t views() const { return views_.size(); }
    std::vector<Seen> views_now() const;   // the views this step updated
    // Forget this camera's views, when it stops being tracked with (the lighting switched
    // cameras); views in cameras a set lacks otherwise wait for their next frame.
    void drop_camera(const std::string &name);
    // Every search tile in every camera, then landmarks on every palm: slow; for checking
    // what the scheduler misses (ft-handreplay --oracle).
    std::vector<Seen> exhaustive(const std::map<std::string, Image> &images);
    // Landmark presence a tracked view needs to stay (new views need min presence, 0.5). In
    // bright rooms the camera exposes for the room, the hands come out dim, and presence
    // dips under 0.5 for a frame at a time.
    void set_keep_presence(double p) { keep_presence_ = p; }
    // Misread guards, for fine-tuned landmark models (frame-hands' students): they stay sure of a
    // hand when two hands touch and can read the held hand as the other side, which made a split /
    // hand-over / duplicate loop. On: a reading of an established hand (5+ frames) whose side is
    // more than 0.7 off the hand's own is dropped (the hand-over crops it afresh next frame), and a
    // view split off where its hand already is in that camera is dropped instead of starting a new
    // hand. Off by default: the stock model's side is noisier and the first guard costs it tracking.
    void set_misread_guard(bool on) { misread_guard_ = on; }
    // One view's 3D hand: each landmark along its ray, as far as how big the palm looks says
    // for a hand `scale` times the model's (Hand::scale). False if the palm is degenerate.
    static bool single_view(const Camera &cam, const Landmarks &lm, double scale, V3 out[21]);
    // Cameras a and b's images were under each other's names (the side cameras, track/sides.h):
    // each view moves to the other camera (its crop is in the image's pixels, so it keeps
    // following its hand), and every hand's 3D starts over from its views at the next step.
    void exchange(const std::string &a, const std::string &b);

private:
    struct View {
        const Camera *cam;
        Roi roi;
        int hand = 0;           // 0: not assigned yet
        Landmarks lm;
        bool has_lm = false;
        int frames = 0;
        bool fresh = false;     // lm is from this step
    };
    struct Tile {
        const Camera *cam;
        V2 center;
        double size, rotation, weight, credit = 0;
    };
    void run_landmarks(const std::map<std::string, Image> &images, std::vector<View *> &views);
    bool hand_3d(Hand &hand, std::vector<View *> views, int64_t t_ns);
    double pair_cost(const View &a, const View &b) const;
    double size_misfit(const std::vector<const View *> &views, const V3 *pts) const;
    void associate();
    static void smooth(Hand &h, int64_t t_ns);
    bool inside(const Camera &cam, V2 uv) const;
    void add_tiles(const Camera &cam, double frac, int gx, int gy);

    std::map<std::string, const Camera *> cams_;
    const Nets &nets_;
    Pool &pool_;
    int max_views_, hand_budget_ = 4, search_budget_ = 3;
    double min_presence_ = 0.5, keep_presence_ = 0.5;
    bool misread_guard_ = false;
    std::vector<View> views_;
    std::map<int, Hand> hands_;
    std::vector<Tile> tiles_;
    int next_id_ = 1;
    int64_t last_ns_ = 0, search_ns_ = 0;
};
