// The gaze calibration panel's answers (see ft-pointer.cpp's top): while ft-gazed says the panel
// is up, a press from the relay answers it instead of clicking. Here so it can be tested without
// SteamVR (pointer/test/calpanel-test.sh).
//
// Accept (take this dot): the left button's press ("btn trigger 1", whatever mouse button,
// controller button or key combination is mapped to the left action), Meta+J ("gazekey left 1"),
// and a gaze precision or gaze drag press ("precision|gazedrag <source> 1"): those are the left
// button for someone who mapped it to one, and were dropped until 2026-10-09, so the panel
// never took a dot from them. Quit: the right button ("btn b 1") and Meta+K ("gazekey right 1").
// Their releases, and the other buttons, do nothing while the panel is up.
#pragma once

#include <cstdio>
#include <cstring>

enum class CalPanelAnswer { None, Accept, Quit, Ignore };

inline CalPanelAnswer calPanelAnswer(const char *msg) {
    char source[16];
    int value;
    if (!std::strncmp(msg, "btn trigger 1", 13) || !std::strncmp(msg, "gazekey left 1", 14))
        return CalPanelAnswer::Accept;
    if ((std::sscanf(msg, "precision %15s %d", source, &value) == 2 ||
         std::sscanf(msg, "gazedrag %15s %d", source, &value) == 2) && value == 1)
        return CalPanelAnswer::Accept;
    if (!std::strncmp(msg, "btn b 1", 7) || !std::strncmp(msg, "gazekey right 1", 15))
        return CalPanelAnswer::Quit;
    if (!std::strncmp(msg, "btn ", 4) || !std::strncmp(msg, "gazekey ", 8) ||
        !std::strncmp(msg, "precision ", 10) || !std::strncmp(msg, "gazedrag ", 9))
        return CalPanelAnswer::Ignore;
    return CalPanelAnswer::None;
}
