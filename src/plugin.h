// CLAP plugin state shared by the glue (plugin.cpp) and the platform GUIs (gui_mac.mm, gui_win.cpp).
#pragma once

#include <clap/clap.h>

#include "ltc_core.h"
#include "strings.h"

#define RTT_VERSION "2.0.1"

struct Plugin {
  clap_plugin_t plugin;
  const clap_host_t *host = nullptr;
  double srate = 48000.0;
  ctltc::Decoder dec[2];
  ctltc::Coaster coast[2];             // fills in frames the decoder misses (parameter "Coast")
  std::atomic<int> coastLimit{ctltc::kCoastDefault};
  std::atomic<int> dispCoasted{0};     // audio thread -> UI: frames filled in since the last decoded one
  std::atomic<long> dispFilled{0};     // ... and in total since the LTC locked
  double lastLockCh[2] = {-10, -10};
  int activeCh = -1;
  ctltc::Sender sender;
  ctltc::Router router;
  std::atomic<int> latchDisp{-1};   // audio thread -> UI / state: -1 pass-through, 0 / 1 = LTC leg
  std::atomic<int> loadLatch{-2};   // state load (main thread) -> audio thread; -2 = nothing pending

  // audio thread -> UI
  std::atomic<uint32_t> dispTc{0};     // h<<24 | m<<16 | s<<8 | f
  std::atomic<uint32_t> dispRate{0};   // fps | df<<8 | is2997<<9
  std::atomic<double> lastLock{-10.0};
  std::atomic<bool> signal{false};

  std::atomic<double> frameDur{1.0 / 30.0};

  // source: LTC or the host's playhead ("DAW time")
  ctltc::DawClock dawClock;
  ctltc::SourceSelect select;
  std::atomic<int> mode{ctltc::kSourceAuto};   // parameter "Source"
  std::atomic<int> dispSource{0};              // 0 nothing, 1 LTC, 2 DAW time (for the view)
  std::atomic<int> knowledgeDisp{0};           // SourceSelect::knowledge(), mirrored for state save
  std::atomic<int> loadKnowledge{-1};          // state load -> audio thread
  std::atomic<bool> modeFromGui{false};
  std::atomic<int> muteLtc{1};                 // parameter "Mute LTC"
  std::atomic<bool> muteFromGui{false};
  std::atomic<bool> exclusiveFromGui{false};   // parameter "Exclusive": the value lives in the sender
  std::atomic<bool> rateFromGui{false};        // parameter "Rate": the value lives in the sender
  std::atomic<bool> offsetFromGui{false};      // parameter "Offset" (on / off); the timecode itself is project state in the sender
  bool wasPlaying = false;
  // REAPER project settings, read on the main thread through REAPER's API (other hosts: 30 fps, no offset)
  double (*fnFrameRate)(void *, bool *) = nullptr;
  double (*fnTimeOffset)(void *, bool) = nullptr;
  std::atomic<double> projFps{30.0}, projOffset{0.0};
  std::atomic<bool> projDrop{false};

  void refreshProject() {  // main thread only
    if (fnFrameRate) { bool drop = false; const double f = fnFrameRate(nullptr, &drop); if (f > 1.0) { projFps.store(f); projDrop.store(drop); } }
    if (fnTimeOffset) projOffset.store(fnTimeOffset(nullptr, false));
  }
  void setModeFromGui(int m) {
    mode.store(m < 0 || m > 2 ? 0 : m, std::memory_order_relaxed);
    modeFromGui.store(true, std::memory_order_relaxed);
    if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
  }

  void setMuteFromGui(bool on) {
    muteLtc.store(on ? 1 : 0, std::memory_order_relaxed);
    muteFromGui.store(true, std::memory_order_relaxed);
    if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
  }

  void setExclusiveFromGui(bool on) {
    sender.setExclusive(on);
    exclusiveFromGui.store(true, std::memory_order_relaxed);
    if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
  }

  void setRateFromGui(int sel) {
    sender.setOutputRate(sel);
    rateFromGui.store(true, std::memory_order_relaxed);
    if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
  }

  void setOffsetFromGui(bool on) {  // the timecode stays; only whether it is applied changes
    const ctltc::Timecode o = sender.offset();
    sender.setOffset(o.h, o.m, o.s, o.f, on);
    offsetFromGui.store(true, std::memory_order_relaxed);
    if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
  }
  // "HH:MM:SS:FF" (also H:M:S:F, ';' or '.' as separators). Anything else: unchanged, returns false.
  bool setOffsetTextFromGui(const std::string &text) {
    int v[4] = {0, 0, 0, 0}, n = 0, cur = -1;
    for (char c : text) {
      if (c >= '0' && c <= '9') { if (cur < 0) { if (n == 4) return false; cur = 0; } cur = cur * 10 + (c - '0'); if (cur > 99) return false; }
      else if (c == ':' || c == ';' || c == '.') { if (cur < 0) return false; v[n++] = cur; cur = -1; }
      else if (c != ' ') return false;
    }
    if (cur >= 0) { if (n == 4) return false; v[n++] = cur; }
    if (n != 4 || v[0] > 23 || v[1] > 59 || v[2] > 59 || v[3] > 59) return false;
    sender.setOffset(v[0], v[1], v[2], v[3], sender.offsetOn());
    return true;
  }
  std::string offsetText() const {
    const ctltc::Timecode o = sender.offset();
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d:%02d:%02d", o.h, o.m, o.s, o.f);
    return b;
  }

  // latency parameter (ms). The value lives in the sender; these flags move changes between the threads.
  const clap_host_params_t *hostParams = nullptr;
  std::atomic<bool> latencyFromGui{false};   // GUI changed it -> tell the host (output event)
  std::atomic<bool> latencyNeedsSave{false}; // host changed it -> write the preference on the main thread

  // destination (UI thread only): unicast to ip, or directed broadcast on the interface whose address is ifAddr
  std::string ip, ifAddr;
  bool broadcast = false;
  std::atomic<bool> noInterface{false};  // broadcast chosen, interface not present
  void *view = nullptr;  // platform GUI object
  double guiScale = 1.0;

  void setLatencyFromGui(double ms) {  // main thread
    sender.setLatencyMs(ms);
    ctltc::save_latency_ms(sender.latencyMs());
    latencyFromGui.store(true, std::memory_order_relaxed);
    if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
  }

  // the window shows and steps the latency in ms or in frames of the running rate; the parameter stays in ms
  std::atomic<int> latencyUnit{0};  // 0 ms, 1 frames (per machine)
  double frameMs() const { return frameDur.load(std::memory_order_relaxed) * 1e3; }
  double latencyShown() const { return latencyUnit.load() ? sender.latencyMs() / frameMs() : sender.latencyMs(); }
  const char *latencyFormat() const { return latencyUnit.load() ? "%.2f" : "%.1f"; }
  void setLatencyShownFromGui(double v) { setLatencyFromGui(ctltc::clamp_latency(latencyUnit.load() ? v * frameMs() : v)); }
  void stepLatencyFromGui(int dir, bool otherUnit) {  // one of the shown unit; with Shift one of the other unit
    const bool frames = (latencyUnit.load() != 0) != otherUnit;
    setLatencyFromGui(ctltc::clamp_latency(sender.latencyMs() + dir * (frames ? frameMs() : 1.0)));
  }
  void setLatencyUnitFromGui(int unit) {
    latencyUnit.store(unit ? 1 : 0);
    ctltc::save_latency_unit(unit ? 1 : 0);
  }

  // point the sender at the current destination; false = nothing to send to
  bool applyDestination() {
    bool ok = false;
    if (broadcast) {
      for (const ctltc::NetIf &n : ctltc::list_interfaces()) if (n.addr == ifAddr) { ok = sender.setTarget(n.bcast); break; }
      if (!ok) sender.setTarget("");
      noInterface.store(!ok && !ifAddr.empty());
    } else {
      ok = sender.setTarget(ip);
      noInterface.store(false);
    }
    return ok;
  }
  void saveDestination() { ctltc::save_destination({broadcast, broadcast ? ifAddr : ip}); }
  bool setIP(const std::string &s) {
    if (!ctltc::valid_ip(s)) return false;
    ip = s;
    saveDestination();
    return applyDestination();
  }
  void setInterface(const std::string &addr) { ifAddr = addr; saveDestination(); applyDestination(); }
  void setBroadcast(bool on) { broadcast = on; saveDestination(); applyDestination(); }
  std::string interfaceLabel() const {  // what the picker shows
    if (ifAddr.empty()) return S::chooseInterface;
    for (const ctltc::NetIf &n : ctltc::list_interfaces()) if (n.addr == ifAddr) return n.name + "  " + n.addr;
    return ifAddr;
  }
};

// ---- what the window shows (same words on both platforms) ----------------------------------------------------
constexpr int kGuiW = 440, kGuiH = 271, kGuiBar = 137;
enum UiColor { kUiOff, kUiDim, kUiLtc, kUiDaw, kUiCoast, kUiWarn };

struct UiState {
  char tc[16];
  std::string line, latencyNote;
  UiColor tcColor = kUiOff, lineColor = kUiDim;
};

inline UiState ui_state(Plugin &s) {
  UiState u;
  const uint32_t tc = s.dispTc.load(), rate = s.dispRate.load();
  const bool locked = ctltc::now_s() - s.lastLock.load() < 0.3;
  const bool daw = locked && s.dispSource.load() == 2;
  const int coasted = s.dispCoasted.load();
  const bool coasting = locked && !daw && coasted >= 2;  // a single filled frame is usually confirmed by a late decode
  const bool df = (rate >> 8) & 1;
  std::snprintf(u.tc, sizeof(u.tc), "%02u:%02u:%02u%c%02u", (tc >> 24) & 0xff, (tc >> 16) & 0xff, (tc >> 8) & 0xff, df ? ';' : ':', tc & 0xff);
  const bool blocked = locked && s.sender.blocked();  // nothing leaves: the timecode stays grey
  u.tcColor = blocked ? kUiOff : daw ? kUiDaw : coasting ? kUiCoast : locked ? kUiLtc : kUiOff;

  auto add = [&](const std::string &item) { u.line += (u.line.empty() ? "" : S::sep) + item; };
  if (locked) {
    if (daw) add(S::dawTime);
    add(S::rate(rate & 0xff, (rate >> 9) & 1, df));
    if (coasting) add(S::coast(coasted));
    if (blocked) { add(S::otherInstance); u.lineColor = kUiWarn; }
  } else if (s.signal.load() && s.mode.load() != ctltc::kSourceDawOnly) {
    add(S::noLock);
    u.lineColor = kUiWarn;
  }
  const int latch = s.latchDisp.load();
  if (latch >= 0) add(S::ltcLeg(latch));

  // destination faults join the status line
  const char *send = (s.broadcast ? s.ifAddr.empty() : s.ip.empty()) ? S::noDestination : s.noInterface.load() ? S::noInterface
                     : (locked && s.sender.failed()) ? S::sendFailed : nullptr;
  if (send) { add(send); u.lineColor = kUiWarn; }

  const double offMs = s.sender.latencyMs();
  if (offMs != 0) u.latencyNote = s.latencyUnit.load() ? S::latencyMs(offMs) : S::latencyFrames(offMs / s.frameMs());
  return u;
}

// platform GUI
const char *gui_api();
bool gui_make(Plugin *s);
void gui_free(Plugin *s);
bool gui_parent(Plugin *s, const clap_window_t *win);
void gui_visible(Plugin *s, bool on);
