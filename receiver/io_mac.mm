// macOS outputs: AUHAL for LTC, Core MIDI for MTC (any destination, or a source this app creates).
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

#include <mutex>

#include "io.h"
#include "strings.h"

namespace rx {
namespace {

std::string str(CFStringRef s) {
  if (!s) return "";
  char b[512] = {0};
  CFStringGetCString(s, b, sizeof(b), kCFStringEncodingUTF8);
  return b;
}

std::vector<AudioDeviceID> devices() {
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  std::vector<AudioDeviceID> ids;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return ids;
  ids.resize(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) ids.clear();
  return ids;
}
int output_channels(AudioDeviceID d) {
  AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(d, &a, 0, nullptr, &size) != noErr || !size) return 0;
  std::vector<unsigned char> buf(size);
  AudioBufferList *l = (AudioBufferList *)buf.data();
  if (AudioObjectGetPropertyData(d, &a, 0, nullptr, &size, l) != noErr) return 0;
  int n = 0;
  for (UInt32 i = 0; i < l->mNumberBuffers; i++) n += (int)l->mBuffers[i].mNumberChannels;
  return n;
}
std::string cfprop(AudioDeviceID d, AudioObjectPropertySelector sel) {
  AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  CFStringRef s = nullptr;
  UInt32 size = sizeof(s);
  if (AudioObjectGetPropertyData(d, &a, 0, nullptr, &size, &s) != noErr || !s) return "";
  const std::string r = str(s);
  CFRelease(s);
  return r;
}
double host_seconds(uint64_t t) {
  static const double k = [] { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return double(tb.numer) / double(tb.denom) * 1e-9; }();
  return double(t) * k;
}

}  // namespace

std::vector<Device> audio_outputs() {
  std::vector<Device> out;
  for (AudioDeviceID d : devices()) {
    if (output_channels(d) <= 0) continue;
    const std::string uid = cfprop(d, kAudioDevicePropertyDeviceUID);
    if (!uid.empty()) out.push_back({uid, cfprop(d, kAudioObjectPropertyName)});
  }
  return out;
}

struct AudioOut::Impl {
  AudioUnit unit = nullptr;
  AudioDeviceID dev = 0;
  std::atomic<bool> failed{false};
  std::atomic<int> ch{0};
  double srate = 48000;
  Fill fill;

  static OSStatus render(void *ref, AudioUnitRenderActionFlags *, const AudioTimeStamp *ts, UInt32, UInt32 frames, AudioBufferList *io) {
    Impl *p = (Impl *)ref;
    const int ch = p->ch.load(std::memory_order_relaxed);
    double delay = 0;
    if (ts && (ts->mFlags & kAudioTimeStampHostTimeValid)) delay = host_seconds(ts->mHostTime) - host_seconds(mach_absolute_time());
    if (io->mNumberBuffers == 1 && ch > 0) p->fill((float *)io->mBuffers[0].mData, frames, ch, p->srate, delay > 0 ? delay : 0);
    return noErr;
  }
  static OSStatus alive(AudioObjectID, UInt32, const AudioObjectPropertyAddress *, void *ref) {
    ((Impl *)ref)->failed = true;
    return noErr;
  }
  bool open(const std::string &id) {
    for (AudioDeviceID d : devices()) if (cfprop(d, kAudioDevicePropertyDeviceUID) == id) dev = d;
    if (!dev) return false;
    AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &unit) != noErr) return false;
    if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof(dev)) != noErr) return false;
    AudioStreamBasicDescription hw;
    UInt32 size = sizeof(hw);
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &hw, &size) != noErr) return false;
    srate = hw.mSampleRate;
    const int n = (int)hw.mChannelsPerFrame;
    AudioStreamBasicDescription f = {};
    f.mSampleRate = srate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;  // interleaved
    f.mChannelsPerFrame = (UInt32)n;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = (UInt32)(4 * n);
    f.mFramesPerPacket = 1;
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof(f)) != noErr) return false;
    AURenderCallbackStruct cb = {render, this};
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr) return false;
    if (AudioUnitInitialize(unit) != noErr) return false;
    ch = n;
    AudioObjectPropertyAddress a = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListener(dev, &a, alive, this);
    return AudioOutputUnitStart(unit) == noErr;
  }
  void close() {
    if (dev) {
      AudioObjectPropertyAddress a = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
      AudioObjectRemovePropertyListener(dev, &a, alive, this);
    }
    if (unit) { AudioOutputUnitStop(unit); AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); unit = nullptr; }
    dev = 0; ch = 0;
  }
};

AudioOut::AudioOut() : p_(new Impl) {}
AudioOut::~AudioOut() { stop(); delete p_; }
bool AudioOut::start(const std::string &id, Fill fill) {
  stop();
  p_->fill = std::move(fill);
  const bool ok = p_->open(id);
  if (!ok) { p_->close(); p_->failed = true; }
  return ok;
}
void AudioOut::stop() { p_->close(); p_->failed = false; }
int AudioOut::channels() const { return p_->ch.load(); }
bool AudioOut::failed() const { return p_->failed.load(); }

// ---- MIDI ---------------------------------------------------------------------------------------------------------
namespace {
constexpr const char *kVirtual = "virtual";
MIDIClientRef client() {
  static MIDIClientRef c = [] { MIDIClientRef r = 0; MIDIClientCreate(CFSTR("Art-Net Timecode Receiver"), nullptr, nullptr, &r); return r; }();
  return c;
}
}  // namespace

std::vector<Device> midi_outputs() {
  std::vector<Device> out{{kVirtual, R::virtualPort}};
  client();
  const ItemCount n = MIDIGetNumberOfDestinations();
  for (ItemCount i = 0; i < n; i++) {
    MIDIEndpointRef e = MIDIGetDestination(i);
    SInt32 uid = 0;
    CFStringRef name = nullptr;
    if (!e || MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid) != noErr) continue;
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name);
    out.push_back({std::to_string(uid), str(name)});
    if (name) CFRelease(name);
  }
  return out;
}

struct MidiOut::Impl {
  MIDIPortRef port = 0;
  MIDIEndpointRef dest = 0, source = 0;
  std::mutex mu;
};
MidiOut::MidiOut() : p_(new Impl) {}
MidiOut::~MidiOut() { close(); delete p_; }
bool MidiOut::open(const std::string &id) {
  close();
  std::lock_guard<std::mutex> g(p_->mu);
  if (id == kVirtual) {
    CFStringRef name = CFStringCreateWithCString(nullptr, R::virtualPort, kCFStringEncodingUTF8);
    const OSStatus r = MIDISourceCreate(client(), name, &p_->source);
    CFRelease(name);
    return r == noErr;
  }
  const ItemCount n = MIDIGetNumberOfDestinations();
  for (ItemCount i = 0; i < n; i++) {
    MIDIEndpointRef e = MIDIGetDestination(i);
    SInt32 uid = 0;
    if (e && MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid) == noErr && std::to_string(uid) == id) p_->dest = e;
  }
  return p_->dest && MIDIOutputPortCreate(client(), CFSTR("out"), &p_->port) == noErr;
}
void MidiOut::close() {
  std::lock_guard<std::mutex> g(p_->mu);
  if (p_->port) { MIDIPortDispose(p_->port); p_->port = 0; }
  if (p_->source) { MIDIEndpointDispose(p_->source); p_->source = 0; }
  p_->dest = 0;
}
void MidiOut::send(const unsigned char *b, int n) {
  std::lock_guard<std::mutex> g(p_->mu);
  if (n <= 0 || n > 64) return;
  Byte buf[256];
  MIDIPacketList *list = (MIDIPacketList *)buf;
  MIDIPacket *pk = MIDIPacketListInit(list);
  pk = MIDIPacketListAdd(list, sizeof(buf), pk, 0, (ByteCount)n, b);
  if (!pk) return;
  if (p_->source) MIDIReceived(p_->source, list);
  else if (p_->port && p_->dest) MIDISend(p_->port, p_->dest, list);
}

}  // namespace rx
