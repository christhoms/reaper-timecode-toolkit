// S: every visible word in the plugin (window, host parameters, descriptor). Edit copy here, nowhere else.
//
// Rules (MVR-FORGE S rules, hq/writing-north-star.md):
//   ASCII only. | between inline items. No em dashes.
//   A tooltip only on a control with no words of its own, and it is the control's name.
//   A finding is one clause, with its numbers.
//   Never state that a problem is absent. Readouts state contents; faults state themselves.
//   Explain nothing. The audience knows every term.
//   One absolute value per line in a stacked list.
//   Terse, factual. No reassurance, no next-step hints, no narration of how a value was derived.
//   The data is not a person: it carries a value, it does not "say" one.
//   When Chris proposes wording, ship that wording. Do not re-inflate.
#pragma once

#include <cstdio>
#include <string>

namespace S {

constexpr const char *pluginName = "CT LTC to Art-Net Timecode";
constexpr const char *pluginDescription = "SMPTE LTC to Art-Net timecode";
constexpr const char *vendor = "Chris Thoms";
constexpr const char *portIn = "LTC in", *portOut = "Out";

// window labels
constexpr const char *artnetTo = "Art-Net to", *offset = "Offset", *source = "Source", *ms = "ms";
constexpr const char *sourceNames[3] = {"Auto", "LTC only", "DAW only"};

// status line, joined with sep
constexpr const char *sep = "  |  ";
constexpr const char *dawTime = "DAW time";
inline const char *rate(int fps, bool is2997, bool df) {
  return fps == 30 ? (is2997 ? (df ? "29.97 DF" : "29.97") : "30") : fps == 25 ? "25" : "24";
}
inline std::string coast(int frames) { return "coast " + std::to_string(frames); }
inline const char *muted(int channel) { return channel == 0 ? "LTC L muted" : "LTC R muted"; }

// faults
constexpr const char *noLock = "no lock", *noDestination = "no destination", *sendFailed = "send failed";

inline std::string offsetFrames(double frames) { char b[32]; std::snprintf(b, sizeof(b), "%+.2f fr", frames); return b; }

// host parameters
constexpr const char *paramOffset = "Offset (ms)", *paramSource = "Source", *paramCoast = "Coast (frames)";
constexpr const char *coastOff = "off";
inline void offsetText(char *buf, unsigned size, double ms) { std::snprintf(buf, size, "%+.1f ms", ms); }
inline void coastText(char *buf, unsigned size, int frames) { std::snprintf(buf, size, "%d", frames); }

}  // namespace S
