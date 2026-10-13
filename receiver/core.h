// Art-Net timecode receiver: listener, timeline, LTC generator, MTC quarter frames. No GUI, no devices.
#pragma once

#include <functional>
#include <vector>

#include "ltc_core.h"

namespace rx {
using namespace ctltc;

// ---- Art-Net type to rate -----------------------------------------------------------------------------------------
struct SrcRate { int fps; bool df; double dur; };
inline SrcRate type_rate(int type) {
  switch (type) {
    case 0: return {24, false, 1.0 / 24.0};
    case 1: return {25, false, 1.0 / 25.0};
    case 2: return {30, true, 1001.0 / 30000.0};
    default: return {30, false, 1.0 / 30.0};
  }
}

// ---- timeline: where the incoming timecode is at any wall-clock time ------------------------------------------------
// One packet per frame, sent at the start of that frame; the network only ever delays one. The anchor is the lower
// envelope of (arrival - frames * duration), rising 2 ms/s so it follows a sender whose clock runs slow.
struct Snap {
  bool live = false;
  double E = 0, dur = 1.0 / 30.0, lastT = -1e9;
  long c0 = 0;
  int fps = 30, type = 3;
  bool df = false;
};
constexpr double kHold = 0.15;  // seconds the timecode keeps running after the last packet

class Timeline {
 public:
  void packet(long count, int type, double t) {  // net thread
    const SrcRate r = type_rate(type);
    Snap s = w_;
    const long day = frames_per_day(r.fps, r.df);
    // the next frame, or one a few frames on that arrives when it should: packets were lost, the timecode did not move
    const long step = s.live && type == s.type ? (count - last_ + day) % day : 0;
    const bool cont = step >= 1 && step <= 15 && t - s.lastT < 0.5 && (step == 1 || std::fabs((t - s.lastT) / s.dur - double(step)) < 1.0);
    if (!cont) {
      s.c0 = count; s.E = t; k_ = 0;
      s.fps = r.fps; s.df = r.df; s.dur = r.dur; s.type = type;
    } else {
      k_ += step;
      s.E = std::min(s.E + 0.002 * (t - s.lastT), t - double(k_) * s.dur);
    }
    s.live = true; s.lastT = t;
    last_ = count;
    publish(s);
  }
  void clear() { Snap s; publish(s); }  // net thread

  Snap snap() const {
    Snap s;
    for (;;) {
      const uint32_t a = seq_.load(std::memory_order_acquire);
      if (a & 1) continue;
      s.live = live_.load(std::memory_order_relaxed); s.E = E_.load(std::memory_order_relaxed);
      s.dur = dur_.load(std::memory_order_relaxed); s.lastT = lastT_.load(std::memory_order_relaxed);
      s.c0 = c0_.load(std::memory_order_relaxed); s.fps = fps_.load(std::memory_order_relaxed);
      s.type = type_.load(std::memory_order_relaxed); s.df = df_.load(std::memory_order_relaxed);
      if (seq_.load(std::memory_order_acquire) == a) return s;
    }
  }

 private:
  void publish(const Snap &s) {
    w_ = s;
    seq_.fetch_add(1, std::memory_order_release);
    live_.store(s.live, std::memory_order_relaxed); E_.store(s.E, std::memory_order_relaxed);
    dur_.store(s.dur, std::memory_order_relaxed); lastT_.store(s.lastT, std::memory_order_relaxed);
    c0_.store(s.c0, std::memory_order_relaxed); fps_.store(s.fps, std::memory_order_relaxed);
    type_.store(s.type, std::memory_order_relaxed); df_.store(s.df, std::memory_order_relaxed);
    seq_.fetch_add(1, std::memory_order_release);
  }
  Snap w_;
  long last_ = -1, k_ = 0;
  std::atomic<uint32_t> seq_{0};
  std::atomic<bool> live_{false}, df_{false};
  std::atomic<double> E_{0}, dur_{1.0 / 30.0}, lastT_{-1e9};
  std::atomic<long> c0_{0};
  std::atomic<int> fps_{30}, type_{3};
};

// ---- settings shared by both outputs --------------------------------------------------------------------------------
struct Settings {
  std::atomic<int> rate{0};          // 0 follows the source; 1..4 = 24, 25, 29.97 DF, 30
  std::atomic<double> latencyMs{0};  // positive: later
};

// output position at wall-clock time t: y in output frames (fractional), its rate, real frames per second
struct OutPos { double y = 0, realFps = 30; OutRate o{30, false, 30.0, 3}; };
inline bool out_pos(const Snap &s, const Settings &set, double now, double t, OutPos &p) {
  if (!s.live || now - s.lastT > kHold) return false;
  const double cr = clock_rate(s.fps, s.df);
  if (!out_rate(set.rate.load(std::memory_order_relaxed), p.o)) p.o = {s.fps, s.df, cr, s.type};
  const double x = double(s.c0) + (t - set.latencyMs.load(std::memory_order_relaxed) * 1e-3 - s.E) / s.dur;
  p.y = x / cr * p.o.clock;
  p.realFps = p.o.clock / (s.dur * cr);
  return true;
}
inline long wrap_frame(double y, const OutRate &o) {
  const long day = frames_per_day(o.fps, o.df);
  long f = long(std::floor(y)) % day;
  return f < 0 ? f + day : f;
}

// ---- LTC ----------------------------------------------------------------------------------------------------------
inline void ltc_bits(long count, int fps, bool df, unsigned char bits[80]) {
  const Timecode t = count_to_tc(count, fps, df);
  std::memset(bits, 0, 80);
  auto put = [&](int pos, int n, int v) { for (int i = 0; i < n; i++) bits[pos + i] = (unsigned char)((v >> i) & 1); };
  put(0, 4, t.f % 10); put(8, 2, t.f / 10); bits[10] = df ? 1 : 0;
  put(16, 4, t.s % 10); put(24, 3, t.s / 10);
  put(32, 4, t.m % 10); put(40, 3, t.m / 10);
  put(48, 4, t.h % 10); put(56, 2, t.h / 10);
  static const unsigned char sync[16] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1};
  std::memcpy(bits + 64, sync, 16);
  int ones = 0;
  for (int i = 0; i < 80; i++) ones += bits[i];
  bits[fps == 25 ? 59 : 27] = (unsigned char)(ones & 1);  // polarity correction: an even number of ones
}

// Follows the timeline with a slow rate correction, so callback timing jitter never reaches the bit cells.
class LtcGen {
 public:
  LtcGen(const Timeline &tl, const Settings &set) : tl_(tl), set_(set) {}
  constexpr static float kLevel = 0.316f;  // -10 dBFS

  // mono; tPlay = wall-clock time the first sample is heard. false: stopped, buffer is silence
  bool render(float *out, uint32_t n, double srate, double now, double tPlay) {
    OutPos p;
    if (!out_pos(tl_.snap(), set_, now, tPlay, p)) {
      std::memset(out, 0, sizeof(float) * n);
      locked_ = false;
      return false;
    }
    double inc = p.realFps / srate;
    if (!locked_ || std::fabs(p.y - y_) > 0.5 || p.o.type != type_) {
      y_ = p.y; type_ = p.o.type; locked_ = true;
      half_ = (long long)std::floor(y_ * 160.0);
      load(p.o);
    } else {
      inc += std::max(-0.005 * inc, std::min(0.005 * inc, (p.y - y_) / srate));  // close the error over about a second
    }
    for (uint32_t i = 0; i < n; i++) {
      y_ += inc;
      const long long h = (long long)std::floor(y_ * 160.0);
      while (half_ < h) {
        half_++;
        const int hb = int(half_ % 160);
        if (hb == 0) load(p.o);
        if (!(hb & 1) || bits_[hb >> 1]) level_ = -level_;
      }
      out[i] = level_ * kLevel;
    }
    return true;
  }

 private:
  void load(const OutRate &o) { ltc_bits(wrap_frame(double(half_ / 160), o), o.fps, o.df, bits_); }
  const Timeline &tl_;
  const Settings &set_;
  double y_ = 0;
  long long half_ = 0;
  int type_ = -1;
  float level_ = 1.0f;
  bool locked_ = false;
  unsigned char bits_[80] = {0};
};

// ---- MTC ----------------------------------------------------------------------------------------------------------
// quarter frame 'piece' (0..7) of the frame that was running when piece 0 went out; rate code = Art-Net type
inline unsigned char mtc_quarter(int piece, const Timecode &t, int type) {
  int v = 0;
  switch (piece) {
    case 0: v = t.f & 15; break;
    case 1: v = (t.f >> 4) & 1; break;
    case 2: v = t.s & 15; break;
    case 3: v = (t.s >> 4) & 3; break;
    case 4: v = t.m & 15; break;
    case 5: v = (t.m >> 4) & 3; break;
    case 6: v = t.h & 15; break;
    default: v = ((t.h >> 4) & 1) | ((type & 3) << 1); break;
  }
  return (unsigned char)((piece << 4) | v);
}

class Mtc {
 public:
  using Send = std::function<void(const unsigned char *, int)>;
  Mtc(const Timeline &tl, const Settings &set) : tl_(tl), set_(set) {}
  ~Mtc() { stop(); }
  void start(Send send) {
    stop();
    send_ = std::move(send);
    run_ = true;
    th_ = std::thread([this] { loop(); });
  }
  void stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
  }

 private:
  void full(long count, const OutRate &o) {
    const Timecode t = count_to_tc(count, o.fps, o.df);
    const unsigned char m[10] = {0xF0, 0x7F, 0x7F, 0x01, 0x01, (unsigned char)((o.type << 5) | t.h), (unsigned char)t.m, (unsigned char)t.s, (unsigned char)t.f, 0xF7};
    send_(m, 10);
  }
  void loop() {
    make_thread_realtime(1.0 / 120.0);
    bool locked = false;
    long long q = 0, q0 = 0;   // last quarter frame sent; the quarter frame that carried piece 0
    Timecode tc{0, 0, 0, 0};
    int type = -1;
    long lastFrame = -1;
    OutRate lastRate{30, false, 30.0, 3};
    while (run_) {
      const double now = now_s();
      OutPos p;
      if (!out_pos(tl_.snap(), set_, now, now, p)) {
        if (locked && lastFrame >= 0) full(lastFrame, lastRate);  // where it stopped
        locked = false;
        wait_until_s(now + 0.005);
        continue;
      }
      const long long qn = (long long)std::floor(p.y * 4.0);
      if (!locked || qn + 4 < q || qn > q + 8 || p.o.type != type) {  // start, relocate, rate change
        type = p.o.type;
        full(wrap_frame(p.y, p.o), p.o);
        q0 = (qn / 4 + 1) * 4;   // piece 0 leaves on the next frame boundary
        q = q0 - 1;
        locked = true;
      }
      while (q < qn) {
        q++;
        const int piece = int((q - q0) % 8);
        if (piece == 0) tc = count_to_tc(wrap_frame(double(q / 4), p.o), p.o.fps, p.o.df);
        const unsigned char m[2] = {0xF1, mtc_quarter(piece, tc, p.o.type)};
        send_(m, 2);
      }
      lastFrame = wrap_frame(p.y, p.o); lastRate = p.o;
      const double next = now + (double(q + 1) / 4.0 - p.y) / p.realFps;
      wait_until_s(std::min(next, now + 0.005));
    }
  }
  const Timeline &tl_;
  const Settings &set_;
  Send send_;
  std::atomic<bool> run_{false};
  std::thread th_;
};

// ---- Art-Net listener ---------------------------------------------------------------------------------------------
struct Source { std::string ip; double lastSeen = 0; };

class Listener {
 public:
  explicit Listener(Timeline &tl) : tl_(tl) {}
  ~Listener() { stop(); }
  bool start() {
    net_init();
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == kNoSocket) return false;
    int yes = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(sock_, SOL_SOCKET, SO_REUSEPORT, (const char *)&yes, sizeof(yes));
#endif
    int port = 6454;
    if (const char *e = std::getenv("CT_ARTNET_PORT")) port = std::atoi(e);  // tests only
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_, (sockaddr *)&a, sizeof(a)) != 0) { close_socket(sock_); sock_ = kNoSocket; return false; }
#ifdef _WIN32
    DWORD tv = 100;
#else
    timeval tv{0, 100000};
#endif
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    run_ = true;
    th_ = std::thread([this] { loop(); });
    return true;
  }
  void stop() {
    run_ = false;
    if (th_.joinable()) th_.join();
    if (sock_ != kNoSocket) { close_socket(sock_); sock_ = kNoSocket; }
  }

  // senders heard in the last 3 s, and which one is followed
  std::vector<Source> sources() {
    std::lock_guard<std::mutex> g(mu_);
    const double now = now_s();
    std::vector<Source> out;
    for (auto &s : src_) if (now - s.lastSeen < 3.0) out.push_back(s);
    return out;
  }
  std::string following() { std::lock_guard<std::mutex> g(mu_); return cur_; }
  void choose(const std::string &ip) { std::lock_guard<std::mutex> g(mu_); pick_ = ip; cur_.clear(); }  // "" = first one heard
  std::string choice() { std::lock_guard<std::mutex> g(mu_); return pick_; }

 private:
  void loop() {
    unsigned char b[64];
    while (run_) {
      sockaddr_in from;
      socklen_t fl = sizeof(from);
      const int n = (int)recvfrom(sock_, (char *)b, sizeof(b), 0, (sockaddr *)&from, &fl);
      const double t = now_s();
      if (n < 19 || std::memcmp(b, "Art-Net\0", 8) || b[8] != 0x00 || b[9] != 0x97) continue;
      const int type = b[18] & 3;
      const SrcRate r = type_rate(type);
      if (b[17] > 23 || b[16] > 59 || b[15] > 59 || b[14] >= r.fps) continue;
      char ip[32] = {0};
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
      bool use;
      {
        std::lock_guard<std::mutex> g(mu_);
        Source *s = nullptr;
        for (auto &x : src_) if (x.ip == ip) s = &x;
        if (!s) { src_.push_back({ip, t}); s = &src_.back(); }
        s->lastSeen = t;
        if (!pick_.empty()) cur_ = pick_;
        else if (cur_.empty() || (cur_ != ip && t - curSeen_ > 2.0)) cur_ = ip;  // first heard; another once that one is gone
        use = cur_ == ip;
        if (use) curSeen_ = t;
      }
      if (use) tl_.packet(tc_to_count(b[17], b[16], b[15], b[14], r.fps, r.df), type, t);
    }
  }
  Timeline &tl_;
  sock_t sock_ = kNoSocket;
  std::atomic<bool> run_{false};
  std::thread th_;
  std::mutex mu_;
  std::vector<Source> src_;
  std::string pick_, cur_;
  double curSeen_ = 0;
};

}  // namespace rx
