// Offline test of ft-gaze's eye-server.mmap layout detection (EyeFile::Detect in
// gaze/ft-gaze.cpp) and of reading a sample at the layout it found, on a made-up file in
// memory: no SteamVR, no eye tracker. Detect takes the time as an argument, so the passes
// of ft-gaze's loop are played here with a made-up clock. mmap-layout-test.sh builds and
// runs it in the dev container.
#define main ft_gaze_main
#include "../ft-gaze.cpp"
#undef main

#include <cstdio>
#include <vector>

namespace {

int failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c), ++failures; \
    } while (0)

// The file as the eye server writes it, one sample at a time, with every field from the
// timestamp on moved by `shift` (0 stable, 5 the 0.4.x beta).
struct File {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(324122);  // eye-server.mmap's size
    EyeFile eyes;
    uint32_t n = 0;
    File() { eyes.p = bytes.data(), eyes.size = bytes.size(); }
    template <class T> void Put(size_t off, const T &v) { std::memcpy(bytes.data() + off, &v, sizeof v); }
    void Sample(size_t shift, double t, bool leftLost = false) {
        const float left[3] = {0.05f, 0.02f, -0.9985f}, right[3] = {-0.05f, 0.02f, -0.9985f}, none[3] = {};
        Put(kCounter, ++n);
        Put(kTime + shift, t);
        Put(kLeft1 + shift, leftLost ? none : left);
        Put(kRight1 + shift, right);
        Put(kLeft2 + shift, left);
        Put(kRight2 + shift, right);
        const float open[2] = {0.8f, 0.7f}, var[6] = {1e-3f, 2e-3f, 3e-3f, 4e-3f, 5e-3f, 6e-3f};
        const float meas[8] = {0.1f, 0.2f, 0.3f, 0.4f, 2e-5f, 3e-5f, 4e-5f, 5e-5f}, fix[3] = {0, 0.02f, -0.6f};
        Put(kFix1 + shift, fix);
        Put(kVar1 + shift, var);
        Put(kVar2 + shift, var);
        Put(kOpen + shift, open);
        Put(kMeas + shift, meas);
    }
};

constexpr double kStart = 5000;  // the made-up CLOCK_MONOTONIC_RAW at the first pass
constexpr double kPass = 0.004;  // ft-gaze's loop
constexpr double kAge = 0.017;   // how old a sample is when it appears

// Samples every 1/hz s in `shift`'s layout, Detect called on every loop pass from the first
// sample on (as ft-gaze does once the counter moved), until it decides or `until` s pass.
// Returns the outcome and when (s after the first sample) in `at`.
EyeFile::Detection Run(File &f, size_t shift, double hz, double until, double &at) {
    double next = kStart;
    for (double now = kStart; now < kStart + until; now += kPass) {
        if (now >= next) f.Sample(shift, now - kAge), next += 1 / hz;
        const EyeFile::Detection d = f.eyes.Detect(now);
        if (d != EyeFile::kWaiting) {
            at = now - kStart;
            return d;
        }
    }
    at = until;
    return EyeFile::kWaiting;
}

void Layouts() {
    for (const size_t shift : {size_t(0), size_t(5)}) {
        File f;
        double at;
        CHECK(Run(f, shift, 90, 2, at) == EyeFile::kFound);
        CHECK(f.eyes.known && f.eyes.shift == shift);
        CHECK(at <= 1 / 90.0 + kPass);  // the next sample confirms it
    }
}

void SlowWriters() {
    // 15 a second, as seen on the beta: the next sample, 67 ms on, confirms it (PR #26's
    // fixed 60 ms wait missed about 1 try in 10 there).
    File beta;
    double at;
    CHECK(Run(beta, 5, 15, 2, at) == EyeFile::kFound && beta.eyes.shift == 5);
    CHECK(at > 1 / 15.0 - kPass && at <= 1 / 15.0 + kPass);
    // 3 a second still works (within kConfirm); 1 a second can't confirm in time.
    File slow;
    CHECK(Run(slow, 0, 3, 2, at) == EyeFile::kFound && slow.eyes.shift == 0);
    File slower;
    CHECK(Run(slower, 0, 1, 2, at) == EyeFile::kNone && !slower.eyes.known);
    CHECK(at > EyeFile::kConfirm && at < EyeFile::kConfirm + 2 * kPass);
    CHECK(!slower.eyes.Detecting());  // and the next try starts over
}

void Refused() {
    // A layout we don't know: the same fields, moved by some other amount.
    for (size_t shift = 1; shift <= 16; ++shift) {
        if (shift == 5) continue;
        File f;
        double at;
        const EyeFile::Detection d = Run(f, shift, 90, 1, at);
        CHECK(d == EyeFile::kNone && !f.eyes.known);
        if (d != EyeFile::kNone) std::printf("  (moved by %zu)\n", shift);
    }
    // A server that stopped: its last sample is 10 s old. Refused at once, no waiting.
    File stale;
    stale.Sample(0, kStart - 10);
    CHECK(stale.eyes.Detect(kStart) == EyeFile::kNone && !stale.eyes.Detecting());
    // One that stopped just now: its timestamp fits but never moves on.
    File stopped;
    stopped.Sample(0, kStart - kAge);
    CHECK(stopped.eyes.Detect(kStart) == EyeFile::kWaiting);
    EyeFile::Detection d = EyeFile::kWaiting;
    double now = kStart;
    while (d == EyeFile::kWaiting && now < kStart + 2) d = stopped.eyes.Detect(now += kPass);
    CHECK(d == EyeFile::kNone && !stopped.eyes.known);
    // A set-1 direction that isn't a unit vector (here zeros).
    File warm;
    warm.Sample(0, kStart - kAge, true);
    CHECK(warm.eyes.Detect(kStart) == EyeFile::kNone);
    // An empty file (the server never wrote).
    File empty;
    CHECK(empty.eyes.Detect(kStart) == EyeFile::kNone);
}

void TornWrite() {
    // A pass that reads the timestamp mid-write (the counter already moved on, the top half
    // of the new timestamp not written yet, so it's nowhere near the clock) keeps waiting:
    // the next pass confirms it.
    File f;
    f.Sample(0, kStart - kAge);
    CHECK(f.eyes.Detect(kStart) == EyeFile::kWaiting);
    const double next = kStart + 0.011 - kAge;
    std::memcpy(f.bytes.data() + kTime, &next, 4);
    std::memset(f.bytes.data() + kTime + 4, 0, 4);
    f.Put(kCounter, ++f.n);
    CHECK(f.eyes.Detect(kStart + 0.012) == EyeFile::kWaiting);
    f.Put(kTime, next);
    CHECK(f.eyes.Detect(kStart + 0.016) == EyeFile::kFound && f.eyes.shift == 0);
}

void ReadsTheLayoutFound() {
    // After detection on the beta, a sample comes from the moved fields.
    File f;
    double at;
    CHECK(Run(f, 5, 90, 1, at) == EyeFile::kFound && f.eyes.shift == 5);
    f.Sample(5, kStart + 1);
    EyeSample s;
    CHECK(ReadSample(f.eyes, s));
    CHECK(s.n == f.n && s.t == kStart + 1);
    CHECK(std::fabs(s.left1.x - 0.05) < 1e-6 && std::fabs(s.right2.x + 0.05) < 1e-6);
    CHECK(std::fabs(s.fix1.z + 0.6) < 1e-6);
    CHECK(s.open[0] == 0.8f && s.open[1] == 0.7f);
    CHECK(s.var1[5] == 6e-3f && s.var2[0] == 1e-3f && s.meas[3] == 0.4f && s.meas[7] == 5e-5f);
}

void NoMapping() {
    // ft-gaze without the file: the loop's one read of it gives 0, and touches no memory.
    EyeFile none;
    CHECK(none.Counter() == 0);
    File f;
    f.Sample(0, kStart);
    CHECK(f.eyes.Counter() == f.n);
}

}  // namespace

int main() {
    Layouts();
    SlowWriters();
    Refused();
    TornWrite();
    ReadsTheLayoutFound();
    NoMapping();
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
