// Offline test of the gaze calibration panel's answers (pointer/helper/calpanel.h): which relay
// messages take the dot, close the panel, or do nothing while it's up. Needs no SteamVR.
//
//   pointer/test/calpanel-test.sh
#include "../helper/calpanel.h"

#include <cstdio>

int main() {
    struct Case {
        const char *msg;
        CalPanelAnswer want;
    } cases[] = {
        {"btn trigger 1", CalPanelAnswer::Accept},
        {"gazekey left 1", CalPanelAnswer::Accept},
        {"precision mouse 1", CalPanelAnswer::Accept},
        {"precision keyboard 1", CalPanelAnswer::Accept},
        {"gazedrag mouse 1", CalPanelAnswer::Accept},
        {"gazedrag keyboard 1", CalPanelAnswer::Accept},
        {"btn b 1", CalPanelAnswer::Quit},
        {"gazekey right 1", CalPanelAnswer::Quit},
        {"btn trigger 0", CalPanelAnswer::Ignore},
        {"btn b 0", CalPanelAnswer::Ignore},
        {"btn a 1", CalPanelAnswer::Ignore},  // the relay's laser claim
        {"btn x 1", CalPanelAnswer::Ignore},
        {"btn system 1", CalPanelAnswer::Ignore},
        {"gazekey left 0", CalPanelAnswer::Ignore},
        {"gazekey right 0", CalPanelAnswer::Ignore},
        {"precision mouse 0", CalPanelAnswer::Ignore},
        {"gazedrag mouse 0", CalPanelAnswer::Ignore},
        {"move 0.1000 -0.2000", CalPanelAnswer::None},
        {"show", CalPanelAnswer::None},
        {"recenter", CalPanelAnswer::None},
        {"scroll 0 1", CalPanelAnswer::None},
        {"typing", CalPanelAnswer::None},
    };
    const char *names[] = {"none", "accept", "quit", "ignore"};
    int failed = 0;
    for (const Case &c : cases) {
        const CalPanelAnswer got = calPanelAnswer(c.msg);
        if (got != c.want) {
            std::printf("FAIL \"%s\": %s, want %s\n", c.msg, names[int(got)], names[int(c.want)]);
            ++failed;
        }
    }
    std::printf("%s: %d of %zu cases\n", failed ? "FAILED" : "ok", int(sizeof cases / sizeof cases[0]) - failed,
                sizeof cases / sizeof cases[0]);
    return failed ? 1 : 0;
}
