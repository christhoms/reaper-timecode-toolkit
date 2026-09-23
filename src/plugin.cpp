// Reaper Timecode Toolkit: CLAP glue. GUI: gui_mac.mm / gui_win.cpp.
#include "plugin.h"

namespace {

const char *kFeatures[] = {CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_ANALYZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDesc = {
    CLAP_VERSION_INIT,
    "uk.co.christhoms.reaper-timecode-toolkit",
    S::pluginName,
    S::vendor,
    "",
    "",
    "",
    RTT_VERSION,
    S::pluginDescription,
    kFeatures,
};

Plugin *P(const clap_plugin_t *p) { return (Plugin *)p->plugin_data; }

}  // namespace

// ---- CLAP plugin ---------------------------------------------------------------------------------------------
namespace {

bool plug_init(const clap_plugin_t *p) {
  Plugin *s = P(p);
  const ctltc::Destination d = ctltc::load_destination();
  s->broadcast = d.broadcast;
  (d.broadcast ? s->ifAddr : s->ip) = d.ip;
  s->applyDestination();
  s->sender.setLatencyMs(ctltc::load_latency_ms());
  s->latencyUnit.store(ctltc::load_latency_unit());
  s->hostParams = (const clap_host_params_t *)s->host->get_extension(s->host, CLAP_EXT_PARAMS);
  // REAPER hands its API to CLAP plug-ins through this extension: project frame rate and project start offset
  struct reaper_plugin_info_t { int caller_version; void *hwnd_main; int (*Register)(const char *, void *); void *(*GetFunc)(const char *); };
  if (auto *rpi = (const reaper_plugin_info_t *)s->host->get_extension(s->host, "cockos.reaper_extension")) {
    if (rpi->GetFunc) {
      s->fnFrameRate = (double (*)(void *, bool *))rpi->GetFunc("TimeMap_curFrameRate");
      s->fnTimeOffset = (double (*)(void *, bool))rpi->GetFunc("GetProjectTimeOffset");
    }
  }
  s->refreshProject();
  s->sender.start();
  return true;
}

void plug_destroy(const clap_plugin_t *p) {
  Plugin *s = P(p);
  gui_free(s);
  s->sender.stop();
  delete s;
}

bool plug_activate(const clap_plugin_t *p, double sr, uint32_t, uint32_t) {
  Plugin *s = P(p);
  s->srate = sr;
  s->dec[0].reset(sr);
  s->dec[1].reset(sr);
  s->coast[0].reset(sr);
  s->coast[1].reset(sr);
  s->activeCh = -1;
  s->router.setSampleRate(sr);
  return true;
}
void plug_deactivate(const clap_plugin_t *) {}
bool plug_start_processing(const clap_plugin_t *) { return true; }
void plug_stop_processing(const clap_plugin_t *) {}
void plug_reset(const clap_plugin_t *p) {
  Plugin *s = P(p);
  s->dec[0].reset(s->srate);
  s->dec[1].reset(s->srate);
  s->coast[0].reset(s->srate);
  s->coast[1].reset(s->srate);
  s->activeCh = -1;
}

// ---- parameters --------------------------------------------------------------------------------------
constexpr clap_id kParamLatency = 1, kParamSource = 2, kParamCoast = 3, kParamMute = 4, kParamExclusive = 5, kParamOffset = 6, kParamRate = 7;

// host -> plugin value events, and (after a GUI edit) plugin -> host
void param_events(Plugin *s, const clap_input_events_t *in, const clap_output_events_t *out) {
  const uint32_t n = in ? in->size(in) : 0;
  for (uint32_t i = 0; i < n; i++) {
    const clap_event_header_t *h = in->get(in, i);
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
    const auto *ev = (const clap_event_param_value_t *)h;
    if (ev->param_id == kParamSource) { s->mode.store(std::min(2, std::max(0, int(std::lround(ev->value)))), std::memory_order_relaxed); continue; }
    if (ev->param_id == kParamMute) { s->muteLtc.store(ev->value >= 0.5 ? 1 : 0, std::memory_order_relaxed); continue; }
    if (ev->param_id == kParamExclusive) { s->sender.setExclusive(ev->value >= 0.5); continue; }
    if (ev->param_id == kParamRate) { s->sender.setOutputRate(std::min(4, std::max(0, int(std::lround(ev->value))))); continue; }
    if (ev->param_id == kParamOffset) { const ctltc::Timecode o = s->sender.offset(); s->sender.setOffset(o.h, o.m, o.s, o.f, ev->value >= 0.5); continue; }
    if (ev->param_id == kParamCoast) { s->coastLimit.store(ctltc::clamp_coast(ev->value), std::memory_order_relaxed); continue; }
    if (ev->param_id != kParamLatency) continue;
    s->sender.setLatencyMs(ev->value);
    s->latencyNeedsSave.store(true, std::memory_order_relaxed);
    if (s->host->request_callback) s->host->request_callback(s->host);
  }
  auto tell = [&](std::atomic<bool> &fromGui, clap_id id, double value) {  // a GUI edit -> the host
    if (!out || !fromGui.exchange(false, std::memory_order_relaxed)) return;
    clap_event_param_value_t ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.header.size = sizeof(ev);
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.param_id = id;
    ev.note_id = -1; ev.port_index = -1; ev.channel = -1; ev.key = -1;
    ev.value = value;
    out->try_push(out, &ev.header);
  };
  tell(s->latencyFromGui, kParamLatency, s->sender.latencyMs());
  tell(s->muteFromGui, kParamMute, s->muteLtc.load(std::memory_order_relaxed));
  tell(s->modeFromGui, kParamSource, s->mode.load(std::memory_order_relaxed));
  tell(s->exclusiveFromGui, kParamExclusive, s->sender.exclusive() ? 1 : 0);
  tell(s->rateFromGui, kParamRate, s->sender.outputRate());
  tell(s->offsetFromGui, kParamOffset, s->sender.offsetOn() ? 1 : 0);
}

uint32_t params_count(const clap_plugin_t *) { return 7; }
bool params_get_info(const clap_plugin_t *, uint32_t index, clap_param_info_t *info) {
  if (index == 1) {
    std::memset(info, 0, sizeof(*info));
    info->id = kParamSource;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM | CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof(info->name), "%s", S::paramSource);
    info->min_value = 0; info->max_value = 2; info->default_value = 0;
    return true;
  }
  if (index == 3) {
    std::memset(info, 0, sizeof(*info));
    info->id = kParamMute;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof(info->name), "%s", S::paramMute);
    info->min_value = 0; info->max_value = 1; info->default_value = 1;
    return true;
  }
  if (index == 6) {
    std::memset(info, 0, sizeof(*info));
    info->id = kParamRate;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
    std::snprintf(info->name, sizeof(info->name), "%s", S::paramRate);
    info->min_value = 0; info->max_value = 4; info->default_value = 0;
    return true;
  }
  if (index == 5) {
    std::memset(info, 0, sizeof(*info));
    info->id = kParamOffset;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof(info->name), "%s", S::paramOffset);
    info->min_value = 0; info->max_value = 1; info->default_value = 0;
    return true;
  }
  if (index == 4) {
    std::memset(info, 0, sizeof(*info));
    info->id = kParamExclusive;
    info->flags = CLAP_PARAM_IS_STEPPED;  // not automatable: it arbitrates between instances
    std::snprintf(info->name, sizeof(info->name), "%s", S::paramExclusive);
    info->min_value = 0; info->max_value = 1; info->default_value = 1;
    return true;
  }
  if (index == 2) {
    std::memset(info, 0, sizeof(*info));
    info->id = kParamCoast;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    std::snprintf(info->name, sizeof(info->name), "%s", S::paramCoast);
    info->min_value = 0; info->max_value = ctltc::kCoastMax; info->default_value = ctltc::kCoastDefault;
    return true;
  }
  if (index != 0) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = kParamLatency;
  info->flags = CLAP_PARAM_IS_AUTOMATABLE;
  std::snprintf(info->name, sizeof(info->name), "%s", S::paramLatency);
  info->min_value = ctltc::kLatencyMinMs;
  info->max_value = ctltc::kLatencyMaxMs;
  info->default_value = 0.0;
  return true;
}
const auto &kSourceNames = S::sourceNames;
bool params_get_value(const clap_plugin_t *p, clap_id id, double *v) {
  if (id == kParamSource) { *v = P(p)->mode.load(); return true; }
  if (id == kParamCoast) { *v = P(p)->coastLimit.load(); return true; }
  if (id == kParamMute) { *v = P(p)->muteLtc.load(); return true; }
  if (id == kParamExclusive) { *v = P(p)->sender.exclusive() ? 1 : 0; return true; }
  if (id == kParamOffset) { *v = P(p)->sender.offsetOn() ? 1 : 0; return true; }
  if (id == kParamRate) { *v = P(p)->sender.outputRate(); return true; }
  if (id != kParamLatency) return false;
  *v = P(p)->sender.latencyMs();
  return true;
}
bool params_value_to_text(const clap_plugin_t *, clap_id id, double v, char *buf, uint32_t size) {
  if (id == kParamRate) { std::snprintf(buf, size, "%s", S::rateNames[std::min(4, std::max(0, int(std::lround(v))))]); return true; }
  if (id == kParamSource) { std::snprintf(buf, size, "%s", kSourceNames[std::min(2, std::max(0, int(std::lround(v))))]); return true; }
  if (id == kParamMute || id == kParamExclusive || id == kParamOffset) { std::snprintf(buf, size, "%s", v >= 0.5 ? S::on : S::off); return true; }
  if (id == kParamCoast) {
    const int c = ctltc::clamp_coast(v);
    if (c == 0) std::snprintf(buf, size, "%s", S::coastOff);
    else S::coastText(buf, size, c);
    return true;
  }
  if (id != kParamLatency) return false;
  S::latencyText(buf, size, v);
  return true;
}
bool params_text_to_value(const clap_plugin_t *, clap_id id, const char *txt, double *v) {
  if (id == kParamRate && txt) {
    for (int i = 0; i < 5; i++) if (!std::strcmp(txt, S::rateNames[i])) { *v = i; return true; }
    *v = std::min(4, std::max(0, std::atoi(txt)));
    return true;
  }
  if (id == kParamSource && txt) {
    for (int i = 0; i < 3; i++) if (!std::strcmp(txt, kSourceNames[i])) { *v = i; return true; }
    *v = std::min(2, std::max(0, std::atoi(txt)));
    return true;
  }
  if ((id == kParamMute || id == kParamExclusive || id == kParamOffset) && txt) { *v = !std::strcmp(txt, S::on) || std::atof(txt) >= 0.5 ? 1 : 0; return true; }
  if (id == kParamCoast && txt) { *v = !std::strcmp(txt, S::coastOff) ? 0 : ctltc::clamp_coast(std::atof(txt)); return true; }
  if (id != kParamLatency || !txt) return false;
  *v = ctltc::clamp_latency(std::atof(txt));
  return true;
}
void params_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) { param_events(P(p), in, out); }
const clap_plugin_params_t kParams = {params_count, params_get_info, params_get_value, params_value_to_text, params_text_to_value, params_flush};

clap_process_status plug_process(const clap_plugin_t *p, const clap_process_t *pr) {
  Plugin *s = P(p);
  const uint32_t n = pr->frames_count;
  const clap_audio_buffer_t *in = pr->audio_inputs_count > 0 ? &pr->audio_inputs[0] : nullptr;
  clap_audio_buffer_t *out = pr->audio_outputs_count > 0 ? &pr->audio_outputs[0] : nullptr;

  param_events(s, pr->in_events, pr->out_events);
  s->router.setMute(s->muteLtc.load(std::memory_order_relaxed) != 0);
  const int pending = s->loadLatch.exchange(-2, std::memory_order_relaxed);
  if (pending != -2) s->router.setLatch(pending);
  const int pendingK = s->loadKnowledge.exchange(-1, std::memory_order_relaxed);
  if (pendingK >= 0) s->select.setKnowledge(pendingK);

  const double t0 = ctltc::now_s();
  struct LtcObs { ctltc::Frame fr; double t; bool synth; } ltc[64];
  int nLtc = 0;

  if (in && in->data32) {
    const uint32_t nch = std::min<uint32_t>(2, in->channel_count);
    bool sig = false;
    for (uint32_t ch = 0; ch < nch; ch++) {
      const float *x = in->data32[ch];
      if (!x) continue;
      ctltc::Frame raw, fr;
      s->coast[ch].setLimit(s->coastLimit.load(std::memory_order_relaxed));
      for (uint32_t i = 0; i < n; i++) {
        const int kind = s->dec[ch].pushRaw(x[i], raw);
        double back = 0;
        bool synth = false;
        if (!s->coast[ch].tick(kind, raw, s->dec[ch].sinceEdge(), fr, back, synth)) continue;
        const double t = t0 + (double(i + 1) - back) / s->srate;  // a filled-in frame is stamped with its predicted boundary
        s->lastLockCh[ch] = t;
        // LTC on both channels: stay on one so the sender sees every frame exactly once
        if (s->activeCh < 0 || (s->activeCh != (int)ch && t - s->lastLockCh[s->activeCh] > 0.5)) s->activeCh = (int)ch;
        if (s->activeCh != (int)ch) continue;
        if (nLtc < 64) { ltc[nLtc].fr = fr; ltc[nLtc].t = t; ltc[nLtc].synth = synth; nLtc++; }
      }
      sig = sig || s->dec[ch].hasSignal();
    }
    s->signal.store(sig, std::memory_order_relaxed);

    // one-sided LTC? (decided per block, from lock state at the end of this block)
    if (nch == 2) {
      const double tEndR = t0 + double(n) / s->srate;
      s->router.update(tEndR - s->lastLockCh[0] < 0.3, tEndR - s->lastLockCh[1] < 0.3);
      s->latchDisp.store(s->router.latch(), std::memory_order_relaxed);
    }
  }

  // ---- which source feeds the sender for this block -----------------------------------------------------------
  const clap_event_transport_t *tr = pr->transport;
  const bool playing = tr && (tr->flags & CLAP_TRANSPORT_IS_PLAYING) && (tr->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE);
  const double blockSec = double(n) / s->srate, tEnd = t0 + blockSec;
  const bool ltcLocked = nLtc > 0 || (s->activeCh >= 0 && tEnd - s->lastLockCh[s->activeCh] < 0.3);
  const bool useDaw = s->select.useDaw(s->mode.load(std::memory_order_relaxed), playing, ltcLocked, s->signal.load(std::memory_order_relaxed), blockSec);
  s->knowledgeDisp.store(s->select.knowledge(), std::memory_order_relaxed);
  if (playing && !s->wasPlaying && s->host->request_callback) s->host->request_callback(s->host);  // re-read the project settings
  s->wasPlaying = playing;

  auto show = [&](const ctltc::Timecode &c, int fps, bool df, bool is2997, double dur, double t, int src) {
    s->dispTc.store((uint32_t(c.h) << 24) | (uint32_t(c.m) << 16) | (uint32_t(c.s) << 8) | uint32_t(c.f), std::memory_order_relaxed);
    s->dispRate.store(uint32_t(fps) | (df ? 1u << 8 : 0u) | (is2997 ? 1u << 9 : 0u), std::memory_order_relaxed);
    s->lastLock.store(t, std::memory_order_relaxed);
    s->frameDur.store(dur, std::memory_order_relaxed);
    s->dispSource.store(src, std::memory_order_relaxed);
  };
  // what leaves: the output frame (rate converted, offset applied) for source frame 'count'
  auto showCount = [&](long count, int fps, bool df, bool is2997, double dur, double t, int src) {
    ctltc::OutRate o;
    if (ctltc::out_rate(s->sender.outputRate(), o)) {
      dur *= ctltc::clock_rate(fps, df) / o.clock;
      count = ctltc::convert_count(count, fps, df, o);
      fps = o.fps; df = o.df; is2997 = o.df;
    }
    show(ctltc::count_to_tc(s->sender.withOffset(count, fps, df), fps, df), fps, df, is2997, dur, t, src);
  };

  if (useDaw) {
    const ctltc::DawRate r = ctltc::daw_rate(s->projFps.load(std::memory_order_relaxed), s->projDrop.load(std::memory_order_relaxed));
    const double pos = double(tr->song_pos_seconds) / double(CLAP_SECTIME_FACTOR) + s->projOffset.load(std::memory_order_relaxed);
    s->dawClock.block(pos, n, s->srate, t0, r, [&](long count, double t, double dur) {
      s->sender.observeAt(count, r.fps, r.df, r.type(), dur, t);
      showCount(count, r.fps, r.df, r.fps == 30 && r.real < 29.985, dur, t, 2);
    });
  } else {
    s->dawClock.stop();
    if (s->mode.load(std::memory_order_relaxed) != ctltc::kSourceDawOnly) {
      for (int i = 0; i < nLtc; i++) {
        const ctltc::Frame &fr = ltc[i].fr;
        s->sender.observe(fr, ltc[i].t);
        // show what is being SENT: LTC names the frame that just ended, the current frame is that + 1
        showCount((ctltc::tc_to_count(fr.h, fr.m, fr.s, fr.f, fr.fps, fr.df) + 1) % ctltc::frames_per_day(fr.fps, fr.df), fr.fps, fr.df,
                  fr.is2997(), fr.duration(), ltc[i].t, 1);
        s->dispCoasted.store(ltc[i].synth ? s->coast[s->activeCh].coasted() : 0, std::memory_order_relaxed);
        s->dispFilled.store(s->coast[s->activeCh].filled(), std::memory_order_relaxed);
      }
    }
  }

  // output: pass-through, or (one-sided LTC) the programme leg on both outputs with the LTC muted
  if (out && out->data32) {
    const bool stereo = in && in->data32 && in->channel_count >= 2 && out->channel_count >= 2 && in->data32[0] && in->data32[1] &&
                        out->data32[0] && out->data32[1];
    if (stereo) s->router.render(in->data32[0], in->data32[1], out->data32[0], out->data32[1], n);
    for (uint32_t ch = stereo ? 2 : 0; ch < out->channel_count; ch++) {
      float *y = out->data32[ch];
      if (!y) continue;
      const float *x = (in && in->data32 && ch < in->channel_count) ? in->data32[ch] : nullptr;
      if (x == y) continue;
      if (x) std::memcpy(y, x, sizeof(float) * n);
      else std::memset(y, 0, sizeof(float) * n);
    }
  }
  return CLAP_PROCESS_CONTINUE;
}

// audio ports: one stereo in, one stereo out
uint32_t ports_count(const clap_plugin_t *, bool) { return 1; }
bool ports_get(const clap_plugin_t *, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
  if (index != 0) return false;
  std::memset(info, 0, sizeof(*info));
  info->id = is_input ? 1 : 2;
  std::snprintf(info->name, sizeof(info->name), "%s", is_input ? S::portIn : S::portOut);
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->channel_count = 2;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = is_input ? 2 : 1;
  return true;
}
const clap_plugin_audio_ports_t kPorts = {ports_count, ports_get};

// gui
bool gui_is_api_supported(const clap_plugin_t *, const char *api, bool floating) { return !floating && !std::strcmp(api, gui_api()); }
bool gui_get_preferred_api(const clap_plugin_t *, const char **api, bool *floating) { *api = gui_api(); *floating = false; return true; }
bool gui_create(const clap_plugin_t *p, const char *api, bool floating) { return gui_is_api_supported(p, api, floating) && gui_make(P(p)); }
void gui_destroy(const clap_plugin_t *p) { gui_free(P(p)); }
bool gui_set_scale(const clap_plugin_t *p, double scale) {
#ifdef _WIN32
  if (scale >= 0.5 && scale <= 4.0) { P(p)->guiScale = scale; return true; }
#endif
  return false;
}
bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) {
  *w = uint32_t(kGuiW * P(p)->guiScale + 0.5);
  *h = uint32_t(kGuiH * P(p)->guiScale + 0.5);
  return true;
}
bool gui_can_resize(const clap_plugin_t *) { return false; }
bool gui_get_resize_hints(const clap_plugin_t *, clap_gui_resize_hints_t *) { return false; }
bool gui_adjust_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { return gui_get_size(p, w, h); }
bool gui_set_size(const clap_plugin_t *, uint32_t, uint32_t) { return true; }
bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *win) { return gui_parent(P(p), win); }
bool gui_set_transient(const clap_plugin_t *, const clap_window_t *) { return false; }
void gui_suggest_title(const clap_plugin_t *, const char *) {}
bool gui_show(const clap_plugin_t *p) { gui_visible(P(p), true); return P(p)->view != nullptr; }
bool gui_hide(const clap_plugin_t *p) { gui_visible(P(p), false); return P(p)->view != nullptr; }
const clap_plugin_gui_t kGui = {gui_is_api_supported, gui_get_preferred_api, gui_create,       gui_destroy,    gui_set_scale,
                                gui_get_size,         gui_can_resize,        gui_get_resize_hints, gui_adjust_size, gui_set_size,
                                gui_set_parent,       gui_set_transient,     gui_suggest_title, gui_show,       gui_hide};

// state: only the one-sided latch, so a reloaded project mutes the LTC leg from the first sample
// v2 = v1 + source mode + what the instance knows about LTC on its input (see SourceSelect); v3 = v2 + coast limit; v4 = v3 + mute LTC;
// v5 = v4 + exclusive; v6 = v5 + offset (on, h, m, s, f); v7 = v6 + output rate
constexpr int kStateSize[] = {0, 6, 8, 9, 10, 11, 16, 17};  // bytes per version
bool state_save(const clap_plugin_t *p, const clap_ostream_t *st) {
  const ctltc::Timecode o = P(p)->sender.offset();
  const unsigned char b[17] = {'C', 'T', 'L', 'A', 7, (unsigned char)(P(p)->latchDisp.load() + 1), (unsigned char)P(p)->mode.load(),
                              (unsigned char)P(p)->knowledgeDisp.load(), (unsigned char)P(p)->coastLimit.load(), (unsigned char)P(p)->muteLtc.load(),
                              (unsigned char)(P(p)->sender.exclusive() ? 1 : 0), (unsigned char)(P(p)->sender.offsetOn() ? 1 : 0),
                              (unsigned char)o.h, (unsigned char)o.m, (unsigned char)o.s, (unsigned char)o.f,
                              (unsigned char)P(p)->sender.outputRate()};
  return st->write(st, b, sizeof(b)) == (int64_t)sizeof(b);
}
bool state_load(const clap_plugin_t *p, const clap_istream_t *st) {
  unsigned char b[17] = {0};
  int64_t got = 0;
  while (got < (int64_t)sizeof(b)) {
    const int64_t r = st->read(st, b + got, sizeof(b) - got);
    if (r <= 0) break;
    got += r;
  }
  const int ver = b[4];
  if (got < 6 || std::memcmp(b, "CTLA", 4) || b[5] > 2 || ver < 1 || ver > 7) return false;
  if (ver == 1 ? got < kStateSize[1] : got != kStateSize[ver]) return false;
  const bool v2 = ver == 2, v3 = ver >= 3, v4 = ver >= 4, v5 = ver >= 5, v6 = ver >= 6, v7 = ver >= 7;
  if (ver >= 2 && (b[6] > 2 || b[7] > 2)) return false;
  if ((v3 && b[8] > ctltc::kCoastMax) || (v4 && b[9] > 1) || (v5 && b[10] > 1)) return false;
  if (v6 && (b[11] > 1 || b[12] > 23 || b[13] > 59 || b[14] > 59 || b[15] > 59)) return false;
  if (v7 && b[16] > 4) return false;
  if (v7) P(p)->sender.setOutputRate(b[16]);
  if (v6) P(p)->sender.setOffset(b[12], b[13], b[14], b[15], b[11] != 0);
  if (v5) P(p)->sender.setExclusive(b[10] != 0);
  if (v4) P(p)->muteLtc.store(b[9], std::memory_order_relaxed);
  P(p)->loadLatch.store(int(b[5]) - 1, std::memory_order_relaxed);
  P(p)->latchDisp.store(int(b[5]) - 1, std::memory_order_relaxed);
  if (v3) P(p)->coastLimit.store(b[8], std::memory_order_relaxed);
  if (v2 || v3) {
    P(p)->mode.store(b[6], std::memory_order_relaxed);
    P(p)->loadKnowledge.store(b[7], std::memory_order_relaxed);
    P(p)->knowledgeDisp.store(b[7], std::memory_order_relaxed);
  }
  return true;
}
const clap_plugin_state_t kState = {state_save, state_load};

const void *plug_get_extension(const clap_plugin_t *, const char *id) {
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kPorts;
  if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
  return nullptr;
}
void plug_on_main_thread(const clap_plugin_t *p) {
  Plugin *s = P(p);
  if (s->latencyNeedsSave.exchange(false, std::memory_order_relaxed)) ctltc::save_latency_ms(s->sender.latencyMs());
  s->refreshProject();
}

// factory / entry
uint32_t factory_count(const clap_plugin_factory_t *) { return 1; }
const clap_plugin_descriptor_t *factory_desc(const clap_plugin_factory_t *, uint32_t i) { return i == 0 ? &kDesc : nullptr; }
const clap_plugin_t *factory_create(const clap_plugin_factory_t *, const clap_host_t *host, const char *id) {
  if (!host || !clap_version_is_compatible(host->clap_version) || std::strcmp(id, kDesc.id)) return nullptr;
  Plugin *s = new Plugin();
  s->host = host;
  s->plugin.desc = &kDesc;
  s->plugin.plugin_data = s;
  s->plugin.init = plug_init;
  s->plugin.destroy = plug_destroy;
  s->plugin.activate = plug_activate;
  s->plugin.deactivate = plug_deactivate;
  s->plugin.start_processing = plug_start_processing;
  s->plugin.stop_processing = plug_stop_processing;
  s->plugin.reset = plug_reset;
  s->plugin.process = plug_process;
  s->plugin.get_extension = plug_get_extension;
  s->plugin.on_main_thread = plug_on_main_thread;
  return &s->plugin;
}
const clap_plugin_factory_t kFactory = {factory_count, factory_desc, factory_create};

bool entry_init(const char *) { return true; }
void entry_deinit(void) {}
const void *entry_get_factory(const char *id) { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr; }

}  // namespace

extern "C" {
CLAP_EXPORT extern const clap_plugin_entry_t clap_entry;
const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit, entry_get_factory};
}
