// R: every visible word in the receiver. Same rules as src/strings.h.
#pragma once

#include <string>

namespace R {

constexpr const char *appName = "Art-Net Timecode Receiver";

// window labels
constexpr const char *source = "Source", *rate = "Rate", *latency = "Latency", *ltc = "LTC", *mtc = "MTC";
constexpr const char *latencyUnits[2] = {"ms", "fr"};
constexpr const char *automatic = "Auto", *off = "Off", *allChannels = "All";
constexpr const char *rateNames[5] = {"Auto", "24", "25", "29.97 DF", "30"};
inline std::string channel(int n) { return "Ch " + std::to_string(n); }
constexpr const char *virtualPort = "Art-Net Timecode Receiver";  // the MIDI source this app creates (macOS)
constexpr const char *loopbackName = "Timecode Loopback";         // the audio device the installer adds (macOS)

// status line, joined with sep
constexpr const char *sep = "  |  ";
inline const char *rateName(int fps, bool df) { return fps == 24 ? "24" : fps == 25 ? "25" : df ? "29.97 DF" : "30"; }

// faults
constexpr const char *portBusy = "port 6454 in use", *ltcFailed = "LTC device failed", *mtcFailed = "MTC port failed";

}  // namespace R
