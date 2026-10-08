// Remote screens (remote.c): displays of other machines, streamed by ft-stream
// (stream/ft-stream.cpp) and shown as panels of their own, with every screen's controls.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <wayland-server-core.h>

#include "vr.h"

// Remote screens are numbered from here (from 1, like the others), clear of KWin's screens
// and spare outputs, so their numbers don't move when the desktop's screens change.
#define FT_REMOTE_FIRST 101
#define FT_REMOTE_MAX 16

// vr: connected to SteamVR (without it, no stream starts: there'd be no panel).
void ft_remote_init(struct wl_event_loop *loop, bool vr);
void ft_remote_shutdown(void);
// Whether screen `index` (from 0) is a remote screen that's running.
bool ft_remote_is(int index);
// Pointer input on a remote screen's panel.
void ft_remote_event(const struct ft_event *e);
// A key (linux KEY_* code) for remote screen `index`.
void ft_remote_key(int index, uint32_t code, bool pressed);
// Typing went elsewhere: the keys it holds on the host come up.
void ft_remote_blur(int index);
// Every tick: each stream hears how much of its screen you see, and lost ones restart.
void ft_remote_tick(void);
// A child process exited: true if it was one of ours.
bool ft_remote_child(pid_t pid, int status);
// "remote ..." and "remotes" commands; false for anything else.
bool ft_remote_command(const char *cmd, char *reply, int size);
