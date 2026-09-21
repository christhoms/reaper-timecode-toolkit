// LTC decoder, coaster, DAW clock, Art-Net sender. Decoder: port of Cockos' JSFX "SMPTE LTC Reader/Meter" plus
// drop-frame flag, automatic frame rate and measured frame period.
#pragma once

#include "platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace ctltc {

// ---- timecode maths ----------------------------------------------------------------------------------
struct Timecode { int h, m, s, f; };

inline long frames_per_day(int fps, bool df) { return df ? 2589408L : 86400L * fps; }

inline long tc_to_count(int h, int m, int s, int f, int fps, bool df) {
  if (df) {
    long total_min = h * 60L + m;
    return (h * 3600L + m * 60L + s) * 30L + f - 2L * (total_min - total_min / 10L);
  }
  return (h * 3600L + m * 60L + s) * fps + f;
}

inline Timecode count_to_tc(long n, int fps, bool df) {
  long day = frames_per_day(fps, df);
  n %= day;
  if (n < 0) n += day;
  if (df) {
    long d = n / 17982L, r = n % 17982L;
    n += 18L * d + (r >= 2 ? 2L * ((r - 2L) / 1798L) : 0L);
    fps = 30;
  }
  Timecode t;
  t.f = int(n % fps);
  t.s = int((n / fps) % 60);
  t.m = int((n / (fps * 60L)) % 60);
  t.h = int((n / (fps * 3600L)) % 24);
  return t;
}

// one decoded LTC frame (the frame that just ENDED at the sample where push() returned true)
struct Frame {
  int h = 0, m = 0, s = 0, f = 0;
  bool df = false;
  int fps = 30;               // nominal: 24 / 25 / 30
  double fpsMeasured = 30.0;  // from the frame period in samples
  bool is2997() const { return fps == 30 && (df || fpsMeasured < 29.985); }
  double duration() const { return fps == 30 ? (is2997() ? 1001.0 / 30000.0 : 1.0 / 30.0) : 1.0 / fps; }
  int artnetType() const { return fps == 24 ? 0 : fps == 25 ? 1 : df ? 2 : 3; }
};

// ---- LTC decoder (one audio channel) -----------------------------------------------------------------
class Decoder {
 public:
  void reset(double srate) {
    srate_ = srate;
    minthresh_ = std::pow(10.0, -80.0 / 20.0);
    threshenv_ = std::exp(-1.0 / (0.1 * srate));
    thresh_ = minthresh_;  // start LOW: the envelope reaches any signal level within ~100 ms, so quiet LTC locks as
                           // fast as hot LTC (starting at 1.0, as the JSFX does, -40 dBFS needed 1.6 s to lock)
    itm1_ = otm1_ = 0.0;
    lastsign_ = 1;
    sillen_ = 0;
    std::memset(buf_, 0, sizeof(buf_));
    bufpos_ = 0;
    gotbit_ = -1;
    syncpos_ = -1;
    rateIdx_ = 0;
    pulsesize_ = srate_ / kRates[rateIdx_] / 160.0;
    nsamp_ = 0;
    lastSyncSamp_ = -1;
    period_ = 0.0;
    nosync_ = 0;
    prevCount_ = -1;
    sinceEdge_ = 0;
  }

  // samples since the last level change; LTC has one per bit cell
  long sinceEdge() const { return sinceEdge_; }

  bool hasSignal() const { return thresh_ > minthresh_ * 10.0; }

  // true at the sample where a checked frame ends
  bool push(float x, Frame &out) { return pushRaw(x, out) == 2; }

  // 0 nothing, 1 well-formed frame, 2 frame that follows the previous one (checked)
  int pushRaw(float x, Frame &out) {
    nsamp_++;
    sinceEdge_++;
    otm1_ = 0.999 * otm1_ + x - itm1_;
    itm1_ = x;
    const double s = otm1_;

    sillen_ += 1;
    if (sillen_ > pulsesize_ * 2.2) {
      syncpos_ = -1;
      sillen_ = 0;
      gotbit_ = -1;
    }
    thresh_ = thresh_ * threshenv_ + std::fabs(s) * (1.0 - threshenv_);
    if (thresh_ < minthresh_) thresh_ = minthresh_;

    // no sync for half a second while there IS signal: try the next frame rate
    if (++nosync_ > long(srate_ * 0.5)) {
      nosync_ = 0;
      if (hasSignal()) {
        rateIdx_ = (rateIdx_ + 1) % 3;
        pulsesize_ = srate_ / kRates[rateIdx_] / 160.0;
        period_ = 0.0;
      }
    }

    int got = 0;
    if ((s < -thresh_ * 0.8 && lastsign_ > 0) || (s > thresh_ * 0.8 && lastsign_ < 0)) {
      lastsign_ = -lastsign_;
      sinceEdge_ = 0;
      gotbit_ += 1;
      if (sillen_ > pulsesize_ * 1.8) {  // done with bit
        gotbit_ = std::min(gotbit_, 1);
        sillen_ = 0;
        buf_[bufpos_] = (unsigned char)gotbit_;
        if (++bufpos_ >= 80) bufpos_ = 0;

        if (syncpos_ >= 0) syncpos_ += 1;
        if (syncpos_ < 0 || syncpos_ >= 80) {
          syncpos_ = -1;
          static const unsigned char kSync[16] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1};
          bool sync = true;
          for (int i = 0; i < 16 && sync; i++) sync = bit(64 + i) == kSync[i];
          if (sync) {
            out.f = bit(0) + bit(1) * 2 + bit(2) * 4 + bit(3) * 8 + 10 * (bit(8) + bit(9) * 2);
            out.s = bit(16) + bit(17) * 2 + bit(18) * 4 + bit(19) * 8 + 10 * (bit(24) + bit(25) * 2 + bit(26) * 4);
            out.m = bit(32) + bit(33) * 2 + bit(34) * 4 + bit(35) * 8 + 10 * (bit(40) + bit(41) * 2 + bit(42) * 4);
            out.h = bit(48) + bit(49) * 2 + bit(50) * 4 + bit(51) * 8 + 10 * (bit(56) + bit(57) * 2);
            out.df = bit(10) != 0;
            bool periodOk = false;
            if (lastSyncSamp_ >= 0) {
              const double pnow = double(nsamp_ - lastSyncSamp_), pexp = srate_ / kRates[rateIdx_];
              periodOk = pnow > pexp * 0.9 && pnow < pexp * 1.1;
              if (periodOk) period_ = period_ <= 0 ? pnow : period_ * 0.9 + pnow * 0.1;
            }
            lastSyncSamp_ = nsamp_;
            // rate from the measured period: 24 fps also decodes with the 25 fps pulse width
            out.fpsMeasured = period_ > 0 ? srate_ / period_ : double(kRates[rateIdx_]);
            out.fps = out.fpsMeasured < 24.5 ? 24 : out.fpsMeasured < 27.0 ? 25 : 30;
            if (period_ > 0) pulsesize_ = period_ / 160.0;
            const bool plausible = out.h < 24 && out.m < 60 && out.s < 60 && out.f < out.fps;
            const long cnt = plausible ? tc_to_count(out.h, out.m, out.s, out.f, out.fps, out.df) : -1;
            const bool chained = plausible && periodOk && prevCount_ >= 0 && prevFps_ == out.fps && prevDf_ == out.df &&
                                 cnt == (prevCount_ + 1) % frames_per_day(out.fps, out.df);
            got = chained ? 2 : plausible ? 1 : 0;
            prevCount_ = cnt; prevFps_ = out.fps; prevDf_ = out.df;
            nosync_ = 0;
            syncpos_ = 0;
          }
        }
        gotbit_ = -1;
      }
    }
    return got;
  }

 private:
  static constexpr int kRates[3] = {30, 25, 24};
  int bit(int i) const { return buf_[(bufpos_ + i) % 80]; }

  double srate_ = 48000, minthresh_ = 1e-4, threshenv_ = 0, thresh_ = 1, itm1_ = 0, otm1_ = 0;
  double pulsesize_ = 10, period_ = 0;
  int lastsign_ = 1, gotbit_ = -1, syncpos_ = -1, bufpos_ = 0, rateIdx_ = 0;
  long sillen_ = 0, nsamp_ = 0, lastSyncSamp_ = -1, nosync_ = 0, prevCount_ = -1, sinceEdge_ = 0;
  int prevFps_ = 0;
  bool prevDf_ = false;
  unsigned char buf_[80];
};

// ---- coasting (flywheel) over frames the decoder misses ---------------------------------------------------------
// Predicts frame boundaries from the measured period. Passes decoded frames that carry the predicted number near
// the predicted time (checked or not), fills missed boundaries for up to `limit` frames while the input toggles,
// follows a checked frame that does not fit (relocate). limit 0: checked frames only.
constexpr int kCoastMax = 150, kCoastDefault = 30;
inline int clamp_coast(double v) { return std::isfinite(v) ? int(std::min(double(kCoastMax), std::max(0.0, std::round(v)))) : kCoastDefault; }

class Coaster {
 public:
  void reset(double srate) { srate_ = srate; locked_ = false; coasted_ = 0; filled_ = 0; n_ = 0; }
  void setLimit(int frames) { limit_ = clamp_coast(frames); }
  int coasted() const { return coasted_; }  // frames filled in since the last decoded one
  long filled() const { return filled_; }   // frames filled in since the last (re)lock that no late decode confirmed

  // once per sample after Decoder::pushRaw. true: `out` ended `back` samples ago; synth = filled in
  bool tick(int kind, const Frame &fr, long sinceEdge, Frame &out, double &back, bool &synth) {
    n_++;
    if (limit_ <= 0) {
      locked_ = false;
      if (kind != 2) return false;
      out = fr; back = 0; synth = false;
      return true;
    }
    if (kind > 0) {
      const long cnt = tc_to_count(fr.h, fr.m, fr.s, fr.f, fr.fps, fr.df);
      if (locked_ && fr.fps == tmpl_.fps && fr.df == tmpl_.df) {
        // match by frame number, then time: distorted LTC reports frame ends up to 3 ms late
        const long day = frames_per_day(fr.fps, fr.df);
        const long k = (cnt - last_ + day) % day;
        const double pred = ref_ + double(k) * period_;
        if (k <= limit_ + 1 && std::fabs(double(n_) - pred) <= period_ / 4.0) {
          if (kind == 2) { tmpl_ = fr; period_ = srate_ / fr.fpsMeasured; }
          ref_ = pred + 0.25 * (double(n_) - pred);  // follow the decoder, without taking over its jitter
          if (k == 0) {                              // this boundary was already filled in: it is confirmed now
            if (coasted_ > 0) { coasted_ = 0; filled_--; }
            return false;
          }
          coasted_ = 0;
          last_ = cnt;
          out = tmpl_; out.h = fr.h; out.m = fr.m; out.s = fr.s; out.f = fr.f;
          back = double(n_) - ref_; synth = false;
          return true;
        }
      }
      if (kind != 2) return false;  // unchecked and not where we expect it: damaged
      locked_ = true;               // start, or a relocate
      tmpl_ = fr; last_ = cnt; ref_ = double(n_); period_ = srate_ / fr.fpsMeasured; coasted_ = 0; filled_ = 0;
      out = fr; back = 0; synth = false;
      return true;
    }
    if (!locked_ || double(n_) < ref_ + period_ + period_ / 40.0) return false;  // wait two bit cells for the decoder
    if (coasted_ >= limit_) { locked_ = false; return false; }
    ref_ += period_;
    last_ = (last_ + 1) % frames_per_day(tmpl_.fps, tmpl_.df);
    coasted_++;
    // no edge for 10 bit cells: stopped or dropped out. Keep counting, send nothing.
    if (double(sinceEdge) > period_ / 8.0) return false;
    filled_++;
    const Timecode c = count_to_tc(last_, tmpl_.fps, tmpl_.df);
    out = tmpl_; out.h = c.h; out.m = c.m; out.s = c.s; out.f = c.f;
    back = double(n_) - ref_; synth = true;
    return true;
  }

 private:
  double srate_ = 48000, ref_ = 0, period_ = 1600;
  long n_ = 0, last_ = 0, filled_ = 0;
  int limit_ = kCoastDefault, coasted_ = 0;
  bool locked_ = false;
  Frame tmpl_;
};

// ---- one-sided LTC: programme on one leg, LTC on the other ---------------------------------------------------
// LTC on one leg only: mute it, other leg to both outputs (10 ms crossfade). Latched while the LTC leg is silent;
// released when both legs carry LTC or the latched leg carries non-LTC signal for 1 s.
class Router {
 public:
  void setSampleRate(double sr) { step_ = float(1.0 / (0.010 * sr)); }
  int latch() const { return latch_; }  // -1 = pass-through, 0 / 1 = channel that carries the LTC
  void setLatch(int ch) {
    latch_ = ch < 0 ? -1 : (ch > 0 ? 1 : 0);
    if (latch_ >= 0) { muteCh_ = latch_; gain_ = 1.0f; }  // restored state: muted from the first sample
    notLtc_ = 0.0;
  }

  // once per block, after decoding it
  void update(bool locked0, bool locked1, bool signal0, bool signal1, double blockSeconds) {
    if (locked0 != locked1) {
      latch_ = locked0 ? 0 : 1;
      notLtc_ = 0.0;
    } else if (locked0 && locked1) {
      latch_ = -1;  // LTC on both legs: nothing to separate
    } else if (latch_ >= 0) {
      if (latch_ == 0 ? signal0 : signal1) {
        notLtc_ += blockSeconds;
        if (notLtc_ > 1.0) latch_ = -1;
      } else {
        notLtc_ = 0.0;
      }
    }
    if (latch_ >= 0 && gain_ <= 0.0f) muteCh_ = latch_;
    if (latch_ >= 0 && latch_ != muteCh_) muteCh_ = latch_;
  }

  // in-place safe: both inputs are read before anything is written
  void render(const float *in0, const float *in1, float *out0, float *out1, uint32_t n) {
    const float target = latch_ >= 0 ? 1.0f : 0.0f;
    for (uint32_t i = 0; i < n; i++) {
      const float a = in0 ? in0[i] : 0.0f, b = in1 ? in1[i] : 0.0f;
      if (gain_ < target) gain_ = std::min(target, gain_ + step_);
      else if (gain_ > target) gain_ = std::max(target, gain_ - step_);
      float oa = a, ob = b;
      if (gain_ >= 1.0f) {  // fully switched: bit-exact copy of the programme leg, no LTC residue at all
        if (muteCh_ == 0) oa = b; else ob = a;
      } else if (gain_ > 0.0f) {
        if (muteCh_ == 0) oa = a + (b - a) * gain_;
        else ob = b + (a - b) * gain_;
      }
      if (out0) out0[i] = oa;
      if (out1) out1[i] = ob;
    }
  }

 private:
  int latch_ = -1, muteCh_ = 0;
  float gain_ = 0.0f, step_ = 1.0f / 480.0f;
  double notLtc_ = 0.0;
};

// ---- DAW time: the host's playhead as a timecode source ------------------------------------------------------------
struct DawRate {
  int fps = 30;        // nominal 24 / 25 / 30 (what Art-Net can carry)
  bool df = false;
  double real = 30.0;  // frames per second of project time
  int type() const { return fps == 24 ? 0 : fps == 25 ? 1 : df ? 2 : 3; }
};

// project rate to Art-Net rate: 23.976 as 24, 29.97 as 30, 48/50/59.94/60 halved, else 30
inline DawRate daw_rate(double projFps, bool drop) {
  DawRate r;
  if (!(projFps > 1.0)) return r;
  if (projFps > 40.0) projFps *= 0.5;
  const int nom = int(std::lround(projFps));
  if (nom != 24 && nom != 25 && nom != 30) return r;
  r.fps = nom;
  r.real = projFps;
  r.df = drop && nom == 30;
  return r;
}

// reports each frame boundary inside a block with its wall-clock time; play rate from playhead movement
class DawClock {
 public:
  void stop() { valid_ = false; }

  template <class Emit>  // emit(long count, double tBoundary, double frameSeconds)
  void block(double pos, uint32_t n, double srate, double t0, const DawRate &r, Emit &&emit) {
    const double blockSec = double(n) / srate;
    double rate = 1.0;
    if (valid_ && lastBlockSec_ > 0) {
      const double measured = (pos - lastPos_) / lastBlockSec_;
      if (measured > 0.1 && measured < 8.0) rate = measured;  // anything else is a seek / loop: assume normal speed
    }
    // a boundary on a block edge is reported once
    const bool continuous = valid_ && std::fabs(pos - lastEnd_) < 0.25 / r.real;
    if (!continuous) lastCount_ = -1;  // start / seek / loop: forget it, or a jump BACK would look like old frames forever
    lastPos_ = pos; lastBlockSec_ = blockSec; valid_ = true;
    const double end = pos + blockSec * rate;
    lastEnd_ = end;
    const long day = frames_per_day(r.fps, r.df);
    for (long c = long(std::ceil(pos * r.real - 1e-7)); double(c) / r.real < end; c++) {
      if (c < 0) continue;  // pre-roll before project time zero
      if (continuous && c <= lastCount_) continue;
      lastCount_ = c;
      const double i = (double(c) / r.real - pos) / rate * srate;
      emit(c % day, t0 + i / srate, 1.0 / r.real / rate);
    }
  }

 private:
  bool valid_ = false;
  double lastPos_ = 0, lastBlockSec_ = 0, lastEnd_ = 0;
  long lastCount_ = -1;
};

// Auto waits before falling back to DAW time, so a project with LTC sends no DAW frames while the decoder locks
enum SourceMode { kSourceAuto = 0, kSourceLtcOnly = 1, kSourceDawOnly = 2 };

class SourceSelect {
 public:
  int knowledge() const { return knowledge_; }  // 0 unknown, 1 this input has carried LTC, 2 played for a while, never any LTC
  void setKnowledge(int k) { knowledge_ = k < 0 || k > 2 ? 0 : k; }

  // once per block -> true when DAW time is the source for this block
  bool useDaw(int mode, bool playing, bool ltcLocked, bool anySignal, double blockSec) {
    if (ltcLocked) {
      knowledge_ = 1;
      absent_ = played_ = 0;
      return mode == kSourceDawOnly && playing;
    }
    if (!playing) { absent_ = 0; return false; }
    if (mode == kSourceLtcOnly) return false;
    if (mode == kSourceDawOnly) return true;
    const double grace = knowledge_ == 1 ? 0.5 : knowledge_ == 2 ? 0.0 : (anySignal ? 0.15 : 0.0);
    const bool daw = absent_ >= grace;
    absent_ += blockSec;
    played_ += blockSec;
    if (knowledge_ == 0 && played_ > 2.0) knowledge_ = 2;
    return daw;
  }

 private:
  int knowledge_ = 0;
  double absent_ = 0, played_ = 0;
};

// ---- destination IP, remembered between sessions ---------------------------------------------------------
inline bool valid_ip(const std::string &ip) {
  net_init();
  in_addr a;
  return !ip.empty() && inet_pton(AF_INET, ip.c_str(), &a) == 1;
}

inline std::string load_ip() {
  if (const char *e = std::getenv("CT_ARTNET_IP")) return e;  // tests only
  std::string ip;
  if (FILE *f = std::fopen((prefs_path() + kSep + "destination.txt").c_str(), "r")) {
    char line[64] = {0};
    if (std::fgets(line, sizeof(line), f)) ip = line;
    std::fclose(f);
  }
  while (!ip.empty() && (ip.back() == '\n' || ip.back() == '\r' || ip.back() == ' ')) ip.pop_back();
  return valid_ip(ip) ? ip : "";
}

inline void save_ip(const std::string &ip) {
  if (std::getenv("CT_ARTNET_IP")) return;
  make_dir(prefs_path());
  if (FILE *f = std::fopen((prefs_path() + kSep + "destination.txt").c_str(), "w")) {
    std::fprintf(f, "%s\n", ip.c_str());
    std::fclose(f);
  }
}

// output offset in milliseconds (+ = later, - = earlier), remembered like the IP: it belongs to the rig, not the project
constexpr double kOffsetMinMs = -500.0, kOffsetMaxMs = 500.0;
inline double clamp_offset(double ms) { return std::isfinite(ms) ? std::min(kOffsetMaxMs, std::max(kOffsetMinMs, ms)) : 0.0; }

inline double load_offset_ms() {
  if (std::getenv("CT_ARTNET_IP")) return 0.0;  // tests
  double ms = 0.0;
  if (FILE *f = std::fopen((prefs_path() + kSep + "offset_ms.txt").c_str(), "r")) {
    if (std::fscanf(f, "%lf", &ms) != 1) ms = 0.0;
    std::fclose(f);
  }
  return clamp_offset(ms);
}

inline void save_offset_ms(double ms) {
  if (std::getenv("CT_ARTNET_IP")) return;
  make_dir(prefs_path());
  if (FILE *f = std::fopen((prefs_path() + kSep + "offset_ms.txt").c_str(), "w")) {
    std::fprintf(f, "%.2f\n", clamp_offset(ms));
    std::fclose(f);
  }
}

// ---- Art-Net sender --------------------------------------------------------------------------------------
// Sends frame k at E + k * duration, E = upper envelope of (observed time - k * duration). Hosts that render ahead
// deliver frames early and in bursts; packets still leave evenly and never ahead of the decoder.
class Sender {
 public:
  Sender() {
    net_init();
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    int yes = 1;
    if (sock_ != kNoSocket) setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
    std::memset(&dest_, 0, sizeof(dest_));
  }
  ~Sender() {
    stop();
    if (sock_ != kNoSocket) close_socket(sock_);
  }

  void start() {
    if (run_.exchange(true)) return;
    thread_ = std::thread([this] { loop(); });
  }
  void stop() {
    if (!run_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
  }

  bool setTarget(const std::string &ip) {  // UI thread
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    int port = 6454;
    if (const char *e = std::getenv("CT_ARTNET_PORT")) port = std::atoi(e);  // tests only
    a.sin_port = htons((uint16_t)port);
    const bool ok = valid_ip(ip) && inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1;
    std::lock_guard<std::mutex> g(destMutex_);
    haveDest_ = ok;
    if (ok) dest_ = a;
    return ok;
  }

  // audio thread, lock-free. t = wall-clock time (now_s) of the boundary where frame (decoded + 1) starts
  void observe(const Frame &fr, double t) {
    observeAt((tc_to_count(fr.h, fr.m, fr.s, fr.f, fr.fps, fr.df) + 1) % frames_per_day(fr.fps, fr.df), fr.fps, fr.df, fr.artnetType(),
              fr.duration(), t);
  }

  // any source: frame 'count' starts at wall-clock time t and lasts 'dur' seconds
  void observeAt(long count, int fps, bool df, int type, double dur, double t) {
    const uint32_t w = wr_.load(std::memory_order_relaxed);
    const uint32_t next = (w + 1) % kRing;
    if (next == rd_.load(std::memory_order_acquire)) return;  // full: drop
    Obs &o = ring_[w];
    o.df = df;
    o.fps = fps;
    o.dur = dur;
    o.type = type;
    o.count = count;
    o.t = t;
    wr_.store(next, std::memory_order_release);
  }

  uint64_t sent() const { return sent_.load(std::memory_order_relaxed); }

  // positive: later. Negative: earlier, running ahead of the decoder by |offset| (overshoots by that at a stop).
  void setOffsetMs(double ms) { offsetMs_.store(clamp_offset(ms), std::memory_order_relaxed); }
  double offsetMs() const { return offsetMs_.load(std::memory_order_relaxed); }

 private:
  struct Obs { long count; double t, dur; int fps, type; bool df; };
  static constexpr uint32_t kRing = 512;

  void send(long count) {
    const Timecode tc = count_to_tc(count, fps_, df_);
    unsigned char p[19] = {'A', 'r', 't', '-', 'N', 'e', 't', 0, 0x00, 0x97, 0, 14, 0, 0,
                           (unsigned char)tc.f, (unsigned char)tc.s, (unsigned char)tc.m, (unsigned char)tc.h, (unsigned char)type_};
    std::lock_guard<std::mutex> g(destMutex_);
    if (!haveDest_ || sock_ == kNoSocket) return;
    if (sendto(sock_, (const char *)p, sizeof(p), 0, (sockaddr *)&dest_, sizeof(dest_)) == (int)sizeof(p))
      sent_.fetch_add(1, std::memory_order_relaxed);
  }

  void loop() {
    make_thread_realtime(1.0 / 30.0);
    bool active = false;
    long lastCount = -1;   // last observed frame count (wrapped)
    long n0 = 0;           // unwrapped index origin
    long nLast = -1;       // newest observed frame, unwrapped
    long kNext = 0;        // next frame to send, unwrapped
    double E = 0, lastLoop = now_s(), lastObsWall = 0;

    while (run_.load(std::memory_order_relaxed)) {
      double now = now_s();
      if (active) E -= 0.002 * (now - lastLoop);  // envelope decay: follows clock drift, forgets a late callback at 2 ms/s
      lastLoop = now;

      uint32_t r = rd_.load(std::memory_order_relaxed);
      const uint32_t w = wr_.load(std::memory_order_acquire);
      while (r != w) {
        const Obs &o = ring_[r];
        const long day = frames_per_day(o.fps, o.df);
        const bool continuous = active && o.fps == fps_ && o.df == df_ && o.count == (lastCount + 1) % day;
        if (!continuous) {  // start, relocate, rate change: re-anchor on this frame and send it right away
          fps_ = o.fps; df_ = o.df; dur_ = o.dur; type_ = o.type;
          n0 = o.count; nLast = o.count; kNext = o.count;
          E = o.t;
          active = true;
        } else {
          nLast += 1;
          dur_ = o.dur; type_ = o.type;
          E = std::max(E, o.t - double(nLast - n0) * dur_);
        }
        lastCount = o.count;
        lastObsWall = now;
        r = (r + 1) % kRing;
      }
      rd_.store(r, std::memory_order_release);

      if (active && now - lastObsWall > 2.0) active = false;

      const double off = offsetMs_.load(std::memory_order_relaxed) * 1e-3;
      const long ahead = off < 0 ? long(std::ceil(-off / dur_)) : 0;  // frames we may run ahead of the decoder
      const long kMax = nLast + ahead;
      if (active && kNext <= kMax) {
        const double T = E + double(kNext - n0) * dur_ + off;
        now = now_s();
        if (now >= T) {
          const double late = (now - T) / dur_;
          if (late > 1.5) kNext = std::min(kMax, kNext + long(late));  // never send a stale burst
          send(kNext);
          kNext++;
          continue;
        }
        wait_until_s(std::min(T, now + 0.002));  // wake at the deadline; look at the ring every 2 ms meanwhile
      } else {
        wait_until_s(now + 0.002);  // also when idle: the first frame after a start must not wait for a slow poll
      }
    }
  }

  sock_t sock_ = kNoSocket;
  sockaddr_in dest_;
  bool haveDest_ = false;
  std::mutex destMutex_;
  std::atomic<bool> run_{false};
  std::thread thread_;
  Obs ring_[kRing];
  std::atomic<uint32_t> wr_{0}, rd_{0};
  std::atomic<uint64_t> sent_{0};
  std::atomic<double> offsetMs_{0.0};
  int fps_ = 30, type_ = 3;
  bool df_ = false;
  double dur_ = 1.0 / 30.0;
};

}  // namespace ctltc
