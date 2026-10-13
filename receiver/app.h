// Receiver state shared by the two windows (app_win.cpp, app_mac.mm): settings, devices, what the window shows.
#pragma once

#include "core.h"
#include "io.h"
#include "strings.h"

namespace rx {

struct App {
  Timeline tl;
  Settings set;
  Listener listener{tl};
  LtcGen gen{tl, set};
  Mtc mtc{tl, set};
  AudioOut audio;
  MidiOut midi;
  bool bound = false, midiOk = true;
  std::string ltcDev, mtcPort;   // "" = Off
  std::atomic<int> ltcCh{0};     // 0 = all channels, n = channel n
  std::vector<float> mono;       // audio thread

  static std::string file() { return prefs_path() + kSep + "receiver.txt"; }

  void load() {
    if (FILE *f = std::fopen(file().c_str(), "r")) {
      char line[512];
      while (std::fgets(line, sizeof(line), f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == "source") listener.choose(v);
        else if (k == "rate") set.rate = std::min(4, std::max(0, std::atoi(v.c_str())));
        else if (k == "latency_ms") set.latencyMs = clamp_latency(std::atof(v.c_str()));
        else if (k == "ltc") ltcDev = v;
        else if (k == "ltc_ch") ltcCh = std::max(0, std::atoi(v.c_str()));
        else if (k == "mtc") mtcPort = v;
      }
      std::fclose(f);
    }
  }
  void save() {
    make_dir(prefs_path());
    if (FILE *f = std::fopen(file().c_str(), "w")) {
      std::fprintf(f, "source=%s\nrate=%d\nlatency_ms=%.2f\nltc=%s\nltc_ch=%d\nmtc=%s\n", listener.choice().c_str(), set.rate.load(),
                   set.latencyMs.load(), ltcDev.c_str(), ltcCh.load(), mtcPort.c_str());
      std::fclose(f);
    }
  }

  void start() {
    load();
    bound = listener.start();
    openLtc();
    openMtc();
  }
  void shutdown() {
    mtc.stop();
    audio.stop();
    midi.close();
    listener.stop();
  }

  void openLtc() {
    audio.stop();
    if (ltcDev.empty()) return;
    audio.start(ltcDev, [this](float *out, uint32_t n, int ch, double srate, double delay) {
      if (mono.size() < n) mono.resize(n);
      const double now = now_s();
      gen.render(mono.data(), n, srate, now, now + delay);
      const int only = ltcCh.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < n; i++)
        for (int c = 0; c < ch; c++) out[i * ch + c] = (only == 0 || only == c + 1) ? mono[i] : 0.0f;
    });
  }
  void openMtc() {
    mtc.stop();
    midi.close();
    midiOk = true;
    if (mtcPort.empty()) return;
    midiOk = midi.open(mtcPort);
    if (midiOk) mtc.start([this](const unsigned char *b, int n) { midi.send(b, n); });
  }

  void setSource(const std::string &ip) { listener.choose(ip); save(); }
  void setRate(int sel) { set.rate = sel; save(); }
  void setLatency(double ms) { set.latencyMs = clamp_latency(ms); save(); }
  void setLtc(const std::string &id) { ltcDev = id; ltcCh = 0; save(); openLtc(); }
  void setLtcChannel(int ch) { ltcCh = ch; save(); }
  void setMtc(const std::string &id) { mtcPort = id; save(); openMtc(); }

  static std::string nameOf(const std::vector<Device> &list, const std::string &id) {
    if (id.empty()) return R::off;
    for (auto &d : list) if (d.id == id) return d.name;
    return id;
  }
  // a sender's address, with the interface it arrives on
  static std::string sourceLabel(const std::string &ip) {
    in_addr a;
    if (inet_pton(AF_INET, ip.c_str(), &a) != 1) return ip;
    const uint32_t x = ntohl(a.s_addr);
    for (const NetIf &n : list_interfaces()) {
      in_addr ia, ib;
      if (inet_pton(AF_INET, n.addr.c_str(), &ia) != 1 || inet_pton(AF_INET, n.bcast.c_str(), &ib) != 1) continue;
      const uint32_t host = ntohl(ia.s_addr) ^ ntohl(ib.s_addr);  // host bits that are 0 in the address
      uint32_t mask = 0xffffffffu;
      while (mask && (host & mask) != 0) mask <<= 1;              // widen until the differing bits are host bits
      if ((x & mask) == (ntohl(ia.s_addr) & mask)) return ip + "  " + n.name;
    }
    return ip;
  }

  // ---- what the window shows -----------------------------------------------------------------------------------
  struct Ui { char tc[16]; std::string line; bool running = false, fault = false; };
  Timecode lastTc{0, 0, 0, 0};
  bool lastDf = false;
  Ui ui() {
    Ui u;
    const double now = now_s();
    OutPos p;
    u.running = out_pos(tl.snap(), set, now, now, p);
    auto add = [&](const std::string &item) { u.line += (u.line.empty() ? "" : R::sep) + item; };
    if (u.running) {
      lastTc = count_to_tc(wrap_frame(p.y, p.o), p.o.fps, p.o.df);
      lastDf = p.o.df;
      add(R::rateName(p.o.fps, p.o.df));
      add(listener.following());
    }
    std::snprintf(u.tc, sizeof(u.tc), "%02d:%02d:%02d%c%02d", lastTc.h, lastTc.m, lastTc.s, lastDf ? ';' : ':', lastTc.f);
    const char *fault = !bound ? R::portBusy : audio.failed() ? R::ltcFailed : !midiOk ? R::mtcFailed : nullptr;
    if (fault) { add(fault); u.fault = true; }
    return u;
  }
};

}  // namespace rx
