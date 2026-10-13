// Tests for the receiver core: timeline, LTC out (decoded again with the plugin's decoder), MTC, the UDP listener.
#include <vector>

#include "../receiver/core.h"

using namespace rx;

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) { g_fail++; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } \
  } while (0)

// Virtual time: packets of `type` from frame c0 at their frame starts, each up to `jitterMs` late; LTC rendered in
// 10 ms blocks and decoded. Returns the worst distance (ms) between a decoded frame's end and where that frame ends
// on the source timeline; -1 when the decoder got nothing or the frames do not chain.
struct Run { long frames = 0, bad = 0; double worstMs = 0; int fps = 0; bool df = false; long first = -1, last = -1; };
static Run ltc_run(int type, long c0, double secs, double jitterMs, int rateSel, double latencyMs, double stopAt = 1e9, long jumpTo = -1, double jumpAt = 1e9, int dropEvery = 0) {
  Timeline tl; Settings set; set.rate = rateSel; set.latencyMs = latencyMs;
  LtcGen gen(tl, set);
  Decoder dec; const double sr = 48000; dec.reset(sr);
  const SrcRate r = type_rate(type);
  Run R; unsigned rnd = 5; long k = 0, base = c0; double tBase = 0;
  std::vector<float> buf(480);
  Frame fr;
  bool jumped = false;
  OutRate o; const bool conv = out_rate(rateSel, o);
  const double cr = clock_rate(r.fps, r.df);
  for (long blk = 0; blk < long(secs * 100); blk++) {
    const double t = blk * 0.01;
    if (!jumped && t >= jumpAt) { jumped = true; base = jumpTo; tBase = t; k = 0; }
    while (tBase + k * r.dur <= t && tBase + k * r.dur < stopAt) {  // packets due up to now
      rnd = rnd * 1664525u + 1013904223u;
      if (!dropEvery || k % dropEvery < dropEvery - 3)  // the last 3 of every dropEvery packets are lost
        tl.packet((base + k) % frames_per_day(r.fps, r.df), type, tBase + k * r.dur + ((rnd >> 16) % 1000) * 1e-6 * jitterMs);
      k++;
    }
    gen.render(buf.data(), 480, sr, t, t);
    for (int i = 0; i < 480; i++) {
      const int kind = dec.pushRaw(buf[i], fr);
      if (kind == 0) continue;
      const double tEnd = t + (i + 1) / sr;
      if (t >= jumpAt && t < jumpAt + 0.3) continue;  // relocating
      if (kind != 2) { if (R.frames > 0 && tEnd < stopAt) R.bad++; continue; }
      R.frames++; R.fps = fr.fps; R.df = fr.df;
      const long got = tc_to_count(fr.h, fr.m, fr.s, fr.f, fr.fps, fr.df);
      if (R.first < 0) R.first = got;
      R.last = got;
      // source position where this output frame ends, against when it did end
      const double srcFrames = conv ? double(got + 1) / o.clock * cr : double(got + 1);
      const double want = tBase + (srcFrames - double(base)) * r.dur + latencyMs * 1e-3;
      if (t - tBase > 1.5) R.worstMs = std::max(R.worstMs, std::fabs(tEnd - want) * 1e3);  // once the envelope has settled
    }
  }
  return R;
}

static void test_ltc() {
  std::printf("LTC out\n");
  struct Case { const char *name; int type, fps; bool df; } cases[] = {{"24", 0, 24, false}, {"25", 1, 25, false}, {"29.97 DF", 2, 30, true}, {"30", 3, 30, false}};
  for (auto &c : cases) {
    const long c0 = tc_to_count(1, 0, 58, 0, c.fps, c.df);  // crosses a minute (drop-frame skips ;00 ;01)
    const Run R = ltc_run(c.type, c0, 5.0, 3.0, 0, 0.0);
    std::printf("  %s, packets up to 3 ms late: %ld frames, worst %.2f ms from the source after 1.5 s\n", c.name, R.frames, R.worstMs);
    CHECK(R.frames > c.fps * 4 && R.bad == 0 && R.fps == c.fps && R.df == c.df && R.worstMs < 1.5, "%s: %ld frames, %ld bad, worst %.2f ms", c.name, R.frames, R.bad, R.worstMs);
  }
  {
    const Run R = ltc_run(1, tc_to_count(10, 0, 0, 0, 25, false), 5.0, 1.0, 4, 0.0);
    std::printf("  25 in, 30 out: %ld frames, worst %.2f ms\n", R.frames, R.worstMs);
    const Timecode t = count_to_tc(R.last, 30, false);
    CHECK(R.fps == 30 && !R.df && R.bad == 0 && R.frames > 120 && R.worstMs < 1.5 && t.h == 10 && t.m == 0 && t.s == 4, "25 in, 30 out: fps %d, %ld bad, worst %.2f, last %02d:%02d:%02d:%02d", R.fps, R.bad, R.worstMs, t.h, t.m, t.s, t.f);
  }
  {
    const Run R = ltc_run(3, tc_to_count(2, 0, 0, 0, 30, false), 5.0, 1.0, 3, 0.0);
    CHECK(R.fps == 30 && R.df && R.bad == 0 && R.frames > 120 && R.worstMs < 1.5, "30 in, 29.97 DF out: fps %d df %d, %ld bad, worst %.2f", R.fps, R.df, R.bad, R.worstMs);
  }
  for (double lat : {100.0, -100.0}) {
    const Run R = ltc_run(3, tc_to_count(2, 0, 0, 0, 30, false), 4.0, 1.0, 0, lat);
    std::printf("  latency %+.0f ms: worst %.2f ms from the shifted source\n", lat, R.worstMs);
    CHECK(R.bad == 0 && R.frames > 90 && R.worstMs < 1.5, "latency %+.0f: %ld bad, worst %.2f", lat, R.bad, R.worstMs);
  }
  {  // 3 packets in every 10 lost: the LTC neither stops nor moves
    const Run R = ltc_run(3, tc_to_count(2, 0, 0, 0, 30, false), 5.0, 1.0, 0, 0.0, 1e9, -1, 1e9, 10);
    std::printf("  3 of every 10 packets lost: %ld frames, %ld bad, worst %.2f ms\n", R.frames, R.bad, R.worstMs);
    CHECK(R.bad == 0 && R.frames > 140 && R.worstMs < 1.5, "lost packets: %ld frames, %ld bad, worst %.2f", R.frames, R.bad, R.worstMs);
  }
  {  // packets stop at 2 s: LTC stops within the hold time
    const Run R = ltc_run(3, tc_to_count(2, 0, 0, 0, 30, false), 4.0, 0.0, 0, 0.0, 2.0);
    const long lastWanted = tc_to_count(2, 0, 0, 0, 30, false) + 60;
    CHECK(R.last >= lastWanted - 1 && R.last <= lastWanted + long(kHold * 30) + 1, "stop: last frame %ld, packets ended at %ld", R.last, lastWanted);
  }
  {  // relocate at 2 s
    const long to = tc_to_count(5, 30, 0, 0, 30, false);
    const Run R = ltc_run(3, tc_to_count(2, 0, 0, 0, 30, false), 5.0, 1.0, 0, 0.0, 1e9, to, 2.0);
    CHECK(R.last > to + 60 && R.last < to + 100 && R.worstMs < 1.5, "relocate: last %ld (target %ld), worst %.2f", R.last, to, R.worstMs);
  }
  unsigned char bits[80];
  for (int fps : {24, 25, 30}) {
    ltc_bits(tc_to_count(13, 47, 29, 11, fps, false), fps, false, bits);
    int ones = 0; for (int i = 0; i < 80; i++) ones += bits[i];
    CHECK(ones % 2 == 0, "%d fps: polarity correction", fps);
  }
}

static void test_mtc() {
  std::printf("MTC out\n");
  const Timecode t{21, 43, 57, 23};
  int f = 0, s = 0, m = 0, h = 0, rate = -1;
  for (int p = 0; p < 8; p++) {
    const unsigned char q = mtc_quarter(p, t, 2);
    CHECK((q >> 4) == p, "piece %d", p);
    const int v = q & 15;
    switch (p) { case 0: f |= v; break; case 1: f |= v << 4; break; case 2: s |= v; break; case 3: s |= v << 4; break;
                 case 4: m |= v; break; case 5: m |= v << 4; break; case 6: h |= v; break; default: h |= (v & 1) << 4; rate = v >> 1; }
  }
  CHECK(f == 23 && s == 57 && m == 43 && h == 21 && rate == 2, "quarter frames: %02d:%02d:%02d:%02d rate %d", h, m, s, f, rate);

  // real time: 25 fps for 2 s
  Timeline tl; Settings set;
  struct Msg { double t; std::vector<unsigned char> b; };
  std::vector<Msg> got; std::mutex mu;
  Mtc mtc(tl, set);
  mtc.start([&](const unsigned char *b, int n) { std::lock_guard<std::mutex> g(mu); got.push_back({now_s(), std::vector<unsigned char>(b, b + n)}); });
  const long c0 = tc_to_count(3, 0, 0, 0, 25, false);
  const double t0 = now_s() + 0.02;
  for (int k = 0; k < 50; k++) { wait_until_s(t0 + k * 0.04); tl.packet(c0 + k, 1, now_s()); }
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  mtc.stop();
  std::lock_guard<std::mutex> g(mu);
  int quarters = 0, order = 0, fulls = 0; double worst = 0, sum = 0, prev = 0; int nint = 0, expect = 0; bool first = true;
  int qf[8] = {0}; double tPiece0 = 0; long worstFrames = 0;
  for (auto &x : got) {
    if (x.b.size() == 10 && x.b[0] == 0xF0) { fulls++; continue; }
    if (x.b.size() != 2 || x.b[0] != 0xF1) continue;
    const int piece = x.b[1] >> 4;
    if (first) { expect = piece; first = false; CHECK(piece == 0, "first quarter frame is piece %d", piece); }
    if (piece != expect) order++;
    expect = (piece + 1) % 8;
    if (quarters) { const double d = x.t - prev; sum += d; nint++; worst = std::max(worst, std::fabs(d - 0.01)); }
    prev = x.t; quarters++;
    qf[piece] = x.b[1] & 15;
    if (piece == 0) tPiece0 = x.t;
    if (piece == 7) {  // a full set: the frame that was running when piece 0 went out
      const int ff = qf[0] | (qf[1] << 4), ss = qf[2] | (qf[3] << 4), mm = qf[4] | (qf[5] << 4), hh = qf[6] | ((qf[7] & 1) << 4);
      const long cnt = tc_to_count(hh, mm, ss, ff, 25, false);
      const long want = c0 + long(std::floor((tPiece0 - t0) / 0.04 + 0.5));
      worstFrames = std::max(worstFrames, std::labs(cnt - want));
      CHECK((qf[7] >> 1) == 1, "rate code %d for 25 fps", qf[7] >> 1);
    }
  }
  std::printf("  25 fps: %d quarter frames, mean interval %.3f ms, worst deviation %.2f ms, %d full frame(s)\n", quarters, nint ? sum / nint * 1e3 : 0.0, worst * 1e3, fulls);
  CHECK(quarters > 180 && order == 0 && worstFrames == 0 && fulls >= 2 && worst < 0.005, "MTC: %d quarters, %d out of order, %ld frames off, %d full, worst %.2f ms", quarters, order, worstFrames, fulls, worst * 1e3);
}

static void test_listener() {
  std::printf("listener\n");
#ifdef _WIN32
  _putenv_s("CT_ARTNET_PORT", "16454");
#else
  setenv("CT_ARTNET_PORT", "16454", 1);
#endif
  Timeline tl;
  Listener L(tl);
  CHECK(L.start(), "bind");
  Sender s; s.setTarget("127.0.0.1"); s.setExclusive(false); s.start();
  const long c0 = tc_to_count(7, 0, 0, 0, 30, true);
  const double t0 = now_s() + 0.05;
  for (int k = 0; k < 45; k++) { wait_until_s(t0 + k * 1001.0 / 30000.0); s.observeAt(c0 + k, 30, true, 2, 1001.0 / 30000.0, now_s()); }
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const Snap sn = tl.snap();
  Settings set; OutPos p;
  const double now = now_s();
  const bool live = out_pos(sn, set, now, now, p);
  const long want = c0 + long(std::floor((now - t0) / (1001.0 / 30000.0)));
  std::printf("  sender to listener: type %d, position %+.2f frames from the feed\n", sn.type, p.y - double(want));
  CHECK(live && sn.type == 2 && sn.df && std::fabs(p.y - double(want) - 0.5) < 0.6, "listener position: live %d type %d, %.2f frames off", live, sn.type, p.y - double(want));
  const auto src = L.sources();
  CHECK(src.size() == 1 && src[0].ip == "127.0.0.1" && L.following() == "127.0.0.1", "sources: %zu", src.size());
  L.choose("10.9.9.9");
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  s.stop();
  L.stop();
}

int main() {
  test_ltc();
  test_mtc();
  test_listener();
  if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
  std::printf("\nall checks passed\n");
  return 0;
}
