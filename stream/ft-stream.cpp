// ft-stream: one remote display for ft-screens (docs/remote-displays.md). A Moonlight client
// (moonlight-common-c, and libgamestream for pairing and launching) for one display of a
// Vibepollo host. It decodes the stream on the Frame's iris decoder and converts each picture
// into a ring of three RGBA buffers (spike/iris.h), which ft-screens shows as a panel like any
// screen of the desktop: its controls, layout, profiles, curve, pins and hand cutouts. Each
// display is its own client with its own certificate (--id), because Vibepollo ties a Remote
// Monitor to the client that opened it, one stream per client.
//
//   ft-stream pair HOST --id NAME [--token FILE]     (pairs with the host's API token, or a PIN)
//   ft-stream grant HOST --id NAME [--token FILE]    (permissions for a client paired by PIN)
//   ft-stream unpair HOST --id NAME
//   ft-stream monitors HOST [--token FILE]           (the host's monitors, as Vibepollo lists them)
//   ft-stream release HOST --id NAME                 (Vibepollo's "Disconnect Monitor" for this client)
//   ft-stream stream HOST --id NAME --fd N --app display:DEVICE|monitor|primary|ID|NAME
//                    [--size 2560x1440] [--fps 60] [--bitrate kbps]
//
// Keys live in ~/.local/share/frametop-stream/NAME, the host's token in
// ~/.local/share/frametop-stream/hosts/HOST.token (see host.h).
//
// "stream" is started by ft-screens (screens/remote.c), which gives it one end of a
// SOCK_SEQPACKET socket pair as fd N. Messages are text, one per packet.
//   From ft-screens:
//     modifiers M...        the DRM format modifiers SteamVR imports ABGR8888 with (hex), first
//     release I             ring buffer I is off the panel: it may be drawn into again
//     attention focused|view|hidden
//                           hidden: every frame is still decoded (so it's back at once), and
//                           kHiddenFps of them shown
//     move X Y              the pointer, in stream pixels from the top left
//     button CODE 1|0       a linux BTN_* code, pressed or released
//     scroll DX DY          wheel notches (positive DY: down)
//     key CODE 1|0          a linux KEY_* code, pressed or released
//     blur                  typing went to another screen: the keys held here come up
//     quit
//   To ft-screens:
//     state connecting|live|lost [TEXT]   (live via dongle|network: which way it came, see Route)
//     buffers W H FORMAT MODIFIER OFFSET STRIDE    with the three buffers' dmabuf fds
//                           (SCM_RIGHTS), single plane, FORMAT and MODIFIER in hex
//     frame I               ring buffer I holds a new picture (the GPU is done with it)
// It exits when ft-screens closes its end. A Remote Monitor is released then ("Disconnect
// Monitor"); an existing display holds nothing on the host.
//
// Runs in the dev container. Build: stream/build.sh.
#include "spike/iris.h"

#include "host.h"

#include <netdb.h>
#include <opus/opus_multistream.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#include <linux/input-event-codes.h>

#include "input/keyboard.h"  // moonlight-embedded: linux KEY_* -> Windows virtual-key codes

namespace {

using namespace ftstream;

// One frame from moonlight-common-c's decoder thread, waiting for the main loop.
struct Unit {
    std::vector<uint8_t> data;
};

std::mutex g_mu;
std::deque<Unit> g_units;
int g_wake = -1;
std::atomic<bool> g_hidden{false}, g_waitIdr{false};
std::atomic<int> g_ended{0};  // connection terminated: error code + 1
// For the log's report: frames received and decoded, bytes, decode time.
std::atomic<int> g_received{0}, g_skipped{0};
std::atomic<uint64_t> g_bytes{0};
// Out of sight (attention hidden), a panel still shows this many frames a second.
constexpr int kHiddenFps = 10;

// The host's sound. Every display's stream carries it, the same for all of them, so one
// stream per host plays it: the one holding $XDG_RUNTIME_DIR/frametop-audio-HOST.lock (the
// others try again every few seconds, so another takes over when that one ends). It plays
// through PipeWire's PulseAudio server; the host keeps playing it too (localAudioPlayMode).
struct Audio {
    std::atomic<bool> on{false};
    std::string name;  // the playback stream's name in the mixer
    int lock = -1;
    int64_t tryAt = 0;
    OpusMSDecoder *dec = nullptr;
    pa_simple *pa = nullptr;
    bool paFailed = false;
    int rate = 48000, channels = 2, samples = 240;
    std::vector<int16_t> pcm;

    void Claim(const std::string &host, int64_t now) {
        if (on || now < tryAt) return;
        tryAt = now + 3'000'000'000;
        if (lock < 0) {
            const char *dir = std::getenv("XDG_RUNTIME_DIR");
            const std::string path = std::string(dir && *dir ? dir : "/tmp") + "/frametop-audio-" + host + ".lock";
            lock = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        }
        if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0) {
            on = true;
            std::printf("audio: this stream plays %s's sound\n", host.c_str());
            std::fflush(stdout);
        }
    }
};
Audio g_audio;
std::string g_host;  // the host's own address, whichever way the stream goes (Route)

int AudioInit(int, const POPUS_MULTISTREAM_CONFIGURATION opus, void *, int) {
    int err = 0;
    g_audio.dec = opus_multistream_decoder_create(opus->sampleRate, opus->channelCount, opus->streams,
                                                  opus->coupledStreams, opus->mapping, &err);
    if (!g_audio.dec) return std::fprintf(stderr, "audio: opus: %s\n", opus_strerror(err)), -1;
    g_audio.rate = opus->sampleRate, g_audio.channels = opus->channelCount, g_audio.samples = opus->samplesPerFrame;
    g_audio.pcm.resize(size_t(opus->samplesPerFrame * opus->channelCount));
    return 0;
}

void AudioCleanup() {
    if (g_audio.pa) pa_simple_free(g_audio.pa);
    if (g_audio.dec) opus_multistream_decoder_destroy(g_audio.dec);
    g_audio.pa = nullptr, g_audio.dec = nullptr;
}

// On moonlight-common-c's audio thread; a lost packet comes as nullptr (Opus fills it in).
void AudioPlay(char *data, int length) {
    if (!g_audio.on || !g_audio.dec) return;
    if (!g_audio.pa) {
        if (g_audio.paFailed) return;
        const pa_sample_spec spec = {PA_SAMPLE_S16LE, uint32_t(g_audio.rate), uint8_t(g_audio.channels)};
        pa_buffer_attr attr;
        attr.maxlength = uint32_t(-1), attr.prebuf = uint32_t(-1), attr.minreq = uint32_t(-1), attr.fragsize = uint32_t(-1);
        attr.tlength = uint32_t(pa_usec_to_bytes(40'000, &spec));  // about 40 ms queued: little delay over the picture
        int err = 0;
        g_audio.pa = pa_simple_new(nullptr, "Frametop", PA_STREAM_PLAYBACK, nullptr, g_audio.name.c_str(), &spec, nullptr,
                                   &attr, &err);
        if (!g_audio.pa) {
            g_audio.paFailed = true;
            std::fprintf(stderr, "audio: can't play: %s\n", pa_strerror(err));
            return;
        }
    }
    const int n = opus_multistream_decode(g_audio.dec, reinterpret_cast<unsigned char *>(data), length, g_audio.pcm.data(),
                                          g_audio.samples, 0);
    int err = 0;
    if (n > 0) pa_simple_write(g_audio.pa, g_audio.pcm.data(), size_t(n * g_audio.channels) * sizeof(int16_t), &err);
}

int Submit(PDECODE_UNIT du) {
    const bool idr = du->frameType == FRAME_TYPE_IDR;
    ++g_received, g_bytes += uint64_t(du->fullLength);
    if (idr) g_waitIdr = false;
    if (!idr && g_waitIdr) return ++g_skipped, DR_OK;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_units.size() >= 16) {  // the main loop fell far behind: start over from an IDR
        g_units.clear();
        g_waitIdr = true;
        return DR_NEED_IDR;
    }
    Unit u;
    u.data.resize(size_t(du->fullLength));
    size_t at = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) std::memcpy(&u.data[at], e->data, size_t(e->length)), at += size_t(e->length);
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

// The socket to ft-screens.
struct Viewer {
    int fd = -1;

    bool Send(const std::string &msg, const int *fds = nullptr, int nfds = 0) const {
        iovec iov = {const_cast<char *>(msg.data()), msg.size()};
        msghdr m{};
        m.msg_iov = &iov, m.msg_iovlen = 1;
        char control[CMSG_SPACE(sizeof(int) * 4)] = {};
        if (nfds) {
            m.msg_control = control, m.msg_controllen = CMSG_SPACE(sizeof(int) * size_t(nfds));
            cmsghdr *c = CMSG_FIRSTHDR(&m);
            c->cmsg_level = SOL_SOCKET, c->cmsg_type = SCM_RIGHTS, c->cmsg_len = CMSG_LEN(sizeof(int) * size_t(nfds));
            std::memcpy(CMSG_DATA(c), fds, sizeof(int) * size_t(nfds));
        }
        return sendmsg(fd, &m, MSG_NOSIGNAL) == ssize_t(msg.size());
    }

    // One message, or "" when there's none now; false when ft-screens is gone.
    bool Read(std::string *msg, bool wait) const {
        char buf[512];
        const ssize_t n = recv(fd, buf, sizeof buf - 1, wait ? 0 : MSG_DONTWAIT);
        if (n > 0) return *msg = std::string(buf, size_t(n)), true;
        msg->clear();
        return n < 0 && (errno == EAGAIN || errno == EINTR);
    }
};

// Typing: Moonlight wants Windows virtual-key codes and the modifiers held.
struct Keys {
    char modifiers = 0;
    std::set<unsigned> held;

    void Send(unsigned code, bool down) {
        if (code >= sizeof keyCodes / sizeof keyCodes[0] || !keyCodes[code]) return;
        if (down) held.insert(code);
        else if (!held.erase(code)) return;  // never went down here
        char m = 0;
        switch (code) {
            case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: m = MODIFIER_SHIFT; break;
            case KEY_LEFTALT: case KEY_RIGHTALT: m = MODIFIER_ALT; break;
            case KEY_LEFTCTRL: case KEY_RIGHTCTRL: m = MODIFIER_CTRL; break;
            case KEY_LEFTMETA: case KEY_RIGHTMETA: m = MODIFIER_META; break;
        }
        if (m) modifiers = down ? char(modifiers | m) : char(modifiers & ~m);
        LiSendKeyboardEvent(short(0x80 << 8 | keyCodes[code]), down ? KEY_ACTION_DOWN : KEY_ACTION_UP, modifiers);
    }

    void Blur() {
        while (!held.empty()) Send(*held.begin(), false);
    }
};

int MoonlightButton(unsigned code) {
    switch (code) {
        case BTN_RIGHT: return BUTTON_RIGHT;
        case BTN_MIDDLE: return BUTTON_MIDDLE;
        case BTN_SIDE: return BUTTON_X1;
        case BTN_EXTRA: return BUTTON_X2;
        default: return BUTTON_LEFT;
    }
}

// Whether a host's GameStream port (47989) takes a connection within `ms`.
bool Answers(const std::string &address, int ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(address.c_str(), "47989", &hints, &res) != 0) return false;
    bool ok = false;
    for (addrinfo *a = res; a && !ok; a = a->ai_next) {
        const int s = socket(a->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (s < 0) continue;
        if (connect(s, a->ai_addr, a->ai_addrlen) == 0) ok = true;
        else if (errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            ok = poll(&p, 1, ms) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

// The address to reach a host at, by its "route" in ~/.config/frametop-layout.json
// (Frametop Remote Displays sets it): "network" is its own address; "dongle" one of its
// "direct" addresses, such as its Steam Link dongle on the Frame's own hotspot, and none
// (empty) while none answers; "auto" (the default) the first direct one that answers, else
// its own. Through the home router a stream lost a frame's packets in bursts (0.33% of
// packets, 1.8% of frames at 50 Mbit/s); over the dongle, none (2026-10-07).
std::string Route(const std::string &host) {
    std::ifstream f(std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.config/frametop-layout.json");
    const nlohmann::json layout = nlohmann::json::parse(f, nullptr, false);
    if (!layout.is_object() || !layout.contains("hosts") || !layout["hosts"].is_array()) return host;
    for (const auto &h : layout["hosts"]) {
        if (!h.is_object() || h.value("address", "") != host) continue;
        const std::string route = h.value("route", "auto");
        if (route == "network") return host;
        if (h.contains("direct") && h["direct"].is_array())
            for (const auto &d : h["direct"])
                // A second: right after Vibepollo starts, 300 ms sent streams the long way.
                if (d.is_string() && Answers(d.get<std::string>(), 1000)) return d.get<std::string>();
        return route == "dongle" ? "" : host;
    }
    return host;
}

int Stream(Client &client, const std::string &app, int fd, int width, int height, int fps, int bitrate) {
    Viewer viewer{fd};
    // ft-screens' event loop blocks SIGTERM (it takes it through a signalfd), and an older
    // ft-screens passed that on: SIGTERM, and so the death signal, never came.
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    prctl(PR_SET_PDEATHSIG, SIGTERM);  // ft-screens went: so does this
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    signal(SIGPIPE, SIG_IGN);
    if (!client.server.paired) {
        viewer.Send("state lost not paired");
        return std::fprintf(stderr, "not paired as %s: run ft-stream pair %s --id %s\n", client.id.c_str(),
                            client.host.c_str(), client.id.c_str()), 1;
    }
    const int appId = client.FindApp(app);
    if (!appId) return viewer.Send("state lost no such display"), 1;

    // The first word from ft-screens: what SteamVR imports.
    Converter conv;
    conv.toSteamVR = false;
    for (std::string msg; viewer.Read(&msg, true) && !msg.empty();) {
        if (msg.rfind("modifiers", 0) != 0) continue;
        std::istringstream words(msg.substr(9));
        for (std::string m; words >> m;) conv.modifiers.push_back(std::strtoull(m.c_str(), nullptr, 16));
        break;
    }
    Decoder dec;
    dec.toSteamVR = false;
    if (!dec.Open("/dev/video-dec0", 6) || !conv.Init()) return viewer.Send("state lost no decoder"), 1;

    STREAM_CONFIGURATION cfg;
    LiInitializeStreamConfiguration(&cfg);
    cfg.width = width, cfg.height = height, cfg.fps = fps, cfg.bitrate = bitrate;
    cfg.packetSize = 1392;
    cfg.streamingRemotely = STREAM_CFG_AUTO;
    cfg.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    cfg.supportedVideoFormats = VIDEO_FORMAT_H265;
    cfg.clientRefreshRateX100 = fps * 100;
    cfg.colorSpace = COLORSPACE_REC_709;
    cfg.colorRange = COLOR_RANGE_LIMITED;
    cfg.encryptionFlags = ENCFLG_AUDIO;

    viewer.Send("state connecting");
    // Remote Monitors take a moment to appear; Vibepollo says 503 until then.
    const bool resume = appId != kRemoteMonitor && appId != kFrametopDisplay && client.server.currentGame == appId;
    std::string message;
    int code = 0;
    for (int tries = 0; tries < 60 && !g_stop; ++tries) {
        code = client.Launch(cfg, appId, resume, &message);
        if (code != 503) break;
        usleep(500'000);
    }
    std::printf("%s app %d: %d %s\n", resume ? "resume" : "launch", appId, code, message.c_str());
    if (code != 200) return viewer.Send("state lost " + std::to_string(code) + " " + message), 1;

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
    AUDIO_RENDERER_CALLBACKS ar;
    LiInitializeAudioCallbacks(&ar);
    ar.init = AudioInit;
    ar.cleanup = AudioCleanup;
    ar.decodeAndPlaySample = AudioPlay;
    ar.capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;
    g_audio.name = "Remote display: " + g_host;
    g_audio.Claim(g_host, MonoNs());
    if (LiStartConnection(&client.server.serverInfo, &cfg, &cl, &dr, &ar, nullptr, 0, nullptr, 0) != 0)
        return viewer.Send("state lost can't connect"), 1;
    std::printf("stream: connected, %d kbit/s asked\n", bitrate);
    std::fflush(stdout);

    Keys keys;
    bool owned[3] = {};  // ring buffers ft-screens shows or is about to
    bool live = false;
    uint64_t seq = 0;
    int64_t lastReport = MonoNs(), cpu0 = CpuNs(), lastShown = 0, allHeldSince = -1;
    int shown = 0, dropped = 0, rested = 0, lastSent = -1;
    while (!g_stop) {
        pollfd p[3] = {{dec.fd, POLLIN | POLLPRI | POLLOUT, 0}, {g_wake, POLLIN, 0}, {viewer.fd, POLLIN, 0}};
        if (poll(p, 3, 50) > 0 && (p[0].revents & POLLERR)) usleep(1000);
        uint64_t n;
        if (read(g_wake, &n, sizeof n) < 0) {}
        // ft-screens: input, released buffers, attention.
        std::string msg;
        bool open = true;
        while ((open = viewer.Read(&msg, false)) && !msg.empty()) {
            double x, y;
            unsigned c;
            int down, i;
            if (std::sscanf(msg.c_str(), "move %lf %lf", &x, &y) == 2) {
                LiSendMousePositionEvent(short(std::clamp(int(x), 0, width - 1)), short(std::clamp(int(y), 0, height - 1)),
                                         short(width), short(height));
            } else if (std::sscanf(msg.c_str(), "button %u %d", &c, &down) == 2) {
                LiSendMouseButtonEvent(down ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, MoonlightButton(c));
            } else if (std::sscanf(msg.c_str(), "scroll %lf %lf", &x, &y) == 2) {
                if (y != 0) LiSendHighResScrollEvent(short(-y * 120));
                if (x != 0) LiSendHighResHScrollEvent(short(x * 120));
            } else if (std::sscanf(msg.c_str(), "key %u %d", &c, &down) == 2) {
                keys.Send(c, down != 0);
            } else if (msg == "blur") {
                keys.Blur();
            } else if (std::sscanf(msg.c_str(), "release %d", &i) == 1) {
                if (i >= 0 && i < 3) owned[i] = false;
            } else if (msg.rfind("attention ", 0) == 0) {
                g_hidden = msg == "attention hidden";
            } else if (msg == "quit") {
                g_stop = 1;
            }
        }
        // ft-screens closed its end. (Asking again here once lost whatever came in between:
        // a "release" lost so kept a buffer from the ring for good, and after three the
        // panel froze, 2026-10-07.)
        if (!open) break;
        g_audio.Claim(g_host, MonoNs());
        if (dec.Events()) {
            if (!conv.Setup(dec)) break;
            const int fds[3] = {conv.out[0].fd, conv.out[1].fd, conv.out[2].fd};
            char b[160];
            std::snprintf(b, sizeof b, "buffers %u %u %x %llx %u %u", conv.width, conv.height, DRM_FORMAT_ABGR8888,
                          (unsigned long long)conv.out[0].modifier, conv.out[0].offset, conv.out[0].stride);
            if (!viewer.Send(b, fds, 3)) break;
            for (bool &o : owned) o = false;
        }
        dec.Reclaim();
        for (;;) {
            Unit u;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                if (g_units.empty()) break;
                if (!dec.Feed(g_units.front().data.data(), g_units.front().data.size(), seq)) break;
                g_units.pop_front();
            }
            ++seq;
        }
        uint64_t frame = 0;
        bool last = false;
        int i;
        while ((i = dec.Decoded(&frame, &last)) >= 0) {
            const int64_t at0 = MonoNs();
            if (g_hidden && at0 - lastShown < 1'000'000'000 / kHiddenFps) {  // out of sight
                dec.Requeue(i);
                ++rested;
                continue;
            }
            int at = -1;
            for (int k = 0; k < 3 && at < 0; ++k)
                if (!owned[(conv.next + k) % 3]) at = (conv.next + k) % 3;
            if (at < 0) {  // ft-screens still has all three: this picture is skipped
                dec.Requeue(i);
                ++dropped;
                // ft-screens hands one back with every new picture it shows. If none comes
                // back for this long, they were lost: all but the one it shows are ours again.
                if (allHeldSince < 0) allHeldSince = at0;
                else if (at0 - allHeldSince > 2'000'000'000) {
                    std::printf("ring: no buffer back from ft-screens for 2 s, taking them back\n");
                    for (int k = 0; k < 3; ++k) owned[k] = k == lastSent;
                    allHeldSince = -1;
                }
                continue;
            }
            allHeldSince = -1;
            conv.ConvertInto(i, at);
            conv.next = (at + 1) % 3;
            dec.Requeue(i);
            owned[at] = true;
            if (!viewer.Send("frame " + std::to_string(at))) g_stop = 1;
            lastSent = at, lastShown = at0;
            ++shown;
            if (!live) live = true, viewer.Send(client.host != g_host ? "state live via dongle" : "state live via network");
        }
        const int64_t now = MonoNs();
        if (now - lastReport >= 10'000'000'000) {
            const double secs = (now - lastReport) / 1e9;
            uint32_t rtt = 0, rttVar = 0;
            LiGetEstimatedRttInfo(&rtt, &rttVar);
            std::printf("%s: in %.1f fps (%d skipped), shown %.1f fps (%d dropped, %d out of sight), %.1f Mbit/s, rtt %u ms, "
                        "convert %.1f ms, CPU %.1f%% of a core%s\n",
                        g_hidden ? "hidden" : "shown", g_received.exchange(0) / secs, g_skipped.exchange(0), shown / secs,
                        dropped, rested, g_bytes.exchange(0) * 8 / secs / 1e6, rtt, shown ? conv.msSum / shown : 0.0,
                        100 * (CpuNs() - cpu0) / 1e9 / secs, g_audio.on ? ", playing its sound" : "");
            std::fflush(stdout);
            conv.msSum = 0, shown = dropped = rested = 0, lastReport = now, cpu0 = CpuNs();
        }
    }
    const int ended = g_ended;
    g_stop = 1;
    LiStopConnection();
    viewer.Send(ended ? "state lost the host ended the stream" : "state lost");
    dec.Close();
    if (appId == kRemoteMonitor) {
        code = client.Launch(cfg, kDisconnectMonitor, false, &message);
        std::printf("release monitor: %d %s\n", code, message.c_str());
    }
    return ended > 1 ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 3) {
        std::fprintf(stderr, "usage: ft-stream pair|grant|unpair|monitors|release|stream HOST --id NAME [options]\n");
        return 2;
    }
    const std::string cmd = argv[1], host = argv[2];
    std::string id, app = "primary", tokenFile;
    int width = 2560, height = 1440, fps = 60, bitrate = 0, fd = -1;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--id") id = v, ++i;
        else if (a == "--token") tokenFile = v, ++i;
        else if (a == "--app") app = v, ++i;
        else if (a == "--size") std::sscanf(v, "%dx%d", &width, &height), ++i;
        else if (a == "--fps") fps = std::atoi(v), ++i;
        else if (a == "--bitrate") bitrate = std::atoi(v), ++i;
        else if (a == "--fd") fd = std::atoi(v), ++i;
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
    if (width < 64 || height < 64 || width > 8192 || height > 8192 || fps < 1 || fps > 240)
        return std::fprintf(stderr, "bad --size or --fps\n"), 2;
    // Moonlight's rule of thumb, about 20 Mbit/s for 1080p60, scaled by pixels and fps.
    if (!bitrate) bitrate = int(std::min(150000.0, 20000.0 * width * height * fps / (1920.0 * 1080 * 60)));

    g_host = host;
    const std::string route = cmd == "stream" ? Route(host) : host;
    if (route.empty()) {  // dongle only, and it isn't up: ft-screens tries again later
        std::fprintf(stderr, "%s: no direct address answers (route dongle)\n", host.c_str());
        if (fd >= 0) Viewer{fd}.Send("state lost the dongle link is down");
        return 1;
    }
    Client client;
    if (!client.Connect(route, id)) return 1;
    std::printf("host %s%s: %s, %s, current app %d\n", host.c_str(), route != host ? (" through " + route).c_str() : "",
                client.server.serverInfo.serverInfoAppVersion, client.server.paired ? "paired" : "not paired",
                client.server.currentGame);
    if (cmd == "pair") {
        if (client.server.paired) return std::printf("already paired as %s\n", id.c_str()), 0;
        return client.Pair(haveToken ? &api : nullptr) ? 0 : 1;
    }
    if (cmd == "grant") {
        if (!haveToken) return std::fprintf(stderr, "no token in %s\n", tokenFile.c_str()), 1;
        return client.GrantByName(api) ? 0 : 1;
    }
    if (cmd == "unpair") return client.Unpair() ? 0 : 1;
    if (cmd == "release") {
        STREAM_CONFIGURATION cfg;
        LiInitializeStreamConfiguration(&cfg);
        cfg.width = width, cfg.height = height, cfg.fps = fps;
        std::string message;
        const int code = client.Launch(cfg, kDisconnectMonitor, false, &message);
        std::printf("release: %d %s\n", code, message.c_str());
        return code == 200 || code == 410 ? 0 : 1;
    }
    if (cmd == "stream") {
        if (fd < 0) return std::fprintf(stderr, "stream needs --fd: ft-screens starts it\n"), 2;
        return Stream(client, app, fd, width, height, fps, bitrate);
    }
    return std::fprintf(stderr, "unknown command %s\n", cmd.c_str()), 2;
}
