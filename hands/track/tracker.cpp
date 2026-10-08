#include "tracker.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <set>

namespace {

// where arms start, head frame: below and slightly behind the eyes
const V3 kShoulders[2] = {{0.17, -0.25, 0.08}, {-0.17, -0.25, 0.08}};
// landmark pairs across the palm, rigid enough for single-view depth
const int kPalmPairs[][2] = {{0, 5}, {0, 9}, {0, 13}, {0, 17}, {5, 17}, {5, 13}, {9, 17}, {1, 17}, {1, 5}};
constexpr double kFastSpeed = 0.25;    // m/s
constexpr double kSearchInterval = 0.2;
// One Euro filter on the published landmarks: still hands are smoothed hard (tracking
// noise is a few mm per frame), fast ones barely, so they don't lag.
constexpr double kMinCutoff = 2.0;     // Hz, a still hand
constexpr double kBeta = 30.0;         // Hz more per m/s of palm speed
constexpr double kSpeedCutoff = 1.5;   // Hz, for the palm speed itself
// With one view, the hand's distance from the camera comes from how big it looks, which
// is off by 10-30% and wanders ~10% between frames. Its direction is exact. So a hand that
// was just located keeps its distance, drifting toward the one-view guess by this much a frame.
constexpr double kMonoDepthGain = 0.1;
// Is a triangulated hand as far from each camera as its apparent size says? With the
// model's average hand, clean stereo pairs measure 0.71-1.51 times the one-view distance
// (5-95%, median 1.16); pairs of two different hands mostly far less.
constexpr double kSizePrior = 1.16, kRatioLo = 0.6, kRatioHi = 1.9;

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

bool is_color(const Camera &c) { return c.name.rfind("color", 0) == 0; }   // Arcturus, 145 degree image circle
// The wide cameras: the side ones and the color ones
bool is_slam(const Camera &c) { return c.name.rfind("slam", 0) == 0 || is_color(c); }

V2 palm_centre(const Landmarks &lm) { return lm.pts[9]; }

// Two views in one camera on the same hand: the landmark model puts the same points on
// it from both crops, even when the crops differ.
bool same_hand(const Landmarks &a, const Landmarks &b, double size) {
    double d = 0;
    for (int i = 0; i < 21; ++i) d += norm(a.pts[i] - b.pts[i]) / 21;
    return norm(palm_centre(a) - palm_centre(b)) < 0.5 * size || d < 0.25 * size;
}

double hand_size(const Landmarks &lm) {
    double lo[2] = {1e9, 1e9}, hi[2] = {-1e9, -1e9};
    for (const V2 &p : lm.pts)
        for (int k = 0; k < 2; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    return std::max(hi[0] - lo[0], hi[1] - lo[1]);
}

}  // namespace

// ---------------------------------------------------------------------------- pool

Pool::Pool(int threads, const std::vector<int> &cpus) {
    for (int i = 0; i < threads; ++i) threads_.emplace_back(&Pool::loop, this, cpus[i % cpus.size()]);
}

Pool::~Pool() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    for (auto &t : threads_) t.join();
}

void Pool::loop(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);   // ignored if not allowed
    std::unique_lock<std::mutex> l(mu_);
    for (;;) {
        wake_.wait(l, [&] { return stop_ || (jobs_ && next_ < jobs_->size()); });
        if (stop_) return;
        auto &job = (*jobs_)[next_++];
        l.unlock();
        job();
        l.lock();
        if (++finished_ == jobs_->size()) done_.notify_all();
    }
}

void Pool::run(std::vector<std::function<void()>> &jobs) {
    if (jobs.empty()) return;
    std::unique_lock<std::mutex> l(mu_);
    jobs_ = &jobs, next_ = 0, finished_ = 0;
    wake_.notify_all();
    done_.wait(l, [&] { return finished_ == jobs.size(); });
    jobs_ = nullptr;
}

// ------------------------------------------------------------------------- tracker

Tracker::Tracker(const std::map<std::string, Camera> &cams, const Nets &nets, Pool &pool, int max_views)
    : nets_(nets), pool_(pool), max_views_(max_views) {
    for (const auto &[name, cam] : cams) {
        cams_[name] = &cam;
        if (is_slam(cam)) {
            add_tiles(cam, 0.45, 3, 3);
            add_tiles(cam, 0.65, 2, 2);
            add_tiles(cam, 1.0, 1, 1);   // hands close to the face fill much of the frame
        } else {
            add_tiles(cam, 0.6, 3, 2);
            add_tiles(cam, 1.0, 1, 1);
        }
    }
}

void Tracker::add_tiles(const Camera &cam, double frac, int gx, int gy) {
    const double s = frac * std::max(cam.width, cam.height);
    for (int i = 0; i < gx; ++i)
        for (int j = 0; j < gy; ++j) {
            const double x = gx > 1 ? s / 2 + (cam.width - s) * i / (gx - 1) : cam.width / 2.0;
            const double y = gy > 1 ? s / 2 + (cam.height - s) * j / (gy - 1) : cam.height / 2.0;
            Tile t{&cam, {x, y}, s, 0, 0};
            // turn the crop so the expected shoulder-to-hand direction points up
            const V3 ray = cam.ray(t.center), p = cam.origin + ray * 0.45;
            const V3 d = unit(p - kShoulders[p[0] > 0 ? 0 : 1]);
            const V2 a = cam.project(p, nullptr), b = cam.project(p + d * 0.05, nullptr);
            t.rotation = std::atan2(b[0] - a[0], -(b[1] - a[1]));
            t.weight = std::max(0.15, dot(ray, unit(V3{0, -0.45, -0.9})));
            tiles_.push_back(t);
        }
}

double Tracker::interval() const {
    double fastest = -1;
    for (const auto &[id, h] : hands_)
        if (h.seen_ns == last_ns_) fastest = std::max(fastest, norm(h.dpalm));   // filtered: noise isn't speed
    return fastest < 0 ? 1 / 5.0 : fastest > kFastSpeed ? 1 / 30.0 : 1 / 15.0;
}

bool Tracker::inside(const Camera &cam, V2 uv) const {
    const double m = 0.12;
    return uv[0] >= m * cam.width && uv[0] <= (1 - m) * cam.width && uv[1] >= m * cam.height &&
           uv[1] <= (1 - m) * cam.height && cam.off_axis(uv) < (is_color(cam) ? 70.0 : is_slam(cam) ? 80.0 : 75.0);
}

void Tracker::run_landmarks(const std::map<std::string, Image> &images, std::vector<View *> &views) {
    if (views.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::function<void()>> jobs;
    for (View *v : views) {
        const Image &img = images.at(v->cam->name);
        jobs.push_back([this, v, &img] {
            v->lm = nets_.landmarks(img, v->roi);
            v->has_lm = true;
            v->fresh = true;
        });
    }
    pool_.run(jobs);
    stats.hand_calls += int(views.size());
    stats.hand_ms += ms_since(t0);
    ++stats.hand_batches;
}

bool Tracker::single_view(const Camera &cam, const Landmarks &lm, double scale, V3 out[21]) {
    V3 rays[21];
    for (int i = 0; i < 21; ++i) rays[i] = cam.ray(lm.pts[i]);
    std::vector<std::pair<double, double>> est;   // (depth, weight)
    for (const auto &pr : kPalmPairs) {
        const int i = pr[0], j = pr[1];
        const double d = std::hypot(lm.world[i][0] - lm.world[j][0], lm.world[i][1] - lm.world[j][1]) * scale;
        const double a = std::acos(std::clamp(dot(rays[i], rays[j]), -1.0, 1.0));
        if (a > 1e-3 && d > 0.01) est.push_back({d / a, d});
    }
    if (est.empty()) return false;
    std::sort(est.begin(), est.end());
    double total = 0, acc = 0, depth = est.back().first;
    for (auto &e : est) total += e.second;
    for (auto &e : est)
        if ((acc += e.second) >= total / 2) { depth = e.first; break; }
    double zmean = 0;
    for (int i = 0; i < 21; ++i) zmean += lm.world[i][2] / 21;
    for (int i = 0; i < 21; ++i) out[i] = cam.origin + rays[i] * (depth + (lm.world[i][2] - zmean) * scale);
    return true;
}

// A triangulated hand is in front of each camera, as far as its apparent size says (see
// kSizePrior). Returns how far off that is (the sum of |log| ratios), or -1 if implausible.
double Tracker::size_misfit(const std::vector<const View *> &views, const V3 *pts) const {
    double misfit = 0;
    for (const View *v : views) {
        V3 mono[21];
        // along the view's own ray (fisheye: a hand near the image edge is far off the axis)
        if (dot(pts[9] - v->cam->origin, v->cam->ray(v->lm.pts[9])) < 0.08) return -1;
        if (!single_view(*v->cam, v->lm, 1.0, mono)) continue;
        const double r = norm(pts[9] - v->cam->origin) / norm(mono[9] - v->cam->origin);
        if (r < kRatioLo || r > kRatioHi) return -1;
        misfit += std::fabs(std::log(r / kSizePrior));
    }
    return misfit;
}

bool Tracker::hand_3d(Hand &hand, std::vector<View *> views, int64_t t_ns) {
    views.erase(std::remove_if(views.begin(), views.end(), [](View *v) { return !v->has_lm || !v->fresh; }), views.end());
    if (views.empty()) return false;
    if (views.size() >= 2) {
        const int n = int(views.size());
        std::vector<V3> origins(n), dirs(n);
        std::vector<double> w(n), res(21);
        V3 pts[21];
        for (int k = 0; k < 21; ++k) {
            for (int v = 0; v < n; ++v) {
                origins[v] = views[v]->cam->origin;
                dirs[v] = views[v]->cam->ray(views[v]->lm.pts[k]);
                w[v] = views[v]->lm.presence;
            }
            pts[k] = triangulate(origins.data(), dirs.data(), w.data(), n, &res[k]);
        }
        std::nth_element(res.begin(), res.begin() + 10, res.end());
        const double residual = res[10];
        // the views disagree: two different hands; keep the stronger. Rays to two different
        // hands can pass close to each other near the cameras, so check the distance too.
        if (residual > 0.03 || size_misfit({views.begin(), views.end()}, pts) < 0) {
            View *best = *std::max_element(views.begin(), views.end(),
                                           [](View *a, View *b) { return a->lm.presence < b->lm.presence; });
            // a view that disagrees but sits where this hand already is in its camera is this hand
            // misread: drop it (-2), and the hand-over gives a fresh crop next frame
            for (View *v : views) {
                if (v == best) continue;
                v->hand = -1;
                if (!misread_guard_ || !hand.has_pts) continue;
                double z;
                const V2 at = v->cam->project(hand.pts[9], &z);
                if (z > 0 && norm(at - palm_centre(v->lm)) < 0.5 * hand_size(v->lm)) v->hand = -2;
            }
            ++stats.splits;
            return hand_3d(hand, {best}, t_ns);
        }
        // learn how big this user's hand is compared to the model's average hand
        std::vector<double> t, m;
        const Landmarks &ref = views[0]->lm;
        for (const auto &pr : kPalmPairs) {
            t.push_back(norm(pts[pr[0]] - pts[pr[1]]));
            m.push_back(norm(V3{ref.world[pr[0]][0], ref.world[pr[0]][1], ref.world[pr[0]][2]} -
                             V3{ref.world[pr[1]][0], ref.world[pr[1]][1], ref.world[pr[1]][2]}));
        }
        std::nth_element(t.begin(), t.begin() + t.size() / 2, t.end());
        std::nth_element(m.begin(), m.begin() + m.size() / 2, m.end());
        if (m[m.size() / 2] > 0 && residual < 0.008)   // only from clean matches
            hand.scale += 0.1 * (std::clamp(t[t.size() / 2] / m[m.size() / 2], 0.8, 1.6) - hand.scale);
        std::copy(pts, pts + 21, hand.pts);
        hand.residual = residual;
        for (View *v : views) {
            V3 mono[21];
            if (!single_view(*v->cam, v->lm, hand.scale, mono)) continue;
            const V3 o = v->cam->origin;
            stats.mono_ratio.push_back({hand.id, norm(mono[9] - o) / norm(pts[9] - o)});
        }
    } else {
        const Camera &cam = *views[0]->cam;
        V3 pts[21];
        if (!single_view(cam, views[0]->lm, hand.scale, pts)) return false;
        const double guess = norm(pts[9] - cam.origin);
        if (hand.has_pts && t_ns - hand.seen_ns < 300'000'000 && guess > 0) {
            const double was = norm(hand.pts[9] - cam.origin), d = was + kMonoDepthGain * (guess - was);
            for (V3 &p : pts) p = cam.origin + (p - cam.origin) * (d / guess);
        }
        std::copy(pts, pts + 21, hand.pts);
        hand.residual = -1;
    }
    hand.has_pts = true;
    hand.nviews = int(views.size());
    for (View *v : views) hand.right_score += 0.2 * (v->lm.right - hand.right_score);
    return true;
}

// How badly two views in different cameras fit one hand: the rays should meet, each view's
// apparent size should match its distance, and the model should call both the same hand
// (left or right). Negative if they can't be one hand. Side by side hands sit on the same
// epipolar lines of the side cameras, so the distance check is what tells them apart.
double Tracker::pair_cost(const View &a, const View &b) const {
    V3 pts[21];
    std::vector<double> res(21);
    for (int k = 0; k < 21; ++k) {
        const V3 o[2] = {a.cam->origin, b.cam->origin};
        const V3 d[2] = {a.cam->ray(a.lm.pts[k]), b.cam->ray(b.lm.pts[k])};
        const double w[2] = {a.lm.presence, b.lm.presence};
        pts[k] = triangulate(o, d, w, 2, &res[k]);
    }
    std::nth_element(res.begin(), res.begin() + 10, res.end());
    if (res[10] > 0.03) return -1;
    const double misfit = size_misfit({&a, &b}, pts);
    return misfit < 0 ? -1 : res[10] / 0.01 + misfit + std::fabs(a.lm.right - b.lm.right);
}

// Which views in two cameras are the same hand: every way of pairing them up (a few views
// each), scored with pair_cost. Keeps the hands' pairing unless another is clearly better,
// then relabels the views, keeping the longer-tracked hand's id.
void Tracker::associate() {
    constexpr double kPairBonus = 2.0, kBetter = 0.3;
    std::vector<const Camera *> cams;
    for (View &v : views_)
        if (std::find(cams.begin(), cams.end(), v.cam) == cams.end()) cams.push_back(v.cam);
    std::sort(cams.begin(), cams.end(), [](const Camera *a, const Camera *b) { return a->name < b->name; });
    for (size_t i = 0; i < cams.size(); ++i)
        for (size_t j = i + 1; j < cams.size(); ++j) {
            std::vector<View *> A, B;
            for (View &v : views_) {
                if (!v.fresh) continue;
                if (v.cam == cams[i]) A.push_back(&v);
                else if (v.cam == cams[j]) B.push_back(&v);
            }
            if (A.empty() || B.empty() || A.size() > 3 || B.size() > 3) continue;
            std::vector<std::vector<double>> c(A.size(), std::vector<double>(B.size()));
            for (size_t x = 0; x < A.size(); ++x)
                for (size_t y = 0; y < B.size(); ++y) c[x][y] = pair_cost(*A[x], *B[y]);
            auto score = [&](const std::vector<int> &m) {   // m[x]: A[x]'s partner in B, or -1
                double s = 0;
                for (size_t x = 0; x < A.size(); ++x)
                    if (m[x] >= 0 && c[x][m[x]] >= 0) s += c[x][m[x]] - kPairBonus;
                return s;
            };
            std::vector<int> cur(A.size(), -1);
            for (size_t x = 0; x < A.size(); ++x)
                for (size_t y = 0; y < B.size(); ++y)
                    if (A[x]->hand == B[y]->hand) cur[x] = int(y);
            std::vector<int> best = cur, m(A.size(), -1);
            double best_score = score(cur);
            const double cur_score = best_score;
            std::function<void(size_t, unsigned)> walk = [&](size_t x, unsigned used) {
                if (x == A.size()) {
                    const double sc = score(m);
                    if (sc < best_score) best_score = sc, best = m;
                    return;
                }
                m[x] = -1;
                walk(x + 1, used);
                for (size_t y = 0; y < B.size(); ++y)
                    if (!(used >> y & 1) && c[x][y] >= 0) {
                        m[x] = int(y);
                        walk(x + 1, used | 1u << y);
                    }
                m[x] = -1;
            };
            walk(0, 0);
            if (best == cur || best_score > cur_score - kBetter) continue;
            auto frames = [&](int id) {
                const auto h = hands_.find(id);
                return h == hands_.end() ? -1 : h->second.frames;
            };
            for (size_t x = 0; x < A.size(); ++x) {
                if (best[x] < 0) continue;
                View *a = A[x], *b = B[best[x]];
                int id = a->hand;
                bool free = true;   // b's hand isn't another A view's
                for (size_t x2 = 0; x2 < A.size(); ++x2) free = free && (x2 == x || A[x2]->hand != b->hand);
                if (free && frames(b->hand) > frames(id)) id = b->hand;
                a->hand = b->hand = id;
            }
            // a B view left unpaired that still shares a hand with an A view starts its own
            for (size_t y = 0; y < B.size(); ++y) {
                if (std::find(best.begin(), best.end(), int(y)) != best.end()) continue;
                bool shared = false;
                for (View *a : A) shared = shared || a->hand == B[y]->hand;
                if (!shared) continue;
                B[y]->hand = next_id_++;
                hands_[B[y]->hand].id = B[y]->hand;
                ++stats.created;
            }
            ++stats.merged;
        }
}

std::vector<const Hand *> Tracker::step(const std::map<std::string, Image> &images, int64_t t_ns) {
    const auto t_step = std::chrono::steady_clock::now();
    ++stats.sets;
    // Views in cameras without a frame in this set wait, as they are, for their camera's
    // next one: the colour cameras run on their own clock, so a set can hold the mono
    // cameras, the colour ones, or both (drop_camera ends them when a camera stops being used).
    std::vector<View> live, waiting;
    for (View &v : views_) {
        (images.count(v.cam->name) ? live : waiting).push_back(v);
        (images.count(v.cam->name) ? live : waiting).back().fresh = false;
    }

    // 1. hand-over: give hands with too few views a crop in other cameras
    for (auto &[id, hand] : hands_) {
        if (!hand.has_pts) continue;
        std::set<std::string> have;
        for (View &v : live)
            if (v.hand == id) have.insert(v.cam->name);
        if (int(have.size()) >= max_views_) continue;
        std::vector<std::pair<double, View>> options;
        for (auto &[name, cam] : cams_) {
            if (have.count(name) || !images.count(name)) continue;
            V2 uv[21];
            bool front = true;
            for (int k = 0; k < 21; ++k) {
                double z;
                uv[k] = cam->project(hand.pts[k], &z);
                front = front && z > 0;
            }
            const V2 centre = (uv[0] + uv[5] + uv[9] + uv[13] + uv[17]) * 0.2;
            if (!front || !inside(*cam, centre)) continue;
            View v{cam, roi_from_points(uv), id, {}, false, 0};
            options.push_back({cam->off_axis(centre), v});
        }
        std::sort(options.begin(), options.end(), [](auto &a, auto &b) { return a.first < b.first; });
        for (size_t k = 0; k < options.size() && int(have.size() + k) < max_views_; ++k) live.push_back(options[k].second);
    }

    // 2. the landmark model on each hand's best views, within budget
    std::map<int, std::vector<View *>> by_hand;
    for (View &v : live) by_hand[v.hand].push_back(&v);
    std::vector<View *> chosen;
    for (auto &[id, vs] : by_hand) {
        std::sort(vs.begin(), vs.end(), [](View *a, View *b) {
            if (a->has_lm != b->has_lm) return a->has_lm;
            return a->cam->off_axis(a->roi.center) < b->cam->off_axis(b->roi.center);
        });
        for (int k = 0; k < int(vs.size()) && k < max_views_; ++k) chosen.push_back(vs[k]);
    }
    std::stable_sort(chosen.begin(), chosen.end(), [](View *a, View *b) { return a->has_lm > b->has_lm; });
    if (int(chosen.size()) > hand_budget_) chosen.resize(hand_budget_);
    run_landmarks(images, chosen);
    std::vector<View *> kept;
    for (View *v : chosen) {
        if (misread_guard_ && v->has_lm) {   // see set_misread_guard
            const auto h = hands_.find(v->hand);
            if (h != hands_.end() && h->second.frames >= 5 && std::fabs(v->lm.right - h->second.right_score) > 0.7) {
                ++stats.lost;
                continue;
            }
        }
        if (v->lm.presence >= (v->frames > 0 ? keep_presence_ : min_presence_)) {
            v->roi = v->lm.next_roi();
            ++v->frames;
            kept.push_back(v);
        } else {
            ++(v->frames > 0 ? stats.lost : stats.handoff_miss);
        }
    }
    // the same hand twice in one camera: keep the more confident
    std::sort(kept.begin(), kept.end(), [](View *a, View *b) { return a->lm.presence > b->lm.presence; });
    std::vector<View> next;
    for (View *v : kept) {
        const double size = hand_size(v->lm);
        bool dup = false;
        for (View &o : next) dup = dup || (o.cam == v->cam && same_hand(o.lm, v->lm, size));
        if (!dup) next.push_back(*v);
        else ++stats.dups;
    }
    views_ = next;
    views_.insert(views_.end(), waiting.begin(), waiting.end());

    // 3. search for missing hands
    std::set<int> tracked;
    for (View &v : views_) tracked.insert(v.hand);
    if (tracked.size() < 2 && (t_ns - search_ns_) / 1e9 >= kSearchInterval - 0.01) {
        search_ns_ = t_ns;
        const int budget = tracked.empty() ? search_budget_ : std::max(1, search_budget_ - 1);
        for (Tile &t : tiles_)
            if (images.count(t.cam->name)) t.credit += t.weight;
        std::vector<Tile *> picked;
        for (int b = 0; b < budget; ++b) {
            Tile *best = nullptr;
            for (Tile &t : tiles_)
                if (images.count(t.cam->name) && std::find(picked.begin(), picked.end(), &t) == picked.end() &&
                    (!best || t.credit > best->credit))
                    best = &t;
            if (!best) break;
            best->credit = 0;
            picked.push_back(best);
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::vector<Palm>> found(picked.size());
        std::vector<std::function<void()>> jobs;
        for (size_t i = 0; i < picked.size(); ++i) {
            Tile *t = picked[i];
            const Image &img = images.at(t->cam->name);
            jobs.push_back([this, t, &img, &found, i] { found[i] = nets_.palms(img, t->center, t->size, t->rotation); });
        }
        pool_.run(jobs);
        stats.palm_calls += int(picked.size());
        stats.palm_ms += ms_since(t0);
        ++stats.palm_batches;
        std::vector<View> fresh;
        for (size_t i = 0; i < picked.size(); ++i)
            for (const Palm &p : found[i]) {
                const Roi roi = p.roi();
                bool near = false;
                for (auto *list : {&views_, &fresh})
                    for (View &v : *list) near = near || (v.cam == picked[i]->cam && norm(v.roi.center - roi.center) < 0.5 * roi.size);
                if (!near) fresh.push_back({picked[i]->cam, roi, 0, {}, false, 0});
            }
        std::vector<View *> ptrs;
        for (View &v : fresh) ptrs.push_back(&v);
        run_landmarks(images, ptrs);
        for (View &v : fresh)
            if (v.lm.presence >= min_presence_) {
                v.roi = v.lm.next_roi();
                v.frames = 1;
                views_.push_back(v);
            }
    }

    // 4. give new views a hand: the nearest existing hand in 3D, else a new one
    for (View &v : views_) {
        if (v.hand > 0 && hands_.count(v.hand)) continue;
        V3 guess[21];
        const bool have_guess = single_view(*v.cam, v.lm, 1.0, guess);
        int best = 0;
        double dist = 0.12;
        for (auto &[id, h] : hands_) {
            if (!h.has_pts) continue;
            bool same_cam = false;
            for (View &o : views_) same_cam = same_cam || (o.hand == id && o.cam == v.cam);
            if (same_cam) continue;
            const double d = have_guess ? norm(h.pts[9] - guess[9]) : 1e9;
            if (d < dist) best = id, dist = d;
        }
        if (!best) {
            best = next_id_++;
            hands_[best].id = best;
            ++stats.created;
        }
        v.hand = best;
    }

    // 5. which views in different cameras are the same hand
    associate();

    // 6. 3D for every hand seen now; forget hands not seen for a while
    std::vector<const Hand *> out;
    for (auto it = hands_.begin(); it != hands_.end();) {
        Hand &h = it->second;
        std::vector<View *> vs;
        for (View &v : views_)
            if (v.hand == h.id) vs.push_back(&v);
        if (!vs.empty() && hand_3d(h, vs, t_ns)) {
            const V3 palm = (h.pts[0] + h.pts[5] + h.pts[9] + h.pts[13] + h.pts[17]) * 0.2;
            if (h.last_ns && t_ns > h.last_ns)
                h.speed += 0.5 * (std::min(norm(palm - h.last_palm) / ((t_ns - h.last_ns) / 1e9), 5.0) - h.speed);
            h.last_ns = t_ns, h.last_palm = palm, h.seen_ns = t_ns;
            ++h.frames;
            smooth(h, t_ns);
            out.push_back(&h);
            ++it;
        } else if (t_ns - h.seen_ns > 300'000'000) {
            it = hands_.erase(it);
            ++stats.forgotten;
        } else {
            ++it;
        }
    }
    // views split off by a failed triangulation start over as new hands next frame, unless they
    // were this hand misread (-2, the misread guard: dropped)
    const size_t before = views_.size();
    views_.erase(std::remove_if(views_.begin(), views_.end(), [](const View &v) { return v.hand == -2; }), views_.end());
    stats.lost += int(before - views_.size());
    for (View &v : views_)
        if (v.hand <= 0) {
            v.hand = next_id_++;
            hands_[v.hand].id = v.hand;
            ++stats.created;
        }
    last_ns_ = t_ns;
    stats.step_ms += ms_since(t_step);
    return out;
}

std::vector<Seen> Tracker::views_now() const {
    std::vector<Seen> out;
    for (const View &v : views_)
        if (v.fresh) out.push_back({v.cam->name, v.hand, v.roi, v.lm, {}});
    return out;
}

void Tracker::exchange(const std::string &a, const std::string &b) {
    if (!cams_.count(a) || !cams_.count(b)) return;
    const Camera *ca = cams_[a], *cb = cams_[b];
    for (View &v : views_) v.cam = v.cam == ca ? cb : v.cam == cb ? ca : v.cam;
    for (auto &[id, h] : hands_) h.has_pts = false, h.frames = 0, h.residual = -1;
}

void Tracker::drop_camera(const std::string &name) {
    views_.erase(std::remove_if(views_.begin(), views_.end(), [&](const View &v) { return v.cam->name == name; }),
                 views_.end());
}

std::vector<Seen> Tracker::exhaustive(const std::map<std::string, Image> &images) {
    std::vector<Tile *> tiles;
    for (Tile &t : tiles_)
        if (images.count(t.cam->name)) tiles.push_back(&t);
    std::vector<std::vector<Palm>> found(tiles.size());
    std::vector<std::function<void()>> jobs;
    for (size_t i = 0; i < tiles.size(); ++i)
        jobs.push_back([this, &tiles, &images, &found, i] {
            const Tile *t = tiles[i];
            found[i] = nets_.palms(images.at(t->cam->name), t->center, t->size, t->rotation);
        });
    pool_.run(jobs);
    // one crop per palm: tiles overlap, so the same palm turns up several times
    std::vector<std::pair<double, View>> palms;
    for (size_t i = 0; i < tiles.size(); ++i)
        for (const Palm &p : found[i]) palms.push_back({p.score, View{tiles[i]->cam, p.roi(), 0, {}, false, 0}});
    std::sort(palms.begin(), palms.end(), [](auto &a, auto &b) { return a.first > b.first; });
    std::vector<View> crops;
    for (auto &[score, v] : palms) {
        bool near = false;
        for (View &o : crops) near = near || (o.cam == v.cam && norm(o.roi.center - v.roi.center) < 0.5 * v.roi.size);
        if (!near) crops.push_back(v);
    }
    std::vector<View *> ptrs;
    for (View &v : crops) ptrs.push_back(&v);
    run_landmarks(images, ptrs);
    std::sort(crops.begin(), crops.end(), [](const View &a, const View &b) { return a.lm.presence > b.lm.presence; });
    std::vector<Seen> out;
    for (View &v : crops) {
        if (v.lm.presence < min_presence_) continue;
        bool dup = false;
        for (const Seen &o : out)
            dup = dup || (o.cam == v.cam->name && norm(palm_centre(o.lm) - palm_centre(v.lm)) < 0.5 * hand_size(v.lm));
        if (dup) continue;
        V3 pts[21];
        Seen s{v.cam->name, 0, v.roi, v.lm, {}};
        if (single_view(*v.cam, v.lm, 1.0, pts)) s.wrist = pts[0];
        out.push_back(s);
    }
    return out;
}

void Tracker::smooth(Hand &h, int64_t t_ns) {
    const double dt = (t_ns - h.smooth_ns) / 1e9;
    h.smooth_ns = t_ns;
    if (h.frames <= 1 || dt <= 0 || dt > 0.3) {   // new, or back after a gap: start over
        std::copy(h.pts, h.pts + 21, h.smooth);
        h.dpalm = {0, 0, 0};
        return;
    }
    auto alpha = [dt](double cutoff) { return 1 / (1 + 1 / (2 * M_PI * cutoff * dt)); };
    auto palm = [](const V3 *p) { return (p[0] + p[5] + p[9] + p[13] + p[17]) * 0.2; };
    const V3 d = (palm(h.pts) - palm(h.smooth)) * (1 / dt);
    h.dpalm = h.dpalm + (d - h.dpalm) * alpha(kSpeedCutoff);
    // one cutoff for the whole hand, from its palm speed, so its shape stays together
    const double a = alpha(kMinCutoff + kBeta * norm(h.dpalm));
    for (int i = 0; i < 21; ++i) h.smooth[i] = h.smooth[i] + (h.pts[i] - h.smooth[i]) * a;
}
