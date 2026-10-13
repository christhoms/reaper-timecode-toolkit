// Audio and MIDI outputs. io_win.cpp: WASAPI + WinMM. io_mac.mm: Core Audio + Core MIDI.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace rx {

struct Device { std::string id, name; };

std::vector<Device> audio_outputs();
std::vector<Device> midi_outputs();

class AudioOut {
 public:
  // interleaved float, frames, channels, sample rate, seconds until the first frame is heard. Called on the audio thread.
  using Fill = std::function<void(float *, uint32_t, int, double, double)>;
  AudioOut();
  ~AudioOut();
  bool start(const std::string &id, Fill fill);
  void stop();
  int channels() const;   // 0 until the device is running
  bool failed() const;    // could not open, or the device went away

 private:
  struct Impl;
  Impl *p_;
};

class MidiOut {
 public:
  MidiOut();
  ~MidiOut();
  bool open(const std::string &id);
  void close();
  void send(const unsigned char *bytes, int n);

 private:
  struct Impl;
  Impl *p_;
};

}  // namespace rx
