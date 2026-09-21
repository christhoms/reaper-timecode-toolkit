// CLAP plugin state shared by the glue (plugin.cpp) and the platform GUIs (gui_mac.mm, gui_win.cpp).
#pragma once

#include <clap/clap.h>

#include "ltc_core.h"
#include "strings.h"

#define CTLTC_VERSION "1.6.0"

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

  // offset parameter (ms). The value lives in the sender; these flags move changes between the threads.
  const clap_host_params_t *hostParams = nullptr;
  std::atomic<bool> offsetFromGui{false};   // GUI changed it -> tell the host (output event)
  std::atomic<bool> offsetNeedsSave{false}; // host changed it -> write the preference on the main thread

  std::string ip;  // UI thread only
  void *view = nullptr;  // platform GUI object
  double guiScale = 1.0;

  void setOffsetFromGui(double ms) {  // main thread
    sender.setOffsetMs(ms);
    ctltc::save_offset_ms(sender.offsetMs());
    offsetFromGui.store(true, std::memory_order_relaxed);
    if (hostParams && hostParams->request_flush) hostParams->request_flush(host);
  }

  bool setIP(const std::string &s) {
    if (!sender.setTarget(s)) return false;
    ip = s;
    ctltc::save_ip(s);
    return true;
  }
};

// ---- what the window shows (same words on both platforms) ----------------------------------------------------
constexpr int kGuiW = 440, kGuiH = 238, kGuiBar = 104;
enum UiColor { kUiOff, kUiDim, kUiLtc, kUiDaw, kUiCoast, kUiWarn };

struct UiState {
  char tc[16];
  std::string line, send, offsetNote;
  UiColor tcColor = kUiOff, lineColor = kUiDim, sendColor = kUiWarn;
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
  u.tcColor = daw ? kUiDaw : coasting ? kUiCoast : locked ? kUiLtc : kUiOff;

  auto add = [&](const std::string &item) { u.line += (u.line.empty() ? "" : S::sep) + item; };
  if (locked) {
    if (daw) add(S::dawTime);
    add(S::rate(rate & 0xff, (rate >> 9) & 1, df));
    if (coasting) add(S::coast(coasted));
  } else if (s.signal.load() && s.mode.load() != ctltc::kSourceDawOnly) {
    add(S::noLock);
    u.lineColor = kUiWarn;
  }
  const int latch = s.latchDisp.load();
  if (latch >= 0) add(S::ltcLeg(latch));

  if (s.ip.empty()) u.send = S::noDestination;
  else if (locked && s.sender.failed()) u.send = S::sendFailed;

  const double offMs = s.sender.offsetMs();
  if (offMs != 0) u.offsetNote = S::offsetFrames(offMs / (s.frameDur.load() * 1e3));
  return u;
}

// platform GUI
const char *gui_api();
bool gui_make(Plugin *s);
void gui_free(Plugin *s);
bool gui_parent(Plugin *s, const clap_window_t *win);
void gui_visible(Plugin *s, bool on);
