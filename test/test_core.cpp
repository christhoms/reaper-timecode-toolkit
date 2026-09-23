// Tests for Reaper Timecode Toolkit: timecode maths, LTC decoding (synthetic LTC at 24 / 25 / 29.97 DF / 30), the
// Art-Net sender's packet content + timing, and the built .clap loaded through its real entry point.
//   ./test_core [path to the plugin binary]
#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <vector>

#include <clap/clap.h>

#include "ltc_core.h"

using namespace ctltc;

#ifdef _WIN32
static const char *kGuiApi = CLAP_WINDOW_API_WIN32;
static void *lib_open(const char *p) { return (void *)LoadLibraryA(p); }
static void *lib_sym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static void lib_close(void *h) { FreeLibrary((HMODULE)h); }
static void set_env(const char *k, const char *v) { _putenv_s(k, v); }
#else
static const char *kGuiApi = CLAP_WINDOW_API_COCOA;
static void *lib_open(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *lib_sym(void *h, const char *n) { return dlsym(h, n); }
static void lib_close(void *h) { dlclose(h); }
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) { g_fail++; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } \
  } while (0)

// ---- LTC generator (biphase mark, 80 bits per frame) ---------------------------------------------------------
struct LtcGen {
  double srate, fpsReal;
  int fps;
  bool df;
  long count;
  double phase = 0;  // position inside the current half-bit, in samples
  int level = 1;
  std::vector<int> halves;  // queue of half-bit "transition at start?" flags for the current frame
  size_t hi = 0;

  LtcGen(double sr, int fpsNom, double real, bool dropFrame, long start) : srate(sr), fpsReal(real), fps(fpsNom), df(dropFrame), count(start) { fill(); }

  void fill() {
    Timecode t = count_to_tc(count, fps, df);
    int bits[80] = {0};
    auto put = [&](int pos, int nbits, int v) { for (int i = 0; i < nbits; i++) bits[pos + i] = (v >> i) & 1; };
    put(0, 4, t.f % 10); put(8, 2, t.f / 10); bits[10] = df ? 1 : 0;
    put(16, 4, t.s % 10); put(24, 3, t.s / 10);
    put(32, 4, t.m % 10); put(40, 3, t.m / 10);
    put(48, 4, t.h % 10); put(56, 2, t.h / 10);
    static const int sync[16] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1};
    for (int i = 0; i < 16; i++) bits[64 + i] = sync[i];
    halves.clear();
    for (int b = 0; b < 80; b++) { halves.push_back(1); halves.push_back(bits[b]); }  // boundary always flips; mid flips for a 1
    hi = 0;
  }

  float next() {
    const double half = srate / fpsReal / 160.0;
    if (phase <= 0) {
      if (hi >= halves.size()) { count = (count + 1) % frames_per_day(fps, df); fill(); }
      if (halves[hi++]) level = -level;
      phase += half;
    }
    phase -= 1.0;
    return 0.5f * float(level);
  }
};

static void test_math() {
  std::printf("timecode maths\n");
  for (int fps : {24, 25, 30}) {
    for (long n : {0L, 1L, 1799L, 1800L, 17981L, 17982L, 107892L, 86400L * fps - 1}) {
      Timecode t = count_to_tc(n, fps, false);
      CHECK(tc_to_count(t.h, t.m, t.s, t.f, fps, false) == n, "ndf roundtrip fps %d n %ld", fps, n);
    }
  }
  for (long n = 0; n < 2589408L; n += 7) {
    Timecode t = count_to_tc(n, 30, true);
    if (tc_to_count(t.h, t.m, t.s, t.f, 30, true) != n || (t.s == 0 && t.f < 2 && t.m % 10 != 0)) { CHECK(false, "df roundtrip n %ld -> %02d:%02d:%02d;%02d", n, t.h, t.m, t.s, t.f); break; }
  }
  Timecode a = count_to_tc(1799, 30, true), b = count_to_tc(1800, 30, true);
  CHECK(a.m == 0 && a.s == 59 && a.f == 29 && b.m == 1 && b.s == 0 && b.f == 2, "df minute rollover");
}

static void test_decoder() {
  std::printf("LTC decoder\n");
  struct Case { const char *name; int fps; double real; bool df; } cases[] = {
      {"30", 30, 30.0, false}, {"29.97 DF", 30, 30000.0 / 1001.0, true}, {"29.97 NDF", 30, 30000.0 / 1001.0, false}, {"25", 25, 25.0, false}, {"24", 24, 24.0, false}};
  for (double sr : {44100.0, 48000.0, 96000.0}) {
    for (auto &c : cases) {
      long start = tc_to_count(1, 0, 58, 0, c.fps, c.df);  // crosses 01:01:00 (drop-frame skips ;00 ;01 there)
      LtcGen gen(sr, c.fps, c.real, c.df, start);
      Decoder d;
      d.reset(sr);
      Frame fr;
      long expect = -1, good = 0, bad = 0;
      double lastFps = 0;
      for (long i = 0; i < long(sr * 6); i++) {
        if (!d.push(gen.next(), fr)) continue;
        long got = tc_to_count(fr.h, fr.m, fr.s, fr.f, fr.fps, fr.df);
        if (expect >= 0 && got != expect) bad++; else good++;
        expect = got + 1;
        lastFps = fr.fpsMeasured;
        if (fr.fps != c.fps || fr.df != c.df) bad++;
      }
      const bool rateOk = std::fabs(lastFps - c.real) < 0.005;
      CHECK(good > c.fps * 4 && bad == 0 && rateOk, "%s @ %.0f Hz: %ld good, %ld bad, measured %.4f fps", c.name, sr, good, bad, lastFps);
      Frame probe; probe.fps = c.fps; probe.df = c.df; probe.fpsMeasured = lastFps;
      CHECK(probe.is2997() == (c.real < 29.99 && c.fps == 30), "%s: 29.97 detection", c.name);
    }
  }
  // level: the threshold follows the signal, so no gain stage is needed in front of the decoder
  for (double db : {0.0, -20.0, -40.0, -55.0}) {
    const float g = float(std::pow(10.0, db / 20.0) / 0.5);  // generator peaks at 0.5
    LtcGen gen(48000, 30, 30.0, false, tc_to_count(2, 0, 0, 0, 30, false));
    Decoder dl; dl.reset(48000); Frame f2; long okf = 0; double first = -1;
    for (long i = 0; i < 48000 * 3; i++) if (dl.push(gen.next() * g, f2)) { okf++; if (first < 0) first = i / 48000.0; }
    std::printf("  LTC at %5.1f dBFS peak: %ld frames in 3 s, first after %.0f ms\n", db, okf, first * 1e3);
    CHECK(okf > 80, "LTC at %.0f dBFS: only %ld frames", db, okf);
  }
  // noise / silence must not produce frames
  Decoder d; d.reset(48000); Frame fr; int n = 0; unsigned s = 1;
  for (int i = 0; i < 48000 * 3; i++) { s = s * 1664525u + 1013904223u; if (d.push(((s >> 8) & 0xffff) / 32768.0f - 1.0f, fr)) n++; }
  CHECK(n == 0, "white noise decoded %d frames", n);
}


// ---- coasting over damaged LTC ---------------------------------------------------------------------------------
// Runs generator -> damage -> Decoder -> Coaster and records every frame-end event with its boundary sample.
struct CoastRun {
  struct Ev { long count; double samp; bool synth; };
  std::vector<Ev> ev;
  std::vector<std::pair<long, long>> truth;  // (frame count, sample where that frame ENDED)
  long checked = 0;                          // frames the bare decoder would have reported
};
template <class Damage, class Source>
static CoastRun coast_run(double sr, int limit, long nsamples, Source &&src, Damage &&damaged) {
  CoastRun R;
  Decoder d; d.reset(sr);
  Coaster c; c.reset(sr); c.setLimit(limit);
  Frame raw, fr;
  for (long i = 0; i < nsamples; i++) {
    long endedCount = -1;
    float x = src(i, endedCount);
    if (endedCount >= 0) R.truth.push_back({endedCount, i});
    x = damaged(i, x);
    const int kind = d.pushRaw(x, raw);
    if (kind == 2) R.checked++;
    double back = 0; bool synth = false;
    if (c.tick(kind, raw, d.sinceEdge(), fr, back, synth)) R.ev.push_back({tc_to_count(fr.h, fr.m, fr.s, fr.f, fr.fps, fr.df), double(i + 1) - back, synth});
  }
  return R;
}
// worst distance (samples) between an event and the true end of that frame; -1 when an event names a frame that never ended
static double coast_worst(const CoastRun &R) {
  double worst = 0;
  for (auto &e : R.ev) {
    bool found = false;
    for (auto &t : R.truth) if (t.first == e.count) { worst = std::max(worst, std::fabs(e.samp - double(t.second))); found = true; break; }
    if (!found) return -1;
  }
  return worst;
}

static void test_coaster() {
  std::printf("coasting\n");
  const double sr = 48000;
  const long start = tc_to_count(3, 10, 0, 0, 30, false);
  for (double real : {30.0, 30000.0 / 1001.0}) {
    // every 3rd frame has a burst of inverted samples in the middle: the bare decoder loses that frame AND the next one
    LtcGen gen(sr, 30, real, false, start);
    long prev = gen.count, inFrame = 0;
    auto src = [&](long, long &ended) { float x = gen.next(); if (gen.count != prev) { ended = prev; prev = gen.count; inFrame = 0; } inFrame++; return x; };
    auto dmg = [&](long i, float x) { return (i > long(sr) && gen.count % 3 == 0 && inFrame > 500 && inFrame < 700) ? -x : x; };
    CoastRun on = coast_run(sr, 30, long(sr * 10), src, dmg);
    bool order = !on.ev.empty();
    long synth = 0;
    for (size_t i = 1; i < on.ev.size(); i++) order = order && on.ev[i].count == on.ev[i - 1].count + 1;
    for (auto &e : on.ev) synth += e.synth;
    const double worst = coast_worst(on);
    std::printf("  %.2f fps, every 3rd frame damaged: decoder alone %ld of ~300 frames, with coasting %zu (%ld filled in), worst boundary error %.1f samples\n",
                real, on.checked, on.ev.size(), synth, worst);
    CHECK(order && on.ev.size() >= 295 && on.checked < 150 && worst >= 0 && worst < 6.0, "damaged LTC must come out complete, in order and on time");

    LtcGen gen0(sr, 30, real, false, start);
    prev = gen0.count; inFrame = 0;
    auto src0 = [&](long, long &ended) { float x = gen0.next(); if (gen0.count != prev) { ended = prev; prev = gen0.count; inFrame = 0; } inFrame++; return x; };
    auto dmg0 = [&](long i, float x) { return (i > long(sr) && gen0.count % 3 == 0 && inFrame > 500 && inFrame < 700) ? -x : x; };
    CoastRun off = coast_run(sr, 0, long(sr * 10), src0, dmg0);
    CHECK((long)off.ev.size() == off.checked && off.checked == on.checked, "coast off = the decoder's checked frames only (%zu vs %ld)", off.ev.size(), off.checked);
  }
  {  // a stretch where NO frame decodes: filled in up to the limit, not beyond; clean LTC afterwards picks up again
    for (int badFrames : {20, 45}) {
      LtcGen gen(sr, 30, 30.0, false, start);
      long prev = gen.count;
      auto src = [&](long, long &ended) { float x = gen.next(); if (gen.count != prev) { ended = prev; prev = gen.count; } return x; };
      unsigned rs = 7;
      auto dmg = [&](long, float x) {
        const long k = gen.count - start;
        if (k < 60 || k >= 60 + badFrames) return x;
        rs = rs * 1664525u + 1013904223u;
        return ((rs >> 16) % 20 == 0) ? -x : x;  // 5% of the samples inverted: edges everywhere, nothing decodes
      };
      CoastRun R = coast_run(sr, 30, long(sr * 6), src, dmg);
      long gaps = 0, maxGap = 0;
      for (size_t i = 1; i < R.ev.size(); i++) { const long g = R.ev[i].count - R.ev[i - 1].count - 1; if (g != 0) { gaps++; maxGap = std::max(maxGap, g); } }
      long run = 0, longest = 0;
      for (auto &e : R.ev) { run = e.synth ? run + 1 : 0; longest = std::max(longest, run); }
      const double worst = coast_worst(R);
      std::printf("  %d undecodable frames in a row: longest filled-in run %ld, %ld gap(s), worst boundary error %.1f samples\n", badFrames, longest, gaps, worst);
      if (badFrames <= 30) CHECK(gaps == 0 && longest >= badFrames - 2 && worst >= 0 && worst < 6.0, "a hole shorter than the limit is bridged completely");
      else CHECK(gaps == 1 && longest == 30 && maxGap > 0 && worst >= 0 && worst < 6.0, "a hole longer than the limit: exactly 30 filled in, then nothing until the LTC is back");
    }
  }
  {  // the LTC stops (mid-frame): nothing is made up after it
    LtcGen gen(sr, 30, 30.0, false, start);
    long prev = gen.count;
    const long stopAt = long(sr * 2) + 777;
    auto src = [&](long i, long &ended) { if (i >= stopAt) return 0.0f; float x = gen.next(); if (gen.count != prev) { ended = prev; prev = gen.count; } return x; };
    CoastRun R = coast_run(sr, 30, long(sr * 4), src, [](long, float x) { return x; });
    const long lastTrue = R.truth.back().first;
    CHECK(!R.ev.empty() && R.ev.back().count == lastTrue && !R.ev.back().synth, "stop: last event %ld, last complete frame %ld", R.ev.back().count, lastTrue);
  }
  {  // a relocate inside damaged LTC is followed, and nothing of the old position comes after it
    LtcGen a(sr, 30, 30.0, false, tc_to_count(1, 0, 0, 0, 30, false)), b(sr, 30, 30.0, false, tc_to_count(5, 0, 0, 0, 30, false));
    const long jumpAt = long(sr * 2);
    long inFrame = 0, prevC = -1;
    auto src = [&](long i, long &) { LtcGen &g = i < jumpAt ? a : b; float x = g.next(); if (g.count != prevC) { prevC = g.count; inFrame = 0; } inFrame++; return x; };
    auto dmg = [&](long i, float x) { const LtcGen &g = i < jumpAt ? a : b; return (g.count % 3 == 0 && inFrame > 500 && inFrame < 700 && i > long(sr)) ? -x : x; };
    CoastRun R = coast_run(sr, 30, long(sr * 4), src, dmg);
    const long five = tc_to_count(5, 0, 0, 0, 30, false);
    long firstNew = -1, staleAfter = 0, oldAfterJump = 0;
    for (auto &e : R.ev) {
      if (e.count >= five && firstNew < 0) firstNew = long(e.samp);
      if (firstNew >= 0 && e.count < five) staleAfter++;
      if (e.count < five && e.samp > jumpAt) oldAfterJump++;
    }
    std::printf("  relocate: new position after %.0f ms, %ld old frame(s) coasted past the jump\n", (firstNew - jumpAt) / sr * 1e3, oldAfterJump);
    CHECK(firstNew > 0 && (firstNew - jumpAt) / sr < 0.25 && staleAfter == 0 && oldAfterJump <= 6, "relocate while damaged");
  }
}

static void test_daw_core() {
  std::printf("DAW time core\n");
  DawRate a = daw_rate(29.97002997, true), b = daw_rate(23.976, false), c = daw_rate(50.0, false), d = daw_rate(75.0, false), e = daw_rate(30.0, false);
  CHECK(a.fps == 30 && a.df && a.type() == 2 && b.fps == 24 && b.type() == 0 && std::fabs(b.real - 23.976) < 1e-9 && c.fps == 25 && c.real == 25.0 &&
        d.fps == 30 && d.real == 30.0 && e.type() == 3, "project frame rate mapping");
  // boundaries: 25 fps, 48 kHz, 512-sample blocks from project time 100.003 s
  DawClock clk; DawRate r = daw_rate(25.0, false); const double sr = 48000; const uint32_t bs = 512;
  long expect = -1, n = 0; double worst = 0; bool ok = true;
  for (int blk = 0; blk < 400; blk++) {
    const double pos = 100.003 + blk * bs / sr, t0 = 5000.0 + blk * bs / sr;
    clk.block(pos, bs, sr, t0, r, [&](long count, double t, double dur) {
      if (expect >= 0 && count != expect) ok = false;
      expect = count + 1; n++;
      const double trueT = 5000.0 + (count / 25.0 - 100.003);  // wall time at which that frame really starts
      worst = std::max(worst, std::fabs(t - trueT));
      if (std::fabs(dur - 0.04) > 1e-12) ok = false;
    });
  }
  CHECK(ok && n == 106 && worst < 1e-9, "DawClock: %ld frames, worst boundary error %.3g s", n, worst);
  // half speed: frames last twice as long on the wall clock
  DawClock slow; double lastDur = 0; long m = 0;
  for (int blk = 0; blk < 200; blk++) slow.block(10.0 + blk * bs / sr * 0.5, bs, sr, blk * bs / sr, r, [&](long, double, double dur) { lastDur = dur; m++; });
  CHECK(std::fabs(lastDur - 0.08) < 1e-9 && m >= 26 && m <= 28, "DawClock at half speed: frame %.4f s, %ld frames", lastDur, m);
  {  // boundaries exactly on block edges (480-sample blocks at 25 fps: every 4th edge) must come out once, in order
    DawClock edge; long prev = -1, cnt = 0; bool once = true;
    for (int blk = 0; blk < 1000; blk++) edge.block(blk * 480 / sr, 480, sr, blk * 480 / sr, r, [&](long c2, double, double) { if (prev >= 0 && c2 != prev + 1) once = false; prev = c2; cnt++; });
    CHECK(once && cnt == 250, "block-edge boundaries: %ld frames, consecutive %d", cnt, once);
  }

  {  // a start at an EARLIER position whose first block holds no frame boundary (regression: it sent nothing at all)
    DawClock back; long got = 0, firstC = -1;
    for (int blk = 0; blk < 50; blk++) back.block(1200.0 + blk * bs / sr, bs, sr, 0, r, [&](long, double, double) {});
    back.stop();
    for (int blk = 0; blk < 50; blk++) back.block(60.011 + blk * bs / sr, bs, sr, 0, r, [&](long c2, double, double) { if (firstC < 0) firstC = c2; got++; });
    CHECK(got >= 12 && firstC == 1501, "restart at an earlier position: %ld frames from %ld", got, firstC);
    DawClock loop; long seen = 0;  // loop / seek back without a stop in between
    for (int blk = 0; blk < 50; blk++) loop.block(500.0 + blk * bs / sr, bs, sr, 0, r, [&](long, double, double) {});
    for (int blk = 0; blk < 50; blk++) loop.block(100.011 + blk * bs / sr, bs, sr, 0, r, [&](long, double, double) { seen++; });
    CHECK(seen >= 12, "seek back while playing: %ld frames", seen);
  }
  const double B = 512.0 / 48000.0;
  { SourceSelect q; CHECK(q.useDaw(kSourceAuto, true, false, false, B), "auto, silent input, never any LTC: DAW time at once"); }
  { SourceSelect q; int blocks = 0; while (!q.useDaw(kSourceAuto, true, false, true, B)) blocks++;
    CHECK(blocks * B >= 0.14 && blocks * B <= 0.17, "auto, unknown signal: DAW time after %.3f s", blocks * B);
    for (int i = 0; i < 300; i++) q.useDaw(kSourceAuto, true, false, true, B);
    CHECK(q.knowledge() == 2, "after 2 s without LTC the input is known to carry none");
    q.useDaw(kSourceAuto, false, false, true, B);
    CHECK(q.useDaw(kSourceAuto, true, false, true, B), "... so the next start sends DAW time immediately"); }
  { SourceSelect q; CHECK(!q.useDaw(kSourceAuto, true, true, true, B) && q.knowledge() == 1, "LTC locked: LTC wins");
    int blocks = 0; while (!q.useDaw(kSourceAuto, true, false, false, B)) blocks++;
    CHECK(blocks * B >= 0.49 && blocks * B <= 0.52, "auto, LTC dropped out: DAW time after %.3f s", blocks * B);
    SourceSelect l; l.setKnowledge(1); bool any = false; for (int i = 0; i < 2000; i++) any = any || l.useDaw(kSourceLtcOnly, true, false, false, B);
    CHECK(!any, "LTC only: never DAW time");
    CHECK(q.useDaw(kSourceDawOnly, true, true, true, B), "DAW only: DAW time even with LTC present");
    CHECK(!q.useDaw(kSourceDawOnly, false, false, false, B), "stopped: nothing"); }
}

// wait like an audio callback: sleep until just before the deadline, spin only the last 0.3 ms (a real-time thread
// that busy-waits for milliseconds is demoted by the scheduler and becomes LESS punctual)
static void punctual_until(double t) {
  if (t - now_s() > 0.0005) wait_until_s(t - 0.0003);
  while (now_s() < t) {}
}

static void test_router() {
  std::printf("one-sided LTC router\n");
  const uint32_t n = 480;
  std::vector<float> a(n), b(n), oa(n), ob(n);
  for (uint32_t i = 0; i < n; i++) { a[i] = 0.25f * std::sin(i * 0.1f); b[i] = (i / 8) % 2 ? 0.5f : -0.5f; }  // a = music, b = "LTC"
  Router r; r.setSampleRate(48000);
  r.update(false, false); r.render(a.data(), b.data(), oa.data(), ob.data(), n);
  CHECK(r.latch() == -1 && oa == a && ob == b, "no lock: pass-through");
  r.update(false, true); r.render(a.data(), b.data(), oa.data(), ob.data(), n);
  CHECK(r.latch() == 1 && oa == a && ob[0] != a[0] && ob[n - 1] == a[n - 1], "lock on right: left stays, right crossfades to left within 10 ms");
  r.render(a.data(), b.data(), oa.data(), ob.data(), n);
  CHECK(oa == a && ob == a, "latched: left on both outputs");
  // in place
  std::vector<float> ia = a, ib = b; r.render(ia.data(), ib.data(), ia.data(), ib.data(), n);
  CHECK(ia == a && ib == a, "in-place render");
  // no lock for as long as it likes (stopped, paused, a fade, another file): the latch stays
  for (int k = 0; k < 6000; k++) r.update(false, false);
  r.render(a.data(), b.data(), oa.data(), ob.data(), n);
  CHECK(r.latch() == 1 && ob == a, "a minute without lock keeps the latch and the mute");
  r.update(true, false); CHECK(r.latch() == 0, "lock on left moves it");
  r.update(true, true); CHECK(r.latch() == -1, "LTC on both legs: pass-through");
  Router q; q.setSampleRate(48000); q.setLatch(1); q.render(a.data(), b.data(), oa.data(), ob.data(), n);
  CHECK(oa == a && ob == a, "restored latch mutes from the first sample");
}

// ---- UDP listener ------------------------------------------------------------------------------------------------
struct Listener {
  sock_t sock = kNoSocket;
  std::atomic<bool> run{true};
  std::thread th;
  std::mutex mu;
  std::vector<std::pair<double, std::vector<unsigned char>>> pk;
  bool open(int port) {
    net_init();
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (sockaddr *)&a, sizeof(a)) != 0) return false;
#ifdef _WIN32
    DWORD tv = 20;
#else
    timeval tv{0, 20000};
#endif
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    th = std::thread([this] {
      unsigned char b[64];
      while (run) { const int n = (int)recv(sock, (char *)b, sizeof(b), 0); if (n > 0) { std::lock_guard<std::mutex> g(mu); pk.push_back({now_s(), std::vector<unsigned char>(b, b + n)}); } }
    });
    return true;
  }
  void clear() { std::lock_guard<std::mutex> g(mu); pk.clear(); }
  ~Listener() { run = false; if (th.joinable()) th.join(); if (sock != kNoSocket) close_socket(sock); }
};

// Timing tolerance 5 ms: the listener and the feeders here are ordinary threads, whose own scheduling noise is 1-3 ms
// on a busy machine. Typical worst deviation of the sender itself is about 1 ms.
static void check_packets(Listener &L, const char *name, int fps, bool df, double dur, long firstCount, size_t minCount, double maxDevAllowed = 0.005) {
  std::lock_guard<std::mutex> g(L.mu);
  CHECK(L.pk.size() >= minCount, "%s: only %zu packets", name, L.pk.size());
  long prev = -1; double maxDev = 0, sum = 0; int nint = 0; bool content = true;
  for (size_t i = 0; i < L.pk.size(); i++) {
    auto &p = L.pk[i].second;
    if (p.size() != 19 || std::memcmp(p.data(), "Art-Net\0", 8) || p[8] != 0x00 || p[9] != 0x97 || p[11] != 14) { content = false; continue; }
    long c = tc_to_count(p[17], p[16], p[15], p[14], fps, df);
    if (i == 0 && firstCount >= 0 && c != firstCount) { content = false; std::printf("  first packet count %ld, expected %ld\n", c, firstCount); }
    if (prev >= 0 && c != prev + 1) { content = false; std::printf("  packet %zu: count %ld after %ld\n", i, c, prev); }
    prev = c;
    if (i > 0) { double d = L.pk[i].first - L.pk[i - 1].first; sum += d; nint++; maxDev = std::max(maxDev, std::fabs(d - dur)); }
  }
  double mean = nint ? sum / nint : 0;
  std::printf("  %s: %zu packets, mean interval %.3f ms (frame %.3f ms), worst deviation %.2f ms\n", name, L.pk.size(), mean * 1e3, dur * 1e3, maxDev * 1e3);
  CHECK(content, "%s: packet content / order", name);
  CHECK(std::fabs(mean - dur) < 0.0005 && maxDev < maxDevAllowed, "%s: timing", name);
}

static void test_sender(Listener &L) {
  std::printf("Art-Net sender\n");
  Frame fr; fr.fps = 30; fr.df = false; fr.fpsMeasured = 30.0; const double dur = 1.0 / 30.0;
  {  // real-time delivery with +-2 ms observation jitter
    L.clear();
    Sender s; s.setTarget("127.0.0.1"); s.start();
    long c0 = tc_to_count(10, 0, 0, 0, 30, false); unsigned r = 7; double t0 = now_s() + 0.05;
    for (int k = 0; k < 75; k++) {
      double due = t0 + k * dur;
      punctual_until(due);
      Timecode t = count_to_tc(c0 + k, 30, false); fr.h = t.h; fr.m = t.m; fr.s = t.s; fr.f = t.f;
      r = r * 1664525u + 1013904223u;
      s.observe(fr, due - ((r >> 16) % 2000) * 1e-6);  // sample-derived time: exact boundary, seen up to 2 ms early
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    s.stop();
    check_packets(L, "real-time feed", 30, false, dur, c0 + 1, 70);
  }
  for (double offMs : {0.0, 20.0, -50.0}) {  // output latency: packets must leave that much later / earlier than the frame boundary
    L.clear();
    Sender s; s.setTarget("127.0.0.1"); s.setLatencyMs(offMs); s.start();
    const long c0 = tc_to_count(10, 0, 0, 0, 30, false); const double t0 = now_s() + 0.05; const int nfr = 60;
    for (int k = 0; k < nfr; k++) {
      const double due = t0 + k * dur;
      punctual_until(due);
      Timecode t = count_to_tc(c0 + k, 30, false); fr.h = t.h; fr.m = t.m; fr.s = t.s; fr.f = t.f;
      s.observe(fr, due);  // frame c0 + k + 1 starts exactly at 'due'
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    s.stop();
    std::lock_guard<std::mutex> g(L.mu);
    double sum = 0, worst = 0; int nn = 0; long last = -1; bool order = true;
    for (size_t i = 10; i < L.pk.size(); i++) {  // skip the first frames while the sender settles after the start
      auto &q = L.pk[i].second; const long c = tc_to_count(q[17], q[16], q[15], q[14], 30, false);
      if (last >= 0 && c != last + 1) order = false;
      last = c;
      const double err = (L.pk[i].first - (t0 + double(c - c0 - 1) * dur)) * 1e3;  // ms after that frame's true start
      sum += err; nn++; worst = std::max(worst, std::fabs(err - offMs));
    }
    const double mean = nn ? sum / nn : 1e9;
    std::printf("  latency %+6.1f ms: packets leave %+7.2f ms from the frame boundary (worst %.2f ms off), last frame sent %+ld vs decoded\n",
                offMs, mean, worst, last - (c0 + nfr));
    CHECK(order && nn > 40 && std::fabs(mean - offMs) < 1.5 && worst < 5.0, "latency %+.1f ms: mean %+.2f, worst %.2f", offMs, mean, worst);
    CHECK(last - (c0 + nfr) == (offMs < 0 ? long(std::ceil(-offMs * 1e-3 / dur)) : 0), "latency %+.1f ms: overshoot at stop", offMs);
  }
  {  // anticipative host: 6 frames at once every 200 ms, all seen EARLY
    L.clear();
    Sender s; s.setTarget("127.0.0.1"); s.start();
    long c0 = tc_to_count(10, 0, 0, 0, 30, false); double t0 = now_s() + 0.05; int k = 0;
    for (int burst = 0; burst < 12; burst++) {
      double due = t0 + burst * 0.2; punctual_until(due);
      for (int j = 0; j < 6; j++, k++) {
        Timecode t = count_to_tc(c0 + k, 30, false); fr.h = t.h; fr.m = t.m; fr.s = t.s; fr.f = t.f;
        s.observe(fr, due);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    s.stop();
    check_packets(L, "bursty feed", 30, false, dur, c0 + 1, 68);
  }
  {  // relocate: the new position must go out immediately, nothing from the old one afterwards
    L.clear();
    Sender s; s.setTarget("127.0.0.1"); s.start();
    auto feed = [&](long c, int nfr) { double t0 = now_s(); for (int k = 0; k < nfr; k++) { punctual_until(t0 + k * dur); Timecode t = count_to_tc(c + k, 30, false); fr.h = t.h; fr.m = t.m; fr.s = t.s; fr.f = t.f; s.observe(fr, now_s()); } };
    feed(tc_to_count(1, 0, 0, 0, 30, false), 15);
    feed(tc_to_count(5, 30, 0, 0, 30, false), 15);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    s.stop();
    std::lock_guard<std::mutex> g(L.mu);
    int hours1 = 0, hours5 = 0, backwards = 0;
    for (auto &p : L.pk) { if (p.second[17] == 1) { hours1++; if (hours5) backwards++; } else if (p.second[17] == 5) hours5++; }
    CHECK(hours1 >= 13 && hours5 >= 13 && backwards == 0, "relocate: %d at 01h, %d at 05h, %d stale", hours1, hours5, backwards);
    std::printf("  relocate: %d packets at 01:00, %d at 05:30, %d stale\n", hours1, hours5, backwards);
  }
  {  // timecode offset: 00:00:10:00 + 05:20:00:00 leaves as 05:20:10:00; the day wraps; off = untouched; DF adds at the drop-frame count
    Sender s; s.setTarget("127.0.0.1");
    const long ten = tc_to_count(0, 0, 10, 0, 30, false);
    s.setOffset(5, 20, 0, 0, true);
    Timecode t = count_to_tc(s.withOffset(ten, 30, false), 30, false);
    CHECK(t.h == 5 && t.m == 20 && t.s == 10 && t.f == 0, "offset 05:20:00:00 on 00:00:10:00 -> %02d:%02d:%02d:%02d", t.h, t.m, t.s, t.f);
    t = count_to_tc(s.withOffset(tc_to_count(23, 0, 0, 0, 25, false), 25, false), 25, false);
    CHECK(t.h == 4 && t.m == 20 && t.s == 0 && t.f == 0, "offset wraps the day at 25 fps -> %02d:%02d:%02d:%02d", t.h, t.m, t.s, t.f);
    t = count_to_tc(s.withOffset(0, 30, true), 30, true);
    CHECK(t.h == 5 && t.m == 20 && t.s == 0 && t.f == 0, "offset on DF 00:00:00;00 -> %02d:%02d:%02d;%02d", t.h, t.m, t.s, t.f);
    s.setOffset(5, 20, 0, 0, false);
    CHECK(s.withOffset(ten, 30, false) == ten && s.offset().h == 5 && s.offset().m == 20, "offset off: frames pass unchanged, the timecode is kept");
    s.setOffset(0, 0, 0, 29, true);
    CHECK(count_to_tc(s.withOffset(0, 24, false), 24, false).f == 23, "offset frames clamp to the running rate (29 -> 23 at 24 fps)");
  }
  {  // output rate: the value follows the source, frames go out at the output rate
    OutRate o30, odf, o25;
    out_rate(4, o30); out_rate(3, odf); out_rate(2, o25);
    Timecode t = count_to_tc(convert_count(tc_to_count(1, 0, 0, 0, 25, false), 25, false, odf), 30, true);
    CHECK(t.h == 1 && t.m == 0 && t.s == 0 && t.f == 0, "25 to 29.97 DF at 01:00:00:00 -> %02d:%02d:%02d;%02d", t.h, t.m, t.s, t.f);
    t = count_to_tc(convert_count(tc_to_count(2, 10, 30, 12, 25, false), 25, false, o30), 30, false);
    CHECK(t.h == 2 && t.m == 10 && t.s == 30 && t.f == 14, "25 to 30 at 02:10:30:12 -> %02d:%02d:%02d:%02d", t.h, t.m, t.s, t.f);
    t = count_to_tc(convert_count(tc_to_count(2, 10, 30, 29, 30, false), 30, false, o25), 25, false);
    CHECK(t.h == 2 && t.m == 10 && t.s == 30 && t.f == 24, "30 to 25 at 02:10:30:29 -> %02d:%02d:%02d:%02d", t.h, t.m, t.s, t.f);
    for (int sel : {4, 2}) {  // 25 in, 30 out; 30 in, 25 out
      L.clear();
      Sender s; s.setTarget("127.0.0.1"); s.setOutputRate(sel); s.start();
      const int in = sel == 4 ? 25 : 30, out = sel == 4 ? 30 : 25;
      const long c0 = tc_to_count(10, 0, 0, 0, in, false); const double t0 = now_s() + 0.05;
      for (int k = 0; k < in * 2; k++) { punctual_until(t0 + k / double(in)); s.observeAt(c0 + k, in, false, in == 25 ? 1 : 3, 1.0 / in, now_s()); }
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      s.stop();
      char name[32]; std::snprintf(name, sizeof(name), "%d in, %d out", in, out);
      check_packets(L, name, out, false, 1.0 / out, tc_to_count(10, 0, 0, 0, out, false), size_t(out * 2 - 4));
      std::lock_guard<std::mutex> g(L.mu);
      CHECK(!L.pk.empty() && L.pk.back().second[18] == (out == 25 ? 1 : 3), "%s: Art-Net type", name);
    }
  }
  {  // exclusive: the latest start sends alone; a sender with Exclusive off is silenced too; the token returns 2 s after a stop
    L.clear();
    Sender a, b, open;  // a at 01h, b at 02h, open (Exclusive off) at 03h
    open.setExclusive(false);
    for (Sender *s : {&a, &b, &open}) { s->setTarget("127.0.0.1"); s->start(); }
    const double t0 = now_s();
    const long ca = tc_to_count(1, 0, 0, 0, 30, false), cb = tc_to_count(2, 0, 0, 0, 30, false), co = tc_to_count(3, 0, 0, 0, 30, false);
    auto put = [&](Sender &s, long c) { Timecode t = count_to_tc(c, 30, false); fr.h = t.h; fr.m = t.m; fr.s = t.s; fr.f = t.f; s.observe(fr, now_s()); };
    // a and open run throughout; b runs from 0.5 s to 1.0 s
    for (int k = 0; k < 120; k++) {
      punctual_until(t0 + k * dur);
      put(a, ca + k); put(open, co + k);
      if (k >= 15 && k < 30) put(b, cb + k);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    for (Sender *s : {&a, &b, &open}) s->stop();
    std::lock_guard<std::mutex> g(L.mu);
    int n[4] = {0, 0, 0, 0}, mixed = 0; double lastB = 0, firstAAfterB = 0, firstB = 0;
    for (auto &p : L.pk) {
      const int h = p.second[17]; if (h < 1 || h > 3) continue;
      n[h]++;
      if (h == 2) { lastB = p.first; if (!firstB) firstB = p.first; }
      if (h == 1 && lastB && !firstAAfterB) firstAAfterB = p.first;
    }
    for (auto &p : L.pk) if (p.second[17] != 2 && p.second[17] >= 1 && p.second[17] <= 3 && firstB && p.first > firstB + 0.005 && p.first < lastB - 0.005) mixed++;
    std::printf("  exclusive: %d packets from the first, %d from the later start, %d from the one with Exclusive off; first resumes %.2f s after the later one stops\n",
                n[1], n[2], n[3], firstAAfterB - lastB);
    CHECK(n[2] >= 13 && mixed == 0, "exclusive: later start sends alone (%d packets, %d from others meanwhile)", n[2], mixed);
    CHECK(n[3] == 0, "exclusive: the sender with Exclusive off stays silent (%d packets)", n[3]);
    CHECK(firstAAfterB - lastB > 1.9 && firstAAfterB - lastB < 2.3, "exclusive: first sender resumes 2 s after the later one stops (%.2f s)", firstAAfterB - lastB);
  }
}

// ---- the real plugin through its CLAP entry ------------------------------------------------------------------
static const void *host_get_ext(const clap_host_t *, const char *) { return nullptr; }
static void host_noop(const clap_host_t *) {}

static void test_plugin(const char *path, Listener &L) {
  std::printf("plugin binary\n");
  void *h = lib_open(path);
  CHECK(h != nullptr, "cannot load %s", path);
  if (!h) return;
  auto *entry = (const clap_plugin_entry_t *)lib_sym(h, "clap_entry");
  CHECK(entry && entry->init(path), "clap_entry");
  if (!entry) return;
  auto *fac = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  CHECK(fac && fac->get_plugin_count(fac) == 1, "factory");
  const clap_plugin_descriptor_t *desc = fac->get_plugin_descriptor(fac, 0);
  std::printf("  %s (%s) v%s\n", desc->name, desc->id, desc->version);
  clap_host_t host = {CLAP_VERSION_INIT, nullptr, "test", "ct", "", "1", host_get_ext, host_noop, host_noop, host_noop};
  const clap_plugin_t *p = fac->create_plugin(fac, &host, desc->id);
  CHECK(p && p->init(p), "create + init");
  if (!p) return;
  auto *ports = (const clap_plugin_audio_ports_t *)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
  auto *gui = (const clap_plugin_gui_t *)p->get_extension(p, CLAP_EXT_GUI);
  clap_audio_port_info_t info;
  CHECK(ports && ports->count(p, true) == 1 && ports->get(p, 0, true, &info) && info.channel_count == 2, "audio ports");
  CHECK(gui && gui->is_api_supported(p, kGuiApi, false), "gui extension");

  const double sr = 48000; const uint32_t bs = 512;
  CHECK(p->activate(p, sr, bs, bs) && p->start_processing(p), "activate");
  L.clear();
  long start = tc_to_count(6, 29, 59, 0, 30, true);  // The Prince block, drop-frame, crossing a minute
  LtcGen gen(sr, 30, 30000.0 / 1001.0, true, start);
  std::vector<float> l(bs), r(bs), ol(bs), orr(bs);
  float *inp[2] = {l.data(), r.data()}, *outp[2] = {ol.data(), orr.data()};
  clap_audio_buffer_t in{}, out{};
  in.data32 = inp; in.channel_count = 2; out.data32 = outp; out.channel_count = 2;
  clap_process_t pr{};
  pr.frames_count = bs; pr.audio_inputs = &in; pr.audio_outputs = &out; pr.audio_inputs_count = 1; pr.audio_outputs_count = 1;
  // music left, LTC right, for 3 s: pass-through until the LTC is recognised, then left on both outputs
  bool leftOk = true, earlyThru = true, lateBoth = true; double switchedAt = -1;
  const double t0 = now_s(); const int blocks = int(sr * 3 / bs);
  for (int b = 0; b < blocks; b++) {
    for (uint32_t i = 0; i < bs; i++) { l[i] = 0.1f * std::sin(i * 0.05f); r[i] = gen.next(); }
    punctual_until(t0 + b * bs / sr);
    p->process(p, &pr);
    const double tb = b * bs / sr;
    leftOk = leftOk && !std::memcmp(ol.data(), l.data(), bs * sizeof(float));
    const bool rIsLtc = !std::memcmp(orr.data(), r.data(), bs * sizeof(float)), rIsMusic = !std::memcmp(orr.data(), l.data(), bs * sizeof(float));
    if (tb < 0.05) earlyThru = earlyThru && rIsLtc;
    if (switchedAt < 0 && rIsMusic) switchedAt = tb;
    if (tb > 0.5) lateBoth = lateBoth && rIsMusic;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  std::printf("  one-sided LTC: right output carries the left leg from %.0f ms\n", switchedAt * 1e3);
  CHECK(leftOk && earlyThru && lateBoth && switchedAt > 0 && switchedAt < 0.3, "one-sided routing (left %d, early %d, late %d, at %.3f s)", leftOk, earlyThru, lateBoth, switchedAt);
  check_packets(L, "plugin, 29.97 DF on the RIGHT channel", 30, true, 1001.0 / 30000.0, -1, 80);
  {
    std::lock_guard<std::mutex> g(L.mu);
    if (!L.pk.empty()) {
      auto &f = L.pk.front().second; auto &z = L.pk.back().second;
      std::printf("  first %02d:%02d:%02d;%02d  last %02d:%02d:%02d;%02d  type %d\n", f[17], f[16], f[15], f[14], z[17], z[16], z[15], z[14], z[18]);
      CHECK(z[18] == 2, "Art-Net type %d for drop-frame (expected 2)", z[18]);
      Timecode e = count_to_tc(gen.count, 30, true);  // generator is inside frame gen.count now = last finished + 1
      long lastSent = tc_to_count(z[17], z[16], z[15], z[14], 30, true), want = tc_to_count(e.h, e.m, e.s, e.f, 30, true);
      CHECK(std::labs(lastSent - want) <= 1, "last packet %ld vs generator position %ld", lastSent, want);
    }
  }
  {  // LTC stops for a second and comes back: not one sample of it may reach the output
    bool clean = true;
    LtcGen gen2(sr, 30, 30000.0 / 1001.0, true, tc_to_count(6, 50, 0, 0, 30, true));
    for (int b = 0; b < int(sr * 2 / bs); b++) {
      const bool silent = b < int(sr * 1 / bs);
      for (uint32_t i = 0; i < bs; i++) { l[i] = 0.1f * std::sin(i * 0.05f); r[i] = silent ? 0.0f : gen2.next(); }
      p->process(p, &pr);
      clean = clean && !std::memcmp(orr.data(), l.data(), bs * sizeof(float)) && !std::memcmp(ol.data(), l.data(), bs * sizeof(float));
    }
    CHECK(clean, "LTC restart after silence leaked into the output");
  }
  {  // state: the latch survives save / load
    auto *state = (const clap_plugin_state_t *)p->get_extension(p, CLAP_EXT_STATE);
    static std::vector<unsigned char> blob; static size_t rpos; blob.clear(); rpos = 0;
    clap_ostream_t os{nullptr, [](const clap_ostream_t *, const void *d, uint64_t n) -> int64_t { blob.insert(blob.end(), (const unsigned char *)d, (const unsigned char *)d + n); return (int64_t)n; }};
    clap_istream_t is{nullptr, [](const clap_istream_t *, void *d, uint64_t n) -> int64_t { size_t k = std::min<size_t>(n, blob.size() - rpos); std::memcpy(d, blob.data() + rpos, k); rpos += k; return (int64_t)k; }};
    CHECK(state && state->save(p, &os) && blob.size() == 17 && blob[4] == 7 && blob[5] == 2 && blob[6] == 0 && blob[7] == 1 && blob[8] == 30 && blob[9] == 1 && blob[10] == 1 && blob[11] == 0 && blob[16] == 0, "state save (v7: latch right, auto, LTC seen, coast 30, mute on, exclusive on, offset off, rate auto)");
    const clap_plugin_t *p2 = fac->create_plugin(fac, &host, desc->id);
    p2->init(p2); 
    auto *state2 = (const clap_plugin_state_t *)p2->get_extension(p2, CLAP_EXT_STATE);
    CHECK(state2->load(p2, &is), "state load");
    p2->activate(p2, sr, bs, bs); p2->start_processing(p2);
    LtcGen gen3(sr, 30, 30.0, false, 1000);
    for (uint32_t i = 0; i < bs; i++) { l[i] = 0.1f * std::sin(i * 0.05f); r[i] = gen3.next(); }
    p2->process(p2, &pr);
    CHECK(!std::memcmp(orr.data(), l.data(), bs * sizeof(float)), "reloaded project: LTC muted from the very first block");
    p2->stop_processing(p2); p2->deactivate(p2); p2->destroy(p2);
  }
  {  // the Offset parameter through the CLAP interface
    auto *params = (const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS);
    clap_param_info_t pi; double v = 99; char txt[64] = {0};
    CHECK(params && params->count(p) == 7 && params->get_info(p, 0, &pi) && pi.min_value == -500 && pi.max_value == 500, "params: info");
    CHECK(params->get_value(p, pi.id, &v) && v == 0.0, "params: default 0 (got %.2f)", v);
    static clap_event_param_value_t ev; std::memset(&ev, 0, sizeof(ev));
    ev.header.size = sizeof(ev); ev.header.type = CLAP_EVENT_PARAM_VALUE; ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID; ev.param_id = pi.id; ev.value = -37.5;
    clap_input_events_t ie{nullptr, [](const clap_input_events_t *) -> uint32_t { return 1; }, [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * { return &ev.header; }};
    clap_output_events_t oe{nullptr, [](const clap_output_events_t *, const clap_event_header_t *) -> bool { return true; }};
    params->flush(p, &ie, &oe);
    CHECK(params->get_value(p, pi.id, &v) && v == -37.5 && params->value_to_text(p, pi.id, v, txt, sizeof(txt)) && !std::strcmp(txt, "-37.5 ms"), "params: set -37.5 (got %.2f '%s')", v, txt);
    ev.value = 9999; params->flush(p, &ie, &oe); params->get_value(p, pi.id, &v);
    CHECK(v == 500.0, "params: clamped to +500 (got %.1f)", v);
    ev.value = 0; params->flush(p, &ie, &oe);
  }
  {  // ---- DAW time through the real plugin ----------------------------------------------------------------------
    auto *params = (const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS);
    static clap_event_param_value_t mev;
    auto setMode = [&](const clap_plugin_t *pl, int m) {
      std::memset(&mev, 0, sizeof(mev)); mev.header.size = sizeof(mev); mev.header.type = CLAP_EVENT_PARAM_VALUE; mev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      mev.param_id = 2; mev.value = m;
      clap_input_events_t ie{nullptr, [](const clap_input_events_t *) -> uint32_t { return 1; }, [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * { return &mev.header; }};
      clap_output_events_t oe{nullptr, [](const clap_output_events_t *, const clap_event_header_t *) -> bool { return true; }};
      ((const clap_plugin_params_t *)pl->get_extension(pl, CLAP_EXT_PARAMS))->flush(pl, &ie, &oe);
    };
    clap_param_info_t pi2; char txt[64] = {0};
    CHECK(params->count(p) == 7 && params->get_info(p, 1, &pi2) && pi2.id == 2 && (pi2.flags & CLAP_PARAM_IS_STEPPED) && params->value_to_text(p, 2, 1, txt, sizeof(txt)) && !std::strcmp(txt, "LTC only"), "Source parameter");

    // run 'seconds' of real-time blocks on plugin pl. ltcFrom/ltcTo: when the right leg carries LTC. pos0: playhead start
    auto run = [&](const clap_plugin_t *pl, double seconds, double pos0, double ltcFrom, double ltcTo, LtcGen *g) {
      clap_event_transport_t tr{}; tr.header.size = sizeof(tr); tr.header.type = CLAP_EVENT_TRANSPORT; tr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      tr.flags = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
      pr.transport = &tr;
      const double start = now_s();
      for (int b = 0; b < int(sr * seconds / bs); b++) {
        const double tb = b * bs / sr;
        for (uint32_t i = 0; i < bs; i++) { l[i] = 0.1f * std::sin(i * 0.05f); r[i] = (g && tb >= ltcFrom && tb < ltcTo) ? g->next() : 0.0f; }
        tr.song_pos_seconds = (clap_sectime)std::llround((pos0 + tb) * double(CLAP_SECTIME_FACTOR));
        punctual_until(start + tb);
        pl->process(pl, &pr);
      }
      pr.transport = nullptr;
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      return start;
    };
    auto fresh = [&]() { const clap_plugin_t *pl = fac->create_plugin(fac, &host, desc->id); pl->init(pl); pl->activate(pl, sr, bs, bs); pl->start_processing(pl); return pl; };
    auto drop = [&](const clap_plugin_t *pl) { pl->stop_processing(pl); pl->deactivate(pl); pl->destroy(pl); };

    {  // no LTC at all, Auto: project time 00:20:00:00 onwards, from the first block, frame accurate
      const clap_plugin_t *pl = fresh(); L.clear();
      const double pos0 = 1200.0, start = run(pl, 2.0, pos0, 9, 9, nullptr);
      std::lock_guard<std::mutex> g(L.mu);
      double sum = 0, worst = 0; int nn = 0; long last = -1; bool order = true;
      for (auto &q : L.pk) {
        const long c = tc_to_count(q.second[17], q.second[16], q.second[15], q.second[14], 30, false);
        if (last >= 0 && c != last + 1) order = false;
        last = c;
        const double err = (q.first - (start + (c / 30.0 - pos0))) * 1e3; sum += err; nn++; worst = std::max(worst, std::fabs(err));
      }
      const long first = L.pk.empty() ? -1 : tc_to_count(L.pk[0].second[17], L.pk[0].second[16], L.pk[0].second[15], L.pk[0].second[14], 30, false);
      std::printf("  DAW time, no LTC: %zu packets from frame %ld (playhead started at %ld), %+.2f ms from the true frame start, worst %.2f ms, type %d\n",
                  L.pk.size(), first, long(pos0 * 30), nn ? sum / nn : 0.0, worst, L.pk.empty() ? -1 : L.pk[0].second[18]);
      // first start of an instance that has signal but has never seen LTC: 150 ms of patience (it might be LTC)
      CHECK(order && L.pk.size() >= 54 && first <= long(pos0 * 30) + 6 && std::fabs(sum / std::max(nn, 1)) < 1.5 && worst < 5.0 && L.pk[0].second[18] == 3, "DAW time packets");
      // ... after 2 s without LTC it knows there is none: the next start sends from the first frame
      L.mu.unlock(); L.clear();
      for (int b2 = 0; b2 < 30; b2++) { for (uint32_t i = 0; i < bs; i++) l[i] = r[i] = 0; pl->process(pl, &pr); }  // transport stopped for a moment
      run(pl, 0.5, 60.0, 9, 9, nullptr);
      L.mu.lock();
      const long first2 = L.pk.empty() ? -1 : tc_to_count(L.pk[0].second[17], L.pk[0].second[16], L.pk[0].second[15], L.pk[0].second[14], 30, false);
      std::printf("  second start (input known to carry no LTC): first frame %ld, playhead started at %d\n", first2, 60 * 30);
      CHECK(first2 >= 1800 && first2 <= 1801, "second start must send from the first frame (got %ld)", first2);
      drop(pl);
    }
    {  // LTC present from the start, Auto, brand-new instance: not one DAW-time packet may slip out before the lock
      const clap_plugin_t *pl = fresh(); L.clear();
      LtcGen g(sr, 30, 30.0, false, tc_to_count(6, 30, 0, 0, 30, false));
      run(pl, 1.5, 1200.0, 0.0, 9.0, &g);
      std::lock_guard<std::mutex> gg(L.mu);
      int dawPk = 0, ltcPk = 0; for (auto &q : L.pk) (q.second[17] == 6 ? ltcPk : dawPk)++;
      std::printf("  Auto with LTC from the start: %d LTC packets, %d DAW-time packets\n", ltcPk, dawPk);
      CHECK(ltcPk > 35 && dawPk == 0, "Auto must not send DAW time while LTC is locking");
      drop(pl);
    }
    {  // LTC for 1 s, then it drops out while the transport keeps playing
      for (int mode : {0, 1}) {
        const clap_plugin_t *pl = fresh(); setMode(pl, mode); L.clear();
        LtcGen g(sr, 30, 30.0, false, tc_to_count(6, 30, 0, 0, 30, false));
        const double start = run(pl, 3.0, 1200.0, 0.0, 1.0, &g);
        std::lock_guard<std::mutex> gg(L.mu);
        int dawPk = 0, ltcPk = 0; double firstDaw = -1;
        for (auto &q : L.pk) { if (q.second[17] == 6) ltcPk++; else { dawPk++; if (firstDaw < 0) firstDaw = q.first - start; } }
        std::printf("  LTC drops out at 1.0 s, mode %s: %d LTC packets, %d DAW-time packets%s\n", mode == 0 ? "Auto" : "LTC only", ltcPk, dawPk,
                    firstDaw > 0 ? (std::string(", first at ") + std::to_string(firstDaw).substr(0, 5) + " s").c_str() : "");
        // 0.3 s until the lock is considered lost + 0.5 s of patience = DAW time about 0.8 s after the last LTC frame
        if (mode == 0) CHECK(ltcPk > 25 && dawPk > 30 && firstDaw > 1.7 && firstDaw < 1.95, "Auto: DAW time about 0.8 s after the dropout (first at %.3f)", firstDaw);
        else CHECK(ltcPk > 25 && dawPk == 0, "LTC only: nothing after the dropout");
        drop(pl);
      }
    }
    {  // DAW only ignores the LTC on the input
      const clap_plugin_t *pl = fresh(); setMode(pl, 2); L.clear();
      LtcGen g(sr, 30, 30.0, false, tc_to_count(6, 30, 0, 0, 30, false));
      run(pl, 1.0, 1200.0, 0.0, 9.0, &g);
      std::lock_guard<std::mutex> gg(L.mu);
      int dawPk = 0, ltcPk = 0; for (auto &q : L.pk) (q.second[17] == 6 ? ltcPk : dawPk)++;
      CHECK(dawPk > 25 && ltcPk == 0, "DAW only: %d DAW, %d LTC packets", dawPk, ltcPk);
      drop(pl);
    }
    {  // stopped transport: silence on the wire
      const clap_plugin_t *pl = fresh(); L.clear();
      for (int b = 0; b < 100; b++) { for (uint32_t i = 0; i < bs; i++) l[i] = r[i] = 0; pl->process(pl, &pr); }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::lock_guard<std::mutex> gg(L.mu);
      CHECK(L.pk.empty(), "stopped: %zu packets", L.pk.size());
      drop(pl);
    }
  }
  {  // Mute LTC off: one-sided LTC passes through untouched; on again: right carries the left leg
    static clap_event_param_value_t qev;
    auto setMute = [&](int on) {
      std::memset(&qev, 0, sizeof(qev)); qev.header.size = sizeof(qev); qev.header.type = CLAP_EVENT_PARAM_VALUE; qev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      qev.param_id = 4; qev.value = on;
      clap_input_events_t ie{nullptr, [](const clap_input_events_t *) -> uint32_t { return 1; }, [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t * { return &qev.header; }};
      clap_output_events_t oe{nullptr, [](const clap_output_events_t *, const clap_event_header_t *) -> bool { return true; }};
      ((const clap_plugin_params_t *)p->get_extension(p, CLAP_EXT_PARAMS))->flush(p, &ie, &oe);
    };
    LtcGen gen5(sr, 30, 30.0, false, tc_to_count(2, 0, 0, 0, 30, false));
    bool thru = true, routed = true; long n = 0;
    for (int phase = 0; phase < 2; phase++) {
      setMute(phase == 0 ? 0 : 1);
      for (int b = 0; b < int(sr * 1.0 / bs); b++) {
        for (uint32_t i = 0; i < bs; i++) { l[i] = 0.25f * float(std::sin(double(n++) * 0.05)); r[i] = gen5.next(); }
        p->process(p, &pr);
        if (b * bs / sr < 0.5) continue;
        const bool lSame = !std::memcmp(ol.data(), l.data(), bs * sizeof(float));
        if (phase == 0) thru = thru && lSame && !std::memcmp(orr.data(), r.data(), bs * sizeof(float));
        else routed = routed && lSame && !std::memcmp(orr.data(), l.data(), bs * sizeof(float));
      }
    }
    CHECK(thru, "Mute LTC off: both legs must pass through");
    CHECK(routed, "Mute LTC on: right output must carry the left leg");
  }
  {  // LTC on BOTH legs: nothing to separate, straight through
    LtcGen gen4(sr, 25, 25.0, false, tc_to_count(3, 0, 0, 0, 25, false)); bool thru = true;
    for (int b = 0; b < int(sr * 1.5 / bs); b++) {
      for (uint32_t i = 0; i < bs; i++) l[i] = r[i] = gen4.next();
      p->process(p, &pr);
      if (b * bs / sr > 1.0) thru = thru && !std::memcmp(ol.data(), l.data(), bs * sizeof(float)) && !std::memcmp(orr.data(), r.data(), bs * sizeof(float));
    }
    CHECK(thru, "LTC on both legs must pass through");
  }
  p->stop_processing(p);
  p->deactivate(p);
  p->destroy(p);
  entry->deinit();
  lib_close(h);
}

int main(int argc, char **argv) {
  set_env("CT_ARTNET_PORT", "16454");
  set_env("CT_ARTNET_IP", "127.0.0.1");  // never touches the real preference file
  make_thread_realtime(1.0 / 30.0);  // the feeders below stand in for an audio callback: they must be punctual too
  Listener L;
  if (!L.open(16454)) { std::printf("cannot bind 127.0.0.1:16454\n"); return 2; }
  test_math();
  test_decoder();
  test_coaster();
  test_router();
  test_daw_core();
  test_sender(L);
  if (argc > 1) test_plugin(argv[1], L);
  std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
