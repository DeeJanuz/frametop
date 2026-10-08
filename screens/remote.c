// Remote screens: displays of other machines as panels of their own (docs/remote-displays.md).
// Each is streamed by its own ft-stream process (stream/ft-stream.cpp, GPLv3, a separate
// program so this one stays MIT), started here with one end of a SOCK_SEQPACKET socket pair
// as its fd 3. ft-stream decodes into a ring of three RGBA buffers and hands their dmabufs
// over once; then "frame I" says buffer I holds a new picture. It goes to the panel through
// ft_vr_screen_present, like a KWin buffer, and the buffer shown before it goes back
// ("release"). The panel's pointer input, the keys typed while it has the keyboard, and how
// much of it you see ("attention") go the other way; the protocol is in ft-stream.cpp.
//
// Commands (on @ft_screens, from ft-layout):
//   remote <N> start <client> <host> <app> <W>x<H> <fps> <kbit/s> <metres> [label]
//       runs remote screen N (FT_REMOTE_FIRST and up): ft-stream's client name (its paired
//       certificate), the host's address, what to stream (display:DEVICE, monitor, primary),
//       the stream's size and rate (kbit/s 0: ft-stream picks), the panel's width until the
//       layout says, and the panel's name. Again with the same settings: nothing changes;
//       with others, the stream starts over.
//   remote <N> stop
//   remotes  -> "ok <count> <N>:<client>:<state>:<W>x<H> ..."   (state: queued, starting,
//               connecting, live, lost)
// A host's streams start one after another ("queued" until then): see host_busy.
// A stream that ends starts again, after 2 s, then longer after quick failures (up to 30 s).
// Its panel keeps the last picture meanwhile. ft-stream logs to
// $XDG_RUNTIME_DIR/frametop-remote-<N>.log.
#define _GNU_SOURCE
#include "remote.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WLR_USE_UNSTABLE
#include <wlr/util/log.h>

extern char **environ;

struct remote {
    bool used;
    int index;  // the screen's index (its number - 1)
    char client[64], host[64], app[192], label[96];
    int width, height, fps, bitrate;
    double metres;
    pid_t pid;  // ft-stream, 0 when not running
    int fd;     // our end of its socket, -1 when not connected
    struct wl_event_source *source;
    struct ft_dmabuf ring[3];  // its buffers; each is also the key its SteamVR import is kept under
    bool have_ring;
    int shown;             // the ring buffer on the panel, -1 none
    char state[96];        // as ft-stream last said
    int attention;         // what it was last told, -1 nothing yet
    uint32_t restart_at;   // ms: start it then (0: not waiting to)
    uint32_t started_ms;
    int failures;          // quick ends in a row, for the back-off
};

static struct remote g_remotes[FT_REMOTE_MAX];
static struct wl_event_loop *g_loop;
static bool g_vr;
static char g_stream[PATH_MAX];  // ft-stream (found at start: a rebuild replaces our own file)
// Which remote screen each mouse button (BTN_LEFT + n) went down on, -1 none. Vibepollo takes a
// button's release only from the client that pressed it (mouse_press_owner in its input.cpp),
// so a window carried onto another of the host's displays is dropped through the stream it was
// picked up on: the release there went to the display under the laser, and the window stayed
// on the pointer (2026-10-07). The pointer's moves still go to the display it's on.
static int g_pressed_on[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static struct remote *find(int index) {
    for (int i = 0; i < FT_REMOTE_MAX; ++i)
        if (g_remotes[i].used && g_remotes[i].index == index) return &g_remotes[i];
    return NULL;
}

static void say(struct remote *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(struct remote *r, const char *fmt, ...) {
    if (r->fd < 0) return;
    char msg[600];
    va_list a;
    va_start(a, fmt);
    const int n = vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    if (n > 0 && n < (int)sizeof msg) send(r->fd, msg, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
}

// Drops the ring's imports and closes its buffers (a new ring came, or the screen went).
static void forget_ring(struct remote *r) {
    if (!r->have_ring) return;
    for (int i = 0; i < 3; ++i) {
        ft_vr_forget(&r->ring[i]);
        close(r->ring[i].fd[0]);
    }
    r->have_ring = false;
    r->shown = -1;
}

static void disconnect(struct remote *r) {
    if (r->source) wl_event_source_remove(r->source);
    r->source = NULL;
    if (r->fd >= 0) close(r->fd);
    r->fd = -1;
}

static void message(struct remote *r, const char *msg, const int *fds, int nfds) {
    unsigned w, h, format, offset, stride;
    unsigned long long modifier;
    int i;
    if (strncmp(msg, "state ", 6) == 0) {
        snprintf(r->state, sizeof r->state, "%.95s", msg + 6);
        wlr_log(WLR_INFO, "remote %d (%s): %s", r->index + 1, r->client, r->state);
        if (strncmp(r->state, "live", 4) == 0) r->failures = 0;
    } else if (sscanf(msg, "buffers %u %u %x %llx %u %u", &w, &h, &format, &modifier, &offset, &stride) == 6 &&
               nfds == 3 && w > 0 && h > 0 && w <= 16384 && h <= 16384) {
        forget_ring(r);
        for (int k = 0; k < 3; ++k) {
            r->ring[k] = (struct ft_dmabuf){.width = (int)w, .height = (int)h, .format = format,
                                            .modifier = modifier, .n_planes = 1};
            r->ring[k].offset[0] = offset, r->ring[k].stride[0] = stride, r->ring[k].fd[0] = fds[k];
        }
        r->have_ring = true;
        wlr_log(WLR_INFO, "remote %d: %ux%u buffers, modifier 0x%llx", r->index + 1, w, h, modifier);
        return;  // the fds are the ring's now
    } else if (sscanf(msg, "frame %d", &i) == 1 && r->have_ring && i >= 0 && i < 3) {
        if (ft_vr_screen_present(r->index, &r->ring[i], &r->ring[i])) {
            if (r->shown >= 0 && r->shown != i) say(r, "release %d", r->shown);
            r->shown = i;
        } else {
            say(r, "release %d", i);
        }
    }
    for (int k = 0; k < nfds; ++k) close(fds[k]);
}

static void schedule_restart(struct remote *r) {
    const uint32_t ran = now_ms() - r->started_ms;
    r->failures = ran < 20000 ? r->failures + 1 : 0;
    uint32_t wait = 2000;
    for (int k = 1; k < r->failures && wait < 30000; ++k) wait *= 2;
    if (wait > 30000) wait = 30000;
    r->restart_at = now_ms() + wait;
    if (!r->restart_at) r->restart_at = 1;
    wlr_log(WLR_INFO, "remote %d: starting it again in %u s", r->index + 1, wait / 1000);
}

static int readable(int fd, uint32_t mask, void *data) {
    struct remote *r = data;
    for (;;) {
        char buf[512];
        char control[CMSG_SPACE(sizeof(int) * 4)];
        struct iovec iov = {buf, sizeof buf - 1};
        struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
        const ssize_t n = recvmsg(fd, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n <= 0) {  // ft-stream is gone: its exit (SIGCHLD) sets up the restart
            wlr_log(WLR_INFO, "remote %d: stream closed", r->index + 1);
            snprintf(r->state, sizeof r->state, "lost");
            disconnect(r);
            return 0;
        }
        buf[n] = 0;
        int fds[4], nfds = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const int got = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int k = 0; k < got; ++k) {
                int f;
                memcpy(&f, CMSG_DATA(c) + k * sizeof(int), sizeof f);
                if (nfds < 4) fds[nfds++] = f;
                else close(f);
            }
        }
        message(r, buf, fds, nfds);
    }
    return 0;
}

// <repo>/screens/build/ft-screens -> <repo>/stream/build/ft-stream, or $FT_STREAM.
static bool stream_path(char *out, size_t size) {
    const char *env = getenv("FT_STREAM");
    if (env && *env) return snprintf(out, size, "%s", env) < (int)size;
    char exe[PATH_MAX];
    if (!realpath("/proc/self/exe", exe)) return false;  // gone already: ft-screens was rebuilt
    for (int up = 0; up < 3; ++up) {
        char *slash = strrchr(exe, '/');
        if (!slash) return false;
        *slash = 0;
    }
    return snprintf(out, size, "%s/stream/build/ft-stream", exe) < (int)size;
}

static void log_path(const struct remote *r, char *out, size_t size) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    snprintf(out, size, "%s/frametop-remote-%d.log", runtime && *runtime ? runtime : "/tmp", r->index + 1);
}

static bool start_stream(struct remote *r) {
    r->restart_at = 0;
    r->started_ms = now_ms();
    snprintf(r->state, sizeof r->state, "lost");
    char *exe = g_stream, log[PATH_MAX];
    if (!*exe) return false;
    log_path(r, log, sizeof log);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return false;
    if (sv[1] == 3) {  // dup2 onto itself would keep close-on-exec
        const int moved = fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
        close(sv[1]);
        sv[1] = moved;
    }
    char size[32], fps[16], bitrate[16];
    snprintf(size, sizeof size, "%dx%d", r->width, r->height);
    snprintf(fps, sizeof fps, "%d", r->fps);
    snprintf(bitrate, sizeof bitrate, "%d", r->bitrate);
    char *argv[] = {exe,  "stream", r->host,  "--id", r->client, "--fd",      "3",     "--app", r->app,
                    "--size", size, "--fps", fps,     "--bitrate", bitrate, NULL};
    posix_spawn_file_actions_t io;
    posix_spawn_file_actions_init(&io);
    posix_spawn_file_actions_addopen(&io, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&io, 1, log, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600);
    posix_spawn_file_actions_adddup2(&io, 1, 2);
    posix_spawn_file_actions_adddup2(&io, sv[1], 3);
    // Not our event loop's blocked signals (SIGTERM, SIGINT, SIGCHLD go to its signalfd): with
    // SIGTERM blocked, ft-stream heard neither kill nor its death signal.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
    pid_t pid;
    const int rc = posix_spawn(&pid, exe, &io, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&io);
    close(sv[1]);
    if (rc != 0) {
        wlr_log(WLR_ERROR, "remote %d: can't run %s: %s", r->index + 1, exe, strerror(rc));
        close(sv[0]);
        return false;
    }
    r->pid = pid;
    r->fd = sv[0];
    fcntl(r->fd, F_SETFL, O_NONBLOCK);
    r->source = wl_event_loop_add_fd(g_loop, r->fd, WL_EVENT_READABLE, readable, r);
    r->attention = -1;
    snprintf(r->state, sizeof r->state, "starting");
    // What SteamVR imports: ft-stream allocates its ring with one of these.
    uint64_t mods[32];
    const int n = ft_vr_modifiers(DRM_FORMAT_ABGR8888, mods, 32);
    char msg[600];
    int at = snprintf(msg, sizeof msg, "modifiers");
    for (int k = 0; k < n && at < (int)sizeof msg - 20; ++k) at += snprintf(msg + at, sizeof msg - at, " %llx", (unsigned long long)mods[k]);
    send(r->fd, msg, (size_t)at, MSG_NOSIGNAL | MSG_DONTWAIT);
    wlr_log(WLR_INFO, "remote %d: started ft-stream (pid %d) for %s on %s, %s at %d fps (log %s)", r->index + 1, pid,
            r->app, r->host, size, r->fps, log);
    return true;
}

// Asks a running ft-stream to end (it releases a Remote Monitor on the way out).
static void stop_stream(struct remote *r) {
    say(r, "quit");
    disconnect(r);
    if (r->pid > 0) kill(r->pid, SIGTERM);
}

// Vibepollo takes one stream operation at a time ("Another stream operation is still
// running"), and a Remote Monitor appearing while another display's capture starts leaves that
// one without a picture (2026-10-07: three started at once, two never got a frame). So a host's
// streams start one after another: the next once the last is live, lost, or 20 s old.
static bool host_busy(const struct remote *r) {
    for (int i = 0; i < FT_REMOTE_MAX; ++i) {
        const struct remote *o = &g_remotes[i];
        if (o == r || !o->used || o->pid == 0 || strcmp(o->host, r->host) != 0) continue;
        const bool starting = !strncmp(o->state, "starting", 8) || !strncmp(o->state, "connecting", 10);
        if (starting && now_ms() - o->started_ms < 20000) return true;
    }
    return false;
}

void ft_remote_init(struct wl_event_loop *loop, bool vr) {
    g_loop = loop;
    g_vr = vr;
    if (!stream_path(g_stream, sizeof g_stream)) g_stream[0] = 0;
    for (int i = 0; i < FT_REMOTE_MAX; ++i) g_remotes[i].fd = -1, g_remotes[i].shown = -1;
}

void ft_remote_shutdown(void) {
    for (int i = 0; i < FT_REMOTE_MAX; ++i) {
        struct remote *r = &g_remotes[i];
        stop_stream(r);
        if (r->used) ft_vr_screen_destroy(r->index);  // the panel goes before its textures
        forget_ring(r);
    }
}

bool ft_remote_is(int index) { return find(index) != NULL; }

void ft_remote_event(const struct ft_event *e) {
    struct remote *r = find(e->screen);
    if (!r) return;
    const int w = r->have_ring ? r->ring[0].width : r->width, h = r->have_ring ? r->ring[0].height : r->height;
    switch (e->type) {
        case FT_MOTION:
            say(r, "move %.1f %.1f", e->x, e->y);
            break;
        case FT_BUTTON: {
            if (e->x >= 0 && e->y >= 0 && e->x < w && e->y < h) say(r, "move %.1f %.1f", e->x, e->y);
            struct remote *by = r;
            const int b = (int)e->button - BTN_LEFT;
            if (b >= 0 && b < 8) {
                if (e->pressed) {
                    g_pressed_on[b] = r->index;
                } else {
                    struct remote *p = find(g_pressed_on[b]);
                    if (p && p->fd >= 0) by = p;
                    g_pressed_on[b] = -1;
                }
            }
            say(by, "button %u %d", e->button, e->pressed ? 1 : 0);
            wlr_log(WLR_INFO, "remote %d: button %u %s at %.0f,%.0f%s", r->index + 1, e->button, e->pressed ? "down" : "up",
                    e->x, e->y, by != r ? " (through the stream it went down on)" : "");
            break;
        }
        case FT_SCROLL:
            say(r, "scroll %.3f %.3f", e->dx, e->dy);
            break;
        default:
            break;
    }
}

void ft_remote_key(int index, uint32_t code, bool pressed) {
    struct remote *r = find(index);
    if (r) say(r, "key %u %d", code, pressed ? 1 : 0);
}

void ft_remote_blur(int index) {
    struct remote *r = find(index);
    if (r) say(r, "blur");
}

void ft_remote_tick(void) {
    static const char *const names[] = {"hidden", "view", "focused"};  // enum ft_attention
    const uint32_t t = now_ms();
    for (int i = 0; i < FT_REMOTE_MAX; ++i) {
        struct remote *r = &g_remotes[i];
        if (r->used && r->restart_at && (int32_t)(t - r->restart_at) >= 0 && r->pid == 0 && r->fd < 0 &&
            !host_busy(r) && !start_stream(r))
            schedule_restart(r);
        if (!r->used || r->fd < 0) continue;
        const enum ft_attention a = ft_vr_screen_attention(r->index);
        if ((int)a == r->attention) continue;
        r->attention = (int)a;
        say(r, "attention %s", names[a]);
    }
}

bool ft_remote_child(pid_t pid, int status) {
    for (int i = 0; i < FT_REMOTE_MAX; ++i) {
        struct remote *r = &g_remotes[i];
        if (r->pid != pid) continue;
        r->pid = 0;
        if (WIFEXITED(status)) wlr_log(WLR_INFO, "remote %d: ft-stream exited (%d)", r->index + 1, WEXITSTATUS(status));
        else wlr_log(WLR_INFO, "remote %d: ft-stream killed (signal %d)", r->index + 1, WTERMSIG(status));
        disconnect(r);
        if (r->used && !r->restart_at) schedule_restart(r);
        return true;
    }
    return false;
}

// Names, addresses and app ids go to ft-stream's command line: letters, digits and a few
// marks only (a display's device id is {GUID}).
static bool plain(const char *s, const char *extra) {
    if (!*s) return false;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || strchr(extra, *s)))
            return false;
    return true;
}

bool ft_remote_command(const char *cmd, char *reply, int size) {
    if (strcmp(cmd, "remotes") == 0) {
        int n = 0;
        for (int i = 0; i < FT_REMOTE_MAX; ++i) n += g_remotes[i].used;
        int at = snprintf(reply, size, "ok %d", n);
        for (int i = 0; i < FT_REMOTE_MAX && at < size; ++i) {
            const struct remote *r = &g_remotes[i];
            if (!r->used) continue;
            char state[32];
            sscanf(r->state, "%31s", state);
            at += snprintf(reply + at, size - at, " %d:%s:%s:%dx%d", r->index + 1, r->client, state,
                           r->have_ring ? r->ring[0].width : r->width, r->have_ring ? r->ring[0].height : r->height);
        }
        return true;
    }
    int number;
    char word[16];
    if (sscanf(cmd, "remote %d %15s", &number, word) != 2) return false;
    if (number < FT_REMOTE_FIRST || number >= FT_REMOTE_FIRST + FT_REMOTE_MAX) {
        snprintf(reply, size, "error remote screens are %d to %d", FT_REMOTE_FIRST, FT_REMOTE_FIRST + FT_REMOTE_MAX - 1);
        return true;
    }
    const int index = number - 1;
    struct remote *r = find(index);
    if (strcmp(word, "stop") == 0) {
        if (!r) return snprintf(reply, size, "error no remote screen %d", number), true;
        stop_stream(r);
        ft_vr_screen_destroy(index);  // the panel goes before its textures
        forget_ring(r);
        r->used = false;  // keeps its pid until the exit is reaped
        r->restart_at = 0;
        wlr_log(WLR_INFO, "remote %d: stopped", number);
        return snprintf(reply, size, "ok"), true;
    }
    if (strcmp(word, "info") == 0) {
        // Its stream's whole state ("lost can't connect"), for Remote Displays.
        if (!r) return snprintf(reply, size, "error no remote screen %d", number), true;
        return snprintf(reply, size, "ok %s", r->state), true;
    }
    if (strcmp(word, "start") != 0) return snprintf(reply, size, "error remote <N> start|stop|info"), true;
    if (!g_vr) return snprintf(reply, size, "error no SteamVR (--no-vr)"), true;
    struct remote c = {0};
    int label_at = 0;
    bool restored = false;  // its panel is back where it was before a stop (vr.cpp, Parked)
    if (sscanf(cmd, "remote %*d start %63s %63s %191s %dx%d %d %d %lf %n", c.client, c.host, c.app, &c.width, &c.height,
               &c.fps, &c.bitrate, &c.metres, &label_at) != 8)
        return snprintf(reply, size, "error remote <N> start <client> <host> <app> <W>x<H> <fps> <kbit/s> <metres> [label]"), true;
    if (!plain(c.client, "._-") || !plain(c.host, ".:-") || !plain(c.app, "{}._:-"))
        return snprintf(reply, size, "error client, host or app has other characters"), true;
    if (c.width < 64 || c.height < 64 || c.width > 8192 || c.height > 8192 || c.fps < 1 || c.fps > 240 ||
        c.bitrate < 0 || c.bitrate > 500000 || !(c.metres >= 0.15 && c.metres <= 20))
        return snprintf(reply, size, "error size, fps, bitrate or width out of range"), true;
    snprintf(c.label, sizeof c.label, "%s", label_at && cmd[label_at] ? cmd + label_at : c.client);
    const bool same = r && !strcmp(r->client, c.client) && !strcmp(r->host, c.host) && !strcmp(r->app, c.app) &&
                      r->width == c.width && r->height == c.height && r->fps == c.fps && r->bitrate == c.bitrate;
    if (same) return snprintf(reply, size, "ok running"), true;
    if (!r) {
        for (int i = 0; i < FT_REMOTE_MAX && !r; ++i)
            if (!g_remotes[i].used && g_remotes[i].pid == 0) r = &g_remotes[i];
        if (!r) return snprintf(reply, size, "error too many remote screens"), true;
        if (!ft_vr_remote_create(index, c.label, c.metres, &restored))
            return snprintf(reply, size, "error SteamVR made no panel (too many overlays?)"), true;
        *r = (struct remote){.used = true, .index = index, .fd = -1, .shown = -1, .attention = -1};
    } else {
        stop_stream(r);  // new settings: it starts over once the old one has gone
    }
    memcpy(r->client, c.client, sizeof r->client);
    memcpy(r->host, c.host, sizeof r->host);
    memcpy(r->app, c.app, sizeof r->app);
    memcpy(r->label, c.label, sizeof r->label);
    r->width = c.width, r->height = c.height, r->fps = c.fps, r->bitrate = c.bitrate, r->metres = c.metres;
    if (!*g_stream) return snprintf(reply, size, "error no ft-stream next to ft-screens (or $FT_STREAM)"), true;
    char log[PATH_MAX];
    log_path(r, log, sizeof log);
    const int f = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);  // a fresh log
    if (f >= 0) close(f);
    // ft_remote_tick starts it: when its host is free, and once an old stream's exit is reaped.
    r->restart_at = now_ms() | 1;
    snprintf(r->state, sizeof r->state, "queued");
    return snprintf(reply, size, restored ? "ok restored" : "ok"), true;
}
