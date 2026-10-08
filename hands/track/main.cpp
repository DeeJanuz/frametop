// ft-hands: hands in 3D from ft-camd's ring, published for Frametop's ft-screens (the hand
// cutouts), and pinches and grips for the pointer. It started as a port of frame-hands'
// Python prototype: the same scheduling, with the models on a few threads.
//
//   ft-hands [--seconds N] [--threads N] [--int8] [--status S] [--models DIR] [--nice N]
//            [--no-publish] [--no-gestures] [--record DIR] [--sides auto|0|1] [--cams auto|mono|color|all] ...
//            (--help lists them all)
//
// --no-gestures: hands for the cutouts only. No pinch or grip detection, so nothing reaches
// the pointer and a closing hand doesn't raise the rate; the gestures file is removed.
//
// Which cameras (--cams, HANDS_CAMERAS): the four mono IR cameras light the hands with their
// own IR and track well in dim rooms, but in bright light (a sunny room, a window behind the
// hands) they expose for the room and the hands come out dark. The two Arcturus colour
// cameras (the passthrough pair, forward-facing, 145 degrees) are the other way round: dark
// and grainy in a dim room, clear in a lit one. auto (the default) picks by how bright the
// colour cameras' frames are: at HANDS_BRIGHT_ON (mean luma) or over for 2 s, it tracks with
// HANDS_BRIGHT (all: every camera, so hands low at the sides stay in the side cameras; or
// color); under HANDS_BRIGHT_OFF for 2 s, with the mono cameras again. ft-camd runs the colour
// cameras at 2 fps, enough to tell the light, until ft-hands asks for 30
// (/run/user/UID/frametop-hands/color-fps). The colour frames' capture times are on their
// own clock, so they're placed on the mono cameras' by when they were dequeued, less the
// mono cameras' measured delay.
//
// Which side camera is which (--sides, HANDS_SWAP_SIDES): the names come from XRService's log
// (cameras_from_xrservice_log) and ft-camd matches each camera's buffers exactly (VIDIOC_QUERYBUF),
// so they should be right; before 2026-10-05 they were often swapped. auto (the default) still
// tells from the hands it tracks (track/sides.h): once it's sure, it exchanges the two
// cameras if they're backwards (the tracked views move with their images), and checks once
// more. 0 and 1 force the naming (1: exchanged; --swap-sides is --sides 1); it still checks,
// and if the hands disagree it warns and publishes what the hands say as the truth ("swapped"),
// so recordings are labelled right while tracking keeps the forced names. The decision is published in /run/user/UID/frametop-hands/sides.json (see
// write_sides below) and, for recordings, in DIR/sides.json. --record-only can't tell (it tracks
// nothing): under auto it records the ring's names as they are.
//
// Settings in ~/.config/frametop.conf (FT_<name> in the environment overrides them, and
// options override both): HANDS_SWAP_SIDES (auto, 0 or 1), HANDS_CPUS (as --cpus),
// HANDS_CAMERAS, HANDS_BRIGHT, HANDS_BRIGHT_ON, HANDS_BRIGHT_OFF, HANDS_COLOR_LEFT (which
// colour camera is passthrough_left: color_video0 or color_video3), HANDS_COLOR_CROP
// (subtract or none: tools/check_color.py tells both), HANDS_MISREAD_GUARD (0 or 1: the
// tracker's guards for fine-tuned landmark models, Tracker::set_misread_guard).
#include "io.h"
#include "pinch.h"
#include "record.h"
#include "sides.h"

#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <memory>

#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <thread>
#include <utility>

namespace {

volatile std::sig_atomic_t g_stop = 0, g_record = 0;

// Which calibrated camera each video device carries. XRService names each tracking camera's
// sensor subdev once per instance ("Found camera 'slam_left': interface=msm_csiphy0
// v4l_subdev=/dev/v4l-subdev30 ...") and logs the device and subdev each index opened at every
// camera start ("TrackingCameraInit: index: 0. video device: /dev/video9. v4l subdevice:
// /dev/v4l-subdev31"). The index is only the order it opens them in: on every start logged since
// 2026-10-04, index 0 was slam_right. So the subdev names the device; a log without "Found
// camera" lines falls back to the index order (slam_left, slam_right, upper_left, upper_right).
// The devices depend on the colour module: with it, the side cameras are on vfe3 (slam_right)
// and vfe4 (slam_left) and the upper pair on vfe2; without it, XRService runs the side cameras
// through the ISP on vfe0 and vfe1, and the upper pair on vfe3 and vfe4. So the running
// XRService's log decides; when it can't be read, the capture pipes as they are with the module.
// {} if the log has no cameras.
std::map<int, std::string> cameras_from_xrservice_log() {
    static const char *const names[] = {"slam_left", "slam_right", "upper_left", "upper_right"};
    const char *home = std::getenv("HOME");
    std::ifstream in(std::string(home ? home : "") + "/.local/share/Steam/logs/xrservice.txt");
    const std::string key = "TrackingCameraInit: index: ", found_key = "Found camera '";
    std::map<int, std::pair<int, std::string>> init;   // index -> (N of /dev/videoN, subdev), latest start
    std::map<std::string, std::string> name_of;        // subdev -> camera name
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("XRService logging to") != std::string::npos) init.clear(), name_of.clear();
        if (const auto at = line.find(found_key); at != std::string::npos) {
            const auto name_end = line.find('\'', at + found_key.size());
            const auto sub = line.find("v4l_subdev=", at);
            if (name_end != std::string::npos && sub != std::string::npos) {
                const auto sub_end = line.find_first_of(" \t\r", sub + 11);
                name_of[line.substr(sub + 11, sub_end == std::string::npos ? std::string::npos : sub_end - sub - 11)] =
                    line.substr(at + found_key.size(), name_end - at - found_key.size());
            }
            continue;
        }
        const auto at = line.find(key);
        int index = -1, node = -1;
        char subdev[64] = "";
        if (at != std::string::npos &&
            std::sscanf(line.c_str() + at + key.size(), "%d. video device: /dev/video%d. v4l subdevice: %63s", &index,
                        &node, subdev) >= 2 &&
            index >= 0 && index < 4)
            init[index] = {node, subdev};
    }
    std::map<int, std::string> out;
    for (auto &[index, ns] : init) {
        const auto it = name_of.find(ns.second);
        const bool known = it != name_of.end() && std::find(std::begin(names), std::end(names), it->second) != std::end(names);
        out[ns.first] = known ? it->second : names[index];
    }
    return out;
}

const char *camera_for_pipe(int node) {
    char path[64], name[64] = "";
    std::snprintf(path, sizeof path, "/sys/class/video4linux/video%d/name", node);
    std::ifstream f(path);
    f.getline(name, sizeof name);
    if (!std::strcmp(name, "msm_vfe3_video0")) return "slam_right";
    if (!std::strcmp(name, "msm_vfe4_video0")) return "slam_left";
    if (!std::strcmp(name, "msm_vfe2_video0")) return "upper_left";
    if (!std::strcmp(name, "msm_vfe2_video1")) return "upper_right";
    return nullptr;
}

// A setting from ~/.config/frametop.conf, or FT_<key> from the environment; "" if unset.
std::string setting(const std::string &key) {
    if (const char *v = std::getenv(("FT_" + key).c_str())) return v;
    const char *home = std::getenv("HOME");
    std::ifstream in(std::string(home ? home : "") + "/.config/frametop.conf");
    std::string line, value;
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\"'"));
        s.erase(s.find_last_not_of(" \t\"'") + 1);
        return s;
    };
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        const auto eq = line.find('=');
        if (eq != std::string::npos && trim(line.substr(0, eq)) == key) value = trim(line.substr(eq + 1));
    }
    return value;
}

std::vector<int> parse_cpus(const char *p) {
    std::vector<int> out;
    while (*p) {
        char *end;
        const long c = std::strtol(p, &end, 10);
        if (end == p) break;
        out.push_back(int(c));
        p = *end == ',' ? end + 1 : end;
    }
    return out;
}

// Where SIGUSR1 puts recordings: $XDG_DATA_HOME/frametop/hands (~/.local/share/...).
std::string recordings_dir() {
    const char *data = std::getenv("XDG_DATA_HOME"), *home = std::getenv("HOME");
    std::string dir = data && *data ? data : std::string(home ? home : "") + "/.local/share";
    for (const char *part : {"/frametop", "/hands"}) mkdir((dir += part).c_str(), 0700);
    return dir;
}

double cpu_seconds() {
    rusage r;
    getrusage(RUSAGE_SELF, &r);
    return r.ru_utime.tv_sec + r.ru_stime.tv_sec + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1e6;
}

enum class Cams { Mono, Color, All };

const char *cams_name(Cams c) { return c == Cams::Mono ? "mono" : c == Cams::Color ? "color" : "all"; }

bool parse_cams(const std::string &s, Cams &out) {
    if (s == "mono") out = Cams::Mono;
    else if (s == "color") out = Cams::Color;
    else if (s == "all") out = Cams::All;
    else return false;
    return true;
}

// How bright it is, for auto (see the top): the colour frames' mean luma, smoothed over about
// a second, with hysteresis and a 2 s hold each way. No colour frames for 3 s (ft-camd paused
// them, or has none) reads as dim.
struct Lighting {
    double on = 40, off = 25;
    double level = -1;
    bool bright = false;
    uint64_t at_ns = 0, since_ns = 0;   // the last frame; since when it's wanted the other way

    void add(double mean, uint64_t t_ns) {
        const double dt = at_ns && t_ns > at_ns ? (t_ns - at_ns) / 1e9 : 1.0;
        level = level < 0 ? mean : level + (mean - level) * std::min(1.0, dt / 1.0);
        at_ns = t_ns;
    }
    // True when it switched.
    bool update(uint64_t now_ns) {
        if (level >= 0 && now_ns - at_ns > 3'000'000'000ull) level = -1;
        const bool want = level >= 0 && (bright ? level > off : level >= on);
        if (want == bright) return since_ns = 0, false;
        if (!since_ns) since_ns = now_ns;
        if (now_ns - since_ns < 2'000'000'000ull) return false;
        bright = want, since_ns = 0;
        return true;
    }
};

// Writes path through a temporary file, so a reader never sees half of it.
bool write_file(const std::string &path, const std::string &text) {
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "w");
    if (!f) return false;
    const bool ok = std::fputs(text.c_str(), f) >= 0;
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) return unlink(tmp.c_str()), false;
    return true;
}

std::string json_bool(std::optional<bool> b) { return !b ? "null" : *b ? "true" : "false"; }
std::string json_str(const std::string &s) { return s.empty() ? "null" : "\"" + s + "\""; }

}  // namespace

int main(int argc, char **argv) {
    double seconds = 0, status = 5;
    int threads = 3, niceness = 5;
    bool int8 = false, publish = true, track = true, gestures_on = true;
    std::string models = std::string(argv[0]).substr(0, std::string(argv[0]).rfind('/') + 1) + "../models/ncnn";
    std::string record, ring_path = "/run/user/" + std::to_string(getuid()) + "/" FH_RING_NAME;
    // SteamOS starts user processes on CPUs 0-4 and keeps 5-7 (two A720s and the X4) for
    // SteamVR's compositor, whose threads there run at real-time priority, so they always
    // win. XRService pins its head tracking to 2-3. frame-hands' probes/core_ab.py
    // (2026-09-29, headset on, 3 rounds): on 5-7 a step took 8.4 ms against 13.2 on 2-4,
    // latency 9.6 against 14.1 ms, and the compositor's late frames and CPU/GPU time didn't change.
    std::vector<int> cpus = {5, 6, 7};
    if (const auto c = parse_cpus(setting("HANDS_CPUS").c_str()); !c.empty()) cpus = c;
    // Which side camera is which (see the top): auto, 0 or 1, and where that came from
    std::string sides_mode = setting("HANDS_SWAP_SIDES"), sides_from = "config";
    if (sides_mode.empty()) sides_mode = "auto", sides_from = "default";
    // Which cameras (see the top).
    std::string cams_arg = setting("HANDS_CAMERAS"), bright_arg = setting("HANDS_BRIGHT");
    std::string color_left = setting("HANDS_COLOR_LEFT"), color_crop = setting("HANDS_COLOR_CROP");
    if (cams_arg.empty()) cams_arg = "auto";
    if (bright_arg.empty()) bright_arg = "all";
    if (color_left.empty()) color_left = "color_video0";
    if (color_crop.empty()) color_crop = "subtract";
    Lighting light;
    if (const std::string v = setting("HANDS_BRIGHT_ON"); !v.empty()) light.on = std::atof(v.c_str());
    if (const std::string v = setting("HANDS_BRIGHT_OFF"); !v.empty()) light.off = std::atof(v.c_str());
    // How crops are equalized. CLAHE helps the palm search find hands (about 10% more in the
    // dim recording), but makes the landmarks jitter, so they get plain crops.
    Contrast palm_contrast, hand_contrast{Contrast::None};
    double keep_presence = 0.5;   // landmark presence a tracked view needs to stay
    bool misread_guard = setting("HANDS_MISREAD_GUARD") == "1";
    PinchParams pinch_params;
    GripParams grip_params;
    bool gesture_log = false;   // what the pinch and grip detectors measure, 10 times a second
    double record_for = 120, record_hz = 0;   // record_hz: at most this many sets a second (0: all)
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--seconds" && more) seconds = std::atof(argv[++i]);
        else if (a == "--threads" && more) threads = std::max(1, std::atoi(argv[++i]));
        else if (a == "--status" && more) status = std::atof(argv[++i]);
        else if (a == "--models" && more) models = argv[++i];
        else if (a == "--nice" && more) niceness = std::atoi(argv[++i]);
        else if (a == "--int8") int8 = true;
        else if (a == "--no-publish") publish = false;
        else if (a == "--no-gestures") gestures_on = false;
        else if (a == "--pinch-begin" && more) pinch_params.begin_m = std::atof(argv[++i]);
        else if (a == "--pinch-end" && more) pinch_params.end_m = std::atof(argv[++i]);
        else if (a == "--pinch-triangulated") pinch_params.triangulated = true;
        else if (a == "--pinch-palm-down" && more) pinch_params.palm_down_max = std::atof(argv[++i]);
        else if (a == "--grip-begin" && more) grip_params.begin = std::atof(argv[++i]);
        else if (a == "--grip-end" && more) grip_params.end = std::atof(argv[++i]);
        else if (a == "--gesture-log") gesture_log = true;
        else if (a == "--swap-sides") sides_mode = "1", sides_from = "option";
        else if (a == "--sides" && more) sides_mode = argv[++i], sides_from = "option";
        else if (a == "--record-only") track = publish = false;
        else if (a == "--ring" && more) ring_path = argv[++i];
        else if (a == "--record" && more) record = argv[++i];
        else if (a == "--record-for" && more) record_for = std::atof(argv[++i]);
        else if (a == "--record-hz" && more) record_hz = std::max(0.0, std::atof(argv[++i]));
        else if (a == "--keep-presence" && more) keep_presence = std::atof(argv[++i]);
        else if (a == "--misread-guard" && more) misread_guard = std::string(argv[++i]) == "1";
        else if (a == "--cams" && more) cams_arg = argv[++i];
        else if (a == "--bright" && more) bright_arg = argv[++i];
        else if (a == "--bright-on" && more) light.on = std::atof(argv[++i]);
        else if (a == "--bright-off" && more) light.off = std::atof(argv[++i]);
        else if (a == "--color-left" && more) color_left = argv[++i];
        else if (a == "--color-crop" && more) color_crop = argv[++i];
        else if (a == "--contrast" && more) {
            if (!Contrast::parse_pair(argv[++i], palm_contrast, hand_contrast))
                return std::fprintf(stderr, "--contrast MODE or PALM/HAND, each clahe[:CLIP]|none|stretch\n"), 1;
        } else if (a == "--cpus" && more) {
            cpus = parse_cpus(argv[++i]);
            if (cpus.empty()) cpus = {5, 6, 7};
        }
        else {
            std::printf("usage: %s [--seconds N] [--threads N] [--int8] [--status S] [--models DIR] [--nice N] [--no-publish]\n"
                        "          [--no-gestures] (hands for the cutouts only: no pinches or grips)\n"
                        "          [--record DIR] [--record-for S] [--record-hz N] [--record-only] [--cpus 5,6,7]\n"
                        "          [--sides auto|0|1] (auto: tell from the hands which side camera is which; 1: exchange them,\n"
                        "          as --swap-sides; 0: as ft-camd names them)\n"
                        "          [--keep-presence P] (0.5) [--ring PATH] (ft-camd's, or ft-ringplay's)\n"
                        "          [--misread-guard 0|1] (0; 1 for fine-tuned landmark models: see Tracker::set_misread_guard)\n"
                        "          [--cams auto|mono|color|all] (auto) [--bright all|color] (all) [--bright-on L] (40) [--bright-off L] (25)\n"
                        "          [--color-left color_video0|color_video3] [--color-crop subtract|none]\n"
                        "          [--pinch-begin M] (0.020) [--pinch-end M] (0.035) [--pinch-triangulated] [--pinch-palm-down MAX] (1: off)\n"
                        "          [--grip-begin R] (1.2) [--grip-end R] (1.45) [--gesture-log]\n"
                        "          [--contrast MODE|PALM/HAND] (clahe[:CLIP], none, stretch; default clahe:2/none)\n"
                        "Recording saves every frame set (at most N a second with --record-hz) for S seconds (120) to DIR/sets.bin,\n"
                        "for ft-handreplay; SIGUSR1\n"
                        "starts one in ~/.local/share/frametop/hands/rec-<time>. --record-only records without tracking, so it\n"
                        "can run beside a tracking ft-hands. With ft-camd --with-dark, recordings also get each\n"
                        "camera's newest dark frame, as <name>_dk; with --with-color, the color cameras' as color_video<N>.\n"
                        "auto picks the cameras by the light (see the top of track/main.cpp).\n"
                        "Settings in ~/.config/frametop.conf: HANDS_SWAP_SIDES=auto|0|1, HANDS_CPUS=5,6,7, HANDS_CAMERAS, HANDS_BRIGHT,\n"
                        "HANDS_BRIGHT_ON, HANDS_BRIGHT_OFF, HANDS_COLOR_LEFT, HANDS_COLOR_CROP, HANDS_MISREAD_GUARD=0|1\n"
                        "(FT_<name> overrides).\n",
                        argv[0]);
            return a == "--help" ? 0 : 1;
        }
    }
    const bool automatic = cams_arg == "auto";
    Cams fixed = Cams::Mono, bright_cams = Cams::All;
    if ((!automatic && !parse_cams(cams_arg, fixed)) || !parse_cams(bright_arg, bright_cams) || bright_cams == Cams::Mono)
        return std::fprintf(stderr, "--cams auto|mono|color|all, --bright all|color\n"), 1;
    if (color_crop != "subtract" && color_crop != "none") return std::fprintf(stderr, "--color-crop subtract|none\n"), 1;
    if (sides_mode != "auto" && sides_mode != "0" && sides_mode != "1")
        return std::fprintf(stderr, "--sides (HANDS_SWAP_SIDES) auto, 0 or 1, not %s\n", sides_mode.c_str()), 1;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);   // whole lines to the journal as they come
    if (nice(niceness) < 0) std::perror("nice");   // the VR stack wins contested CPUs
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    std::signal(SIGUSR1, [](int) { g_record = 1; });

    std::string err;
    std::map<std::string, Camera> calib;
    Ring ring;
    Nets nets;
    Publisher pub;
    GesturePublisher gestures;
    Pinch pinch(pinch_params);
    Grip grip(grip_params);
    std::unique_ptr<Recorder> rec;
    uint64_t rec_start = 0, rec_next_ns = 0;   // rec_next_ns: --record-hz's next set, capture clock
    // The side cameras (see the top). names_swapped: slam_left's and slam_right's ring cameras
    // are exchanged from ft-camd's naming. truth: whether ft-camd's naming is backwards, once known.
    bool names_swapped = sides_mode == "1";
    std::optional<bool> truth;
    std::string decided_by, sides_state = "deciding", decision_evidence;
    double decided_after_s = -1;
    if (sides_mode != "auto") truth = names_swapped, decided_by = sides_from, sides_state = "forced";
    std::string rec_dir;
    std::string cams_json;   // which device each calibrated camera is (set below), for the sides files
    std::vector<std::pair<size_t, bool>> rec_names;   // from which recorded set on, names_swapped was what
    // DIR/sides.json beside a recording's sets.bin: how its side cameras are named. A set's
    // names are right when its names_swapped equals swapped (null: not known when recorded).
    auto write_rec_sides = [&] {
        if (!rec) return;
        std::string runs;
        for (auto &[from, sw] : rec_names) runs += (runs.empty() ? "" : ", ") + ("[" + std::to_string(from) + ", " + json_bool(sw) + "]");
        write_file(rec_dir + "/sides.json",
                   "{\"swapped\": " + json_bool(truth) + ", \"decided_by\": " + json_str(truth ? decided_by : "") +
                       ", \"names_swapped\": [" + runs + "]" +
                       (decision_evidence.empty() ? "" : ", \"evidence\": " + decision_evidence) +
                       (cams_json.empty() ? "" : ", \"cameras\": " + cams_json) + "}\n");
    };
    auto start_recording = [&](const std::string &dir, std::string &e) {
        rec = std::make_unique<Recorder>();
        if (!rec->open(dir, e)) return rec.reset(), false;
        rec_dir = dir, rec_names = {{0, names_swapped}};
        write_rec_sides();
        rec_start = mono_ns();
        std::printf("recording to %s for %.0f s\n", dir.c_str(), record_for);
        std::fflush(stdout);
        return true;
    };
    if (!load_calibration(calib, err) || !ring.open(ring_path.c_str(), err) || !nets.load(models, int8, err) ||
        (publish && (!pub.open(err) || (gestures_on && !gestures.open(pinch, grip, err)))) ||
        (!record.empty() && !start_recording(record, err))) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!ring.alive()) return std::fprintf(stderr, "ft-camd isn't running (no heartbeat)\n"), 1;
    // so a reader can't take an earlier run's file for this one's
    if (publish && !gestures_on) unlink((run_dir() + "/gestures").c_str());

    std::map<std::string, int> index;   // mono calibration name -> ring camera
    std::map<std::string, int> color;   // colour calibration name (color_video<N>) -> ring camera
    // Recorded as they are with each set: "<name>_dk" (ft-camd --with-dark) and "color_video<N>"
    // (--with-color). Recorded names hold 15 characters, so "upper_right_dark" wouldn't fit.
    std::map<std::string, int> dark;
    std::map<std::string, Camera> used;
    // ft-camd's cameras by XRService's numbering, else by capture pipe (see camera_for_pipe);
    // ft-ringplay's (no device) by the name it gives
    const std::map<int, std::string> by_log = cameras_from_xrservice_log();
    const char *named_by = by_log.empty() ? "capture pipe" : "XRService's log";
    for (int i = 0; i < ring.cameras(); ++i) {
        const fh_ring_cam_t &rc = ring.camera(i);
        if (rc.flags & FH_CAM_COLOR) {
            const std::string name = "color_video" + std::to_string(rc.node);
            dark[name] = i, color[name] = i;
            continue;
        }
        const auto it = by_log.find(rc.node);
        const char *name = rc.node < 0                ? rc.name
                           : !by_log.empty()          ? (it == by_log.end() ? nullptr : it->second.c_str())
                                                      : camera_for_pipe(rc.node);
        if (!name || !calib.count(name)) {
            if (!(rc.flags & FH_CAM_DARK)) std::fprintf(stderr, "video%d (%s): not one of the calibrated cameras, left out\n", rc.node, rc.name);
            continue;
        }
        if (int(rc.width) != calib[name].width || int(rc.height) != calib[name].height) {
            std::fprintf(stderr, "video%d (%s) is %ux%u, but %s is calibrated at %dx%d: left out\n", rc.node, rc.name,
                         rc.width, rc.height, name, calib[name].width, calib[name].height);
            continue;
        }
        if (rc.flags & FH_CAM_DARK) {
            dark[std::string(name) + "_dk"] = i;
        } else if (index.count(name)) {
            std::fprintf(stderr, "video%d (%s) would be %s too (video%d is): left out\n", rc.node, rc.name, name,
                         ring.camera(index[name]).node);
        } else {
            index[name] = i, used[name] = calib[name];
            cams_json += std::string(cams_json.empty() ? "" : ", ") + json_str(name) + ": {\"node\": " +
                         std::to_string(rc.node) + ", \"ring\": " + json_str(rc.name) + "}";
        }
    }
    cams_json = "{\"named_by\": " + json_str(named_by) + ", \"cameras\": {" + cams_json + "}}";
    // ft-camd tells the side cameras' buffers apart by XRService's allocation order, which
    // some XRService restarts reverse (see the top).
    const bool have_sides = index.count("slam_left") && index.count("slam_right");
    std::map<std::string, uint64_t> last;       // per camera: the frame last used
    auto exchange_sides = [&] {
        std::swap(index["slam_left"], index["slam_right"]);
        std::swap(last["slam_left"], last["slam_right"]);
        if (dark.count("slam_left_dk") && dark.count("slam_right_dk")) std::swap(dark["slam_left_dk"], dark["slam_right_dk"]);
    };
    if (names_swapped && have_sides) {
        exchange_sides();
        std::printf("side cameras exchanged (%s)\n", sides_from == "option" ? "--sides 1" : "HANDS_SWAP_SIDES=1");
    }
    // the mono cameras the side check may pair (not the colour ones)
    std::map<std::string, Camera> side_cams;
    for (auto &[name, i] : index) side_cams[name] = calib[name];
    SideCheck side_check(side_cams);
    bool checking = track && have_sides && side_check.usable();
    int side_round = 0;   // auto: 0 deciding, then each check after a decision
    if (!track && sides_mode == "auto" && have_sides)
        std::printf("side cameras: as ft-camd names them (--record-only can't tell; the recording's sides.json says so)\n");
    // The colour pair, calibrated (see the top), unless only the mono cameras are wanted.
    if (color.size() == 2 && (automatic || fixed != Cams::Mono)) {
        const std::string left = color.count(color_left) ? color_left : color.begin()->first;
        const std::string right = color.begin()->first == left ? std::next(color.begin())->first : color.begin()->first;
        const int scale = int(std::lround(1972.0 / ring.camera(color[left]).width));
        std::map<std::string, Camera> cc;
        std::string e;
        if (load_color_calibration(cc, left, right, color_crop == "subtract", scale, e)) {
            for (auto &[name, cam] : cc) used[name] = cam;
            std::printf("colour cameras: %s is passthrough_left, crop %s, 1/%d size\n", left.c_str(), color_crop.c_str(), scale);
        } else {
            std::fprintf(stderr, "colour cameras left out: %s\n", e.c_str());
            color.clear();
        }
    } else {
        color.clear();
    }
    if (color.empty() && (automatic || fixed != Cams::Mono)) {
        if (!automatic) std::printf("no colour cameras (ft-camd --with-color): tracking with the mono ones\n");
        fixed = Cams::Mono;
    }
    const bool switching = automatic && !color.empty();
    Cams mode = switching || color.empty() ? Cams::Mono : fixed;
    std::printf("cameras (by %s):", named_by);
    for (auto &[name, i] : index) std::printf(" %s=video%d", name.c_str(), ring.camera(i).node);
    for (auto &[name, i] : color) std::printf(" %s", name.c_str());
    std::printf("  tracking with %s%s  models: %s%s, %d threads on CPUs", cams_name(mode),
                switching ? " (auto: by the light)" : "", models.c_str(), int8 ? " (int8)" : "", threads);
    for (int c : cpus) std::printf(" %d", c);
    std::printf("\n");

    cpu_set_t set;   // the main loop too
    CPU_ZERO(&set);
    for (int c : cpus) CPU_SET(c, &set);
    if (sched_setaffinity(0, sizeof set, &set) < 0) std::perror("sched_setaffinity");
    nets.set_contrast(palm_contrast, hand_contrast);
    Pool pool(threads, cpus);
    Tracker tracker(used, nets, pool);
    tracker.set_keep_presence(keep_presence);
    tracker.set_misread_guard(misread_guard);
    std::map<std::string, std::vector<uint8_t>> pixels;
    std::map<std::string, uint64_t> lit_seen;   // per colour camera: the frame last counted for the light
    const uint64_t start = mono_ns();
    uint64_t t_status = start, next_ns = 0, t_want = 0, t_glog = 0;
    double cpu0 = cpu_seconds();
    std::vector<double> lat;
    double hands_sum = 0, resid_sum = 0;
    int resid_n = 0, left_sets = 0, right_sets = 0, both_sets = 0, color_steps = 0;
    // The mono cameras' delay from capture to dequeue (their capture clock is CLOCK_MONOTONIC_RAW),
    // to place the colour frames, whose capture clock is their own (see the top).
    double mono_delay_ns = -1;
    const std::string want_file = run_dir() + "/color-fps";
    // The side cameras' naming, for the hand recorder and other tools (hands/rec/session.py
    // reads it): written by a tracking, publishing ft-hands at the start, on every change and
    // every 2 s, and removed when it exits. swapped: ft-camd's naming is backwards (null: not
    // known yet); names_swapped: whether this ft-hands exchanged them; ring_ino: the ring file's
    // inode (a new ft-camd makes a new one, and the decision is only good for that run).
    const std::string sides_file = run_dir() + "/sides.json";
    const bool write_sides_file = publish && track;
    struct stat ring_st{};
    stat(ring_path.c_str(), &ring_st);
    uint64_t t_sides = 0;
    auto write_sides = [&] {
        if (!write_sides_file) return;
        t_sides = mono_ns();
        write_file(sides_file,
                   "{\"pid\": " + std::to_string(getpid()) + ", \"ring_ino\": " + std::to_string(ring_st.st_ino) +
                       ", \"mode\": \"" + sides_mode + "\", \"state\": \"" + sides_state + "\", \"swapped\": " +
                       json_bool(truth) + ", \"decided_by\": " + json_str(truth ? decided_by : "") +
                       ", \"names_swapped\": " + json_bool(names_swapped) +
                       ", \"decided_after_s\": " + std::to_string(decided_after_s) +
                       ", \"evidence\": " + (decision_evidence.empty() ? "null" : decision_evidence) +
                       ", \"checking\": " + (checking ? side_check.json() : "null") +
                       ", \"cameras\": " + (cams_json.empty() ? "null" : cams_json) +
                       ", \"updated_ns\": " + std::to_string(mono_ns()) + "}\n");
    };
    write_sides();
    // A verdict from the side check (see the top and track/sides.h).
    auto side_verdict = [&](SideCheck::Verdict v, uint64_t now) {
        const bool backwards = v == SideCheck::Swapped;   // relative to the names as they are now
        const double after = (now - start) / 1e9;
        const std::string ev = side_check.json(), text = side_check.summary();
        if (sides_mode != "auto") {   // forced: the names stay; if the hands disagree, they're the truth
            const std::string what = (sides_from == "option" ? "--sides " : "HANDS_SWAP_SIDES=") + sides_mode;
            if (backwards)
                std::printf("side cameras: %s looks WRONG: the hands say the side cameras are the other way round (%s). "
                            "Tracking keeps the forced names; recordings are labelled by the hands. Use auto.\n",
                            what.c_str(), text.c_str());
            else
                std::printf("side cameras: %s agrees with the hands (%s)\n", what.c_str(), text.c_str());
            sides_state = backwards ? "forced, disagrees" : "forced, agrees";
            if (backwards) truth = !names_swapped, decided_by = "auto", decided_after_s = after;
            decision_evidence = ev;
            checking = false;
        } else if (side_round == 0 || backwards) {
            if (backwards) {
                exchange_sides();
                names_swapped = !names_swapped;
                tracker.exchange("slam_left", "slam_right");
                if (rec) rec_names.push_back({rec->added(), names_swapped});
            }
            const bool reversed = side_round > 0;
            truth = names_swapped, decided_by = "auto", decided_after_s = after, decision_evidence = ev;
            sides_state = reversed ? "decided, reversed" : "decided";
            std::printf("side cameras: %s after %.1f s (%s)\n",
                        reversed ? "decision REVERSED" : names_swapped ? "SWAPPED, now exchanged" : "as named", after,
                        text.c_str());
            // check once more with the new names, more strictly; at most two changes
            side_check.reset();
            side_check.min_clean = 20, side_check.min_votes = 40;
            checking = ++side_round < 3;
        } else {
            std::printf("side cameras: confirmed after %.1f s (%s)\n", after, text.c_str());
            sides_state = "confirmed";
            checking = false;
        }
        std::fflush(stdout);
        write_rec_sides();
        write_sides();
    };

    // The colour pair's newest frames if both are newer than last used and taken together
    // (their own clock): their time, on the mono cameras' capture clock, else 0.
    auto color_pair = [&](int64_t raw_off, bool copy, std::map<std::string, Image> &images) -> uint64_t {
        fh_ring_slot_t meta[2];
        std::string names[2];
        uint64_t n[2];
        int k = 0;
        for (auto &[name, i] : color) {
            names[k] = name, n[k] = ring.latest(i);
            if (n[k] <= last[name] || !ring.meta(i, n[k], &meta[k])) return 0;
            ++k;
        }
        if (k != 2 || mono_delay_ns < 0) return 0;
        const int64_t apart = int64_t(meta[0].capture_ns) - int64_t(meta[1].capture_ns);
        if (std::llabs(apart) > 3'000'000) return 0;   // one is a frame ahead: wait for the other
        const uint64_t dq = std::min(meta[0].dqbuf_ns, meta[1].dqbuf_ns);
        const uint64_t t = uint64_t(int64_t(dq) - int64_t(mono_delay_ns) + raw_off);
        if (!copy) return t;
        for (int j = 0; j < 2; ++j) {
            const int i = color[names[j]];
            if (!ring.read(i, n[j], pixels[names[j]], &meta[j])) return 0;
            const auto &c = ring.camera(i);
            images[names[j]] = {pixels[names[j]].data(), int(c.width), int(c.height), int(c.width)};
        }
        for (int j = 0; j < 2; ++j) last[names[j]] = n[j];
        return t;
    };
    auto switch_to = [&](Cams to, const char *why) {
        if (to == mode) return;
        for (auto &[name, cam] : used) {
            const bool is_color = color.count(name) > 0;
            const bool keep = to == Cams::All || (to == Cams::Color) == is_color;
            if (!keep) tracker.drop_camera(name);
        }
        std::printf("cameras: %s -> %s (%s)\n", cams_name(mode), cams_name(to), why);
        mode = to;
    };
    auto ambient = [&] {   // the mono cameras' dark frames: the room's IR light
        double sum = 0;
        int n = 0;
        for (auto &[name, i] : index)
            if (ring.camera(i).dark_mean > 0) sum += ring.camera(i).dark_mean, ++n;
        return n ? sum / n : -1;
    };

    int exit_code = 0;
    while (!g_stop && (seconds <= 0 || (mono_ns() - start) / 1e9 < seconds)) {
        if (!ring.alive()) {
            std::fprintf(stderr, "ft-camd stopped\n");
            exit_code = 2;
            break;
        }
        const uint64_t now0 = mono_ns();
        const int64_t raw_off = raw_minus_mono_ns();

        // The light, from the colour frames' brightness (their slot headers only).
        for (auto &[name, i] : color) {
            fh_ring_slot_t m;
            const uint64_t n = ring.latest(i);
            if (n && n != lit_seen[name] && ring.meta(i, n, &m)) light.add(m.mean, m.dqbuf_ns), lit_seen[name] = n;
        }
        if (switching && light.update(now0)) {
            char why[96];
            std::snprintf(why, sizeof why, "colour frames at %.0f, ambient IR %.1f", light.level, ambient());
            switch_to(light.bright ? bright_cams : Cams::Mono, why);
        }
        // Ask ft-camd for the colour cameras' full rate while tracking or recording with them.
        const bool want_color = !color.empty() && (mode != Cams::Mono || rec || g_record);
        if (!color.empty() && now0 - t_want > 1'000'000'000) {
            t_want = now0;
            if (want_color) {
                if (FILE *f = std::fopen(want_file.c_str(), "w")) std::fputs("30\n", f), std::fclose(f);
            } else {
                unlink(want_file.c_str());
            }
        }

        const bool use_mono = mode != Cams::Color, use_color = mode != Cams::Mono;
        const bool mono_driven = use_mono || rec || g_record || !track;
        std::map<std::string, Image> images;
        uint64_t tmin = UINT64_MAX, dq = 0;

        if (mono_driven) {
            // a new frame set: every mono camera has a newer frame, taken at the same moment
            std::map<std::string, uint64_t> latest;
            bool ready = true;
            for (auto &[name, i] : index) {
                latest[name] = ring.latest(i);
                ready = ready && latest[name] > last[name];
            }
            if (!ready) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            // not needed at the current rate, and not recorded: skip it without copying images
            if (track && !rec && !g_record) {
                uint64_t t0 = UINT64_MAX, t1 = 0;
                bool ok = true;
                for (auto &[name, i] : index) {
                    fh_ring_slot_t meta;
                    ok = ok && ring.meta(i, latest[name], &meta);
                    if (ok) t0 = std::min(t0, meta.capture_ns), t1 = std::max(t1, meta.capture_ns);
                }
                if (ok && t1 - t0 <= 3'000'000 && t0 < next_ns) {
                    for (auto &[name, i] : index) last[name] = latest[name];
                    continue;
                }
            }
            std::vector<SetFrame> frames;
            uint64_t tmax = 0;
            bool ok = true;
            for (auto &[name, i] : index) {
                fh_ring_slot_t meta;
                ok = ok && ring.read(i, latest[name], pixels[name], &meta);
                if (!ok) break;
                const auto &c = ring.camera(i);
                images[name] = {pixels[name].data(), int(c.width), int(c.height), int(c.width)};
                frames.push_back({name, pixels[name].data(), c.width, c.height, meta.capture_ns, meta.dqbuf_ns});
                tmin = std::min(tmin, meta.capture_ns), tmax = std::max(tmax, meta.capture_ns), dq = std::max(dq, meta.dqbuf_ns);
                const double delay = double(int64_t(meta.dqbuf_ns) - (int64_t(meta.capture_ns) - raw_off));
                mono_delay_ns = mono_delay_ns < 0 ? delay : mono_delay_ns + 0.02 * (delay - mono_delay_ns);
            }
            if (!ok || tmax - tmin > 3'000'000) {   // torn, or a camera is a frame behind
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            for (auto &[name, i] : index) last[name] = latest[name];
            if (g_record && !rec) {
                g_record = 0;
                char name[64];
                const std::time_t now = std::time(nullptr);
                std::strftime(name, sizeof name, "rec-%Y%m%d-%H%M%S", std::localtime(&now));
                const std::string dir = recordings_dir();
                std::string e;
                if (!start_recording(dir + "/" + name, e)) std::fprintf(stderr, "%s\n", e.c_str());
            }
            if (rec) {   // about 80 MB/s; dark frames double that, color frames add 70 MB/s
                const bool due = record_hz <= 0 || tmin >= rec_next_ns;   // --record-hz skips the sets between
                if (due && record_hz > 0) rec_next_ns = tmin + uint64_t(1e9 / record_hz) - 5'000'000;
                if ((mono_ns() - rec_start) / 1e9 >= record_for) {
                    write_rec_sides();
                    const size_t n = rec->written(), d = rec->dropped();
                    rec.reset();   // writes out what's queued
                    std::printf("recording done: %zu sets, %zu dropped\n", n, d);
                    std::fflush(stdout);
                    if (!track) break;
                } else if (due) {
                    for (auto &[name, i] : dark) {   // the newest dark and color frames, as they are
                        fh_ring_slot_t meta;
                        const uint64_t n = ring.latest(i);
                        const auto &c = ring.camera(i);
                        if (n && ring.read(i, n, pixels[name + "#rec"], &meta))
                            frames.push_back({name, pixels[name + "#rec"].data(), c.width, c.height, meta.capture_ns,
                                              meta.dqbuf_ns});
                    }
                    rec->add(frames);
                }
            }
            if (!track && rec && status > 0 && (mono_ns() - t_status) / 1e9 >= status) {
                std::printf("%5.1fs  recorded %zu sets, dropped %zu\n", (mono_ns() - start) / 1e9, rec->written(), rec->dropped());
                std::fflush(stdout);
                t_status = mono_ns();
            }
            if (!track || tmin < next_ns) continue;   // not needed yet at the current rate
            if (!use_mono) images.clear();            // colour only, driven by mono while recording
            if (use_color && color_pair(raw_off, true, images)) ++color_steps;
            if (images.empty()) continue;
        } else {
            // colour only: a new pair of colour frames
            for (auto &[name, i] : index) {   // keep the mono cameras' delay current
                fh_ring_slot_t meta;
                const uint64_t n = ring.latest(i);
                if (n && ring.meta(i, n, &meta)) {
                    const double delay = double(int64_t(meta.dqbuf_ns) - (int64_t(meta.capture_ns) - raw_off));
                    mono_delay_ns = mono_delay_ns < 0 ? delay : mono_delay_ns + 0.02 * (delay - mono_delay_ns);
                }
                break;
            }
            const uint64_t t = color_pair(raw_off, false, images);
            if (!t) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (t < next_ns) {   // not needed yet: pass it by without copying
                for (auto &[name, i] : color) last[name] = ring.latest(i);
                continue;
            }
            if (!color_pair(raw_off, true, images)) continue;
            tmin = t, ++color_steps;
            for (auto &[name, i] : color) {
                fh_ring_slot_t meta;
                if (ring.meta(i, last[name], &meta)) dq = std::max(dq, meta.dqbuf_ns);
            }
        }

        const auto hands = tracker.step(images, int64_t(tmin));
        const uint64_t capture = uint64_t(int64_t(tmin) - raw_off);   // CLOCK_MONOTONIC
        const std::vector<Seen> views = tracker.views_now();
        std::vector<Seen> side_views;   // the tracker's views, plus the side check's own looks
        if (checking) {
            side_views = views;
            for (Seen &v : side_check.probe(nets, pool, images, views, int64_t(tmin))) side_views.push_back(v);
        }
        if (checking && side_check.add(side_views, int64_t(tmin)) > 0) {
            const SideCheck::Verdict v = side_check.verdict();
            if (v != SideCheck::Undecided) side_verdict(v, mono_ns());
        }
        if (mono_ns() - t_sides > 2'000'000'000ull) write_sides();
        if (gestures_on) {
            grip.update(hands, views, int64_t(capture));
            pinch.update(hands, views, int64_t(capture), grip.gripping());
        }
        if (gestures_on && gesture_log && capture - t_glog >= 100'000'000) {
            t_glog = capture;
            for (int k = 0; k < 2; ++k)
                if (pinch.world_d[k] >= 0)
                    std::printf("gesture %s d world %.3f tri %.3f palm-down %.2f curl %.2f%s\n", k ? "right" : "left ",
                                pinch.world_d[k], pinch.tri_d[k], pinch.palm_down[k], grip.curl[k],
                                pinch.side(k).flags & FH_PINCH_DOWN ? " PINCH" : grip.side(k).flags & FH_PINCH_DOWN ? " GRIP" : "");
        }
        // a gesture down or closing gets the full rate, even while the palm holds still
        next_ns = tmin + uint64_t((std::min(tracker.interval(), pinch.engaged() || grip.engaged() ? 1 / 30.0 : 1.0) -
                                   0.005) * 1e9);
        if (publish) pub.write(hands, capture);
        if (publish && gestures_on) gestures.write(pinch, grip, capture);
        for (const Pinch::Event &e : grip.events)
            std::printf("grip  %s %-5s curl %.2f at %+.3f %+.3f %+.3f\n", e.side ? "right" : "left ", e.what, e.distance,
                        e.point[0], e.point[1], e.point[2]);
        for (const Pinch::Event &e : pinch.events)
            std::printf("pinch %s %-5s d %.3f m at %+.3f %+.3f %+.3f\n", e.side ? "right" : "left ", e.what, e.distance,
                        e.point[0], e.point[1], e.point[2]);
        lat.push_back((mono_ns() - dq) / 1e6);
        hands_sum += double(hands.size());
        bool on_left = false, on_right = false;   // by where the wrist is, not the model's label
        for (const Hand *h : hands) {
            if (h->residual >= 0) resid_sum += h->residual * 1000, ++resid_n;
            (h->pts[0][0] < 0 ? on_left : on_right) = true;
        }
        left_sets += on_left, right_sets += on_right, both_sets += on_left && on_right;

        const uint64_t now = mono_ns();
        if (status > 0 && (now - t_status) / 1e9 >= status) {
            const double dt = (now - t_status) / 1e9, cpu1 = cpu_seconds();
            const Stats &s = tracker.stats;
            std::sort(lat.begin(), lat.end());
            std::printf("%5.1fs %4.1f sets/s  hands %.2f views %zu  palm %3d calls %4.1f ms/batch  hand %3d calls %4.1f ms/batch  "
                        "step %4.1f ms  latency %4.1f ms  resid %.1f mm  CPU %3.0f%%\n",
                        (now - start) / 1e9, s.sets / dt, s.sets ? hands_sum / s.sets : 0, tracker.views(), s.palm_calls,
                        s.palm_batches ? s.palm_ms / s.palm_batches : 0, s.hand_calls,
                        s.hand_batches ? s.hand_ms / s.hand_batches : 0, s.sets ? s.step_ms / s.sets : 0,
                        lat.empty() ? 0 : lat[lat.size() / 2], resid_n ? resid_sum / resid_n : 0, 100 * (cpu1 - cpu0) / dt);
            if (!color.empty())
                std::printf("        cameras %s%s: colour frames at %.1f (bright at %.0f, dim under %.0f), ambient IR %.1f, "
                            "%d steps with colour, colour placed %.1f ms after capture\n",
                            cams_name(mode), switching ? " (auto)" : "", light.level, light.on, light.off, ambient(),
                            color_steps, mono_delay_ns / 1e6);
            if (s.sets)
                std::printf("        sets with a hand: left %2.0f%% right %2.0f%% both %2.0f%%  views lost %d, handoff misses %d, "
                            "dups %d, splits %d  hands new %d merged %d forgotten %d%s\n",
                            100.0 * left_sets / s.sets, 100.0 * right_sets / s.sets, 100.0 * both_sets / s.sets, s.lost,
                            s.handoff_miss, s.dups, s.splits, s.created, s.merged, s.forgotten,
                            !rec ? "" : ("  recorded " + std::to_string(rec->written()) + " dropped " +
                                         std::to_string(rec->dropped())).c_str());
            if (checking)
                std::printf("        side cameras: %s, %s\n", side_round ? "checking the decision" : "deciding",
                            side_check.summary().c_str());
            if (gestures_on) {
                std::printf("        pinches: left %u right %u (held back, palm down: %d %d)  grips: left %u right %u",
                            pinch.side(0).begins, pinch.side(1).begins, pinch.held_back[0], pinch.held_back[1],
                            grip.side(0).begins, grip.side(1).begins);
                for (int k = 0; k < 2; ++k)
                    if (pinch.side(k).flags & FH_PINCH_TRACKED)
                        std::printf("  %s %s d %.3f curl %.2f", k ? "right" : "left",
                                    grip.side(k).flags & FH_PINCH_DOWN    ? "GRIP"
                                    : pinch.side(k).flags & FH_PINCH_DOWN ? "PINCH"
                                                                          : "open",
                                    pinch.side(k).distance, grip.curl[k]);
                std::printf("\n");
            }
            for (const Hand *h : hands)
                std::printf("        hand %d %-5s views %d wrist %+.3f %+.3f %+.3f m  scale %.2f  speed %.2f m/s\n", h->id,
                            h->right() ? "right" : "left", h->nviews, h->pts[0][0], h->pts[0][1], h->pts[0][2], h->scale,
                            h->speed);
            std::fflush(stdout);
            tracker.stats = Stats{};
            t_status = now, cpu0 = cpu1;
            lat.clear(), hands_sum = 0, resid_sum = 0, resid_n = 0, left_sets = right_sets = both_sets = 0;
            color_steps = 0;
        }
    }
    if (!color.empty()) unlink(want_file.c_str());
    if (write_sides_file) unlink(sides_file.c_str());
    write_rec_sides();
    if (publish) pub.write({}, mono_ns());
    if (publish && gestures_on) {
        pinch.release(int64_t(mono_ns()));   // a drag in progress ends, as lost
        grip.release(int64_t(mono_ns()));
        gestures.write(pinch, grip, mono_ns());
    }
    return exit_code;
}
