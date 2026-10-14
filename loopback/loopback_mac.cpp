// Timecode Loopback: a two-channel Core Audio device (AudioServerPlugIn). What an app plays to its output comes back on
// its input, so the receiver's LTC reaches an app that reads timecode from an audio input (Resolume).
//
// Objects: the plug-in, one device, one input stream, one output stream. No controls.
// The ring holds the output by sample time; input reads the same sample times later. Frames nobody wrote read as silence,
// so a stopped writer never loops stale audio.
#include <CoreAudio/AudioServerPlugIn.h>
#include <mach/mach_time.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

#include "../receiver/strings.h"
#include "../src/strings.h"

namespace {

enum : AudioObjectID { kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kStreamIn = 3, kStreamOut = 4 };
constexpr const char *kDeviceUID = "uk.co.christhoms.timecode-loopback";
constexpr const char *kModelUID = "uk.co.christhoms.timecode-loopback.model";
constexpr UInt32 kChannels = 2;
constexpr UInt32 kRing = 16384;  // frames; also the zero time stamp period
constexpr Float64 kRates[] = {44100, 48000, 88200, 96000};

AudioServerPlugInHostRef gHost = nullptr;
std::atomic<UInt32> gRefs{1};
pthread_mutex_t gMu = PTHREAD_MUTEX_INITIALIZER;  // rate, running count, clock anchor
Float64 gRate = 48000, gPendingRate = 48000;
UInt32 gRunning = 0;
UInt64 gAnchor = 0, gStamps = 0;
Float64 gTicksPerFrame = 0;
bool gActive[2] = {true, true};  // input, output stream IsActive

// IO thread only
Float32 gRing[kRing * kChannels];
SInt64 gWriteEnd = -1;  // sample time after the last frame written; -1 = nothing written

bool rate_ok(Float64 r) {
  for (Float64 k : kRates) if (k == r) return true;
  return false;
}
void set_ticks() {
  mach_timebase_info_data_t tb;
  mach_timebase_info(&tb);
  gTicksPerFrame = (1e9 * double(tb.denom) / double(tb.numer)) / gRate;
}
AudioStreamBasicDescription format(Float64 rate) {
  AudioStreamBasicDescription f = {};
  f.mSampleRate = rate;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
  f.mBytesPerPacket = f.mBytesPerFrame = 4 * kChannels;
  f.mFramesPerPacket = 1;
  f.mChannelsPerFrame = kChannels;
  f.mBitsPerChannel = 32;
  return f;
}

// One answer for HasProperty, GetPropertyDataSize and GetPropertyData: with sizeOnly the size is reported, nothing written.
struct Reply {
  UInt32 cap;
  UInt32 *size;
  void *data;
  bool sizeOnly;
  template <class T> OSStatus val(const T &v) {
    *size = sizeof(T);
    if (sizeOnly) return noErr;
    if (cap < sizeof(T)) return kAudioHardwareBadPropertySizeError;
    std::memcpy(data, &v, sizeof(T));
    return noErr;
  }
  template <class T> OSStatus arr(const T *v, UInt32 n) {  // as many items as fit
    if (sizeOnly) { *size = n * sizeof(T); return noErr; }
    const UInt32 k = std::min<UInt32>(n, cap / sizeof(T));
    if (k) std::memcpy(data, v, k * sizeof(T));
    *size = k * sizeof(T);
    return noErr;
  }
  OSStatus str(const char *s) {  // the caller releases it
    *size = sizeof(CFStringRef);
    if (sizeOnly) return noErr;
    if (cap < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
    CFStringRef r = CFStringCreateWithCString(nullptr, s, kCFStringEncodingUTF8);
    std::memcpy(data, &r, sizeof(r));
    return noErr;
  }
};

OSStatus plugin_prop(const AudioObjectPropertyAddress &a, UInt32 qs, const void *q, Reply &r) {
  const AudioObjectID dev = kDevice;
  switch (a.mSelector) {
    case kAudioObjectPropertyBaseClass: return r.val<AudioClassID>(kAudioObjectClassID);
    case kAudioObjectPropertyClass: return r.val<AudioClassID>(kAudioPlugInClassID);
    case kAudioObjectPropertyOwner: return r.val<AudioObjectID>(kAudioObjectUnknown);
    case kAudioObjectPropertyManufacturer: return r.str(S::vendor);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList: return r.arr(&dev, 1);
    case kAudioPlugInPropertyTranslateUIDToDevice: {
      AudioObjectID out = kAudioObjectUnknown;
      if (!r.sizeOnly && qs == sizeof(CFStringRef) && q) {
        CFStringRef uid = *(const CFStringRef *)q;
        CFStringRef mine = CFStringCreateWithCString(nullptr, kDeviceUID, kCFStringEncodingUTF8);
        if (uid && CFStringCompare(uid, mine, 0) == kCFCompareEqualTo) out = kDevice;
        CFRelease(mine);
      }
      return r.val(out);
    }
    case kAudioPlugInPropertyResourceBundle: return r.str("");
    default: return kAudioHardwareUnknownPropertyError;
  }
}

OSStatus device_prop(const AudioObjectPropertyAddress &a, Reply &r) {
  const AudioObjectID both[2] = {kStreamIn, kStreamOut};
  const AudioObjectID *streams = a.mScope == kAudioObjectPropertyScopeOutput ? both + 1 : both;
  const UInt32 nStreams = a.mScope == kAudioObjectPropertyScopeGlobal ? 2 : 1;
  switch (a.mSelector) {
    case kAudioObjectPropertyBaseClass: return r.val<AudioClassID>(kAudioObjectClassID);
    case kAudioObjectPropertyClass: return r.val<AudioClassID>(kAudioDeviceClassID);
    case kAudioObjectPropertyOwner: return r.val<AudioObjectID>(kPlugIn);
    case kAudioObjectPropertyName: return r.str(R::loopbackName);
    case kAudioObjectPropertyManufacturer: return r.str(S::vendor);
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams: return r.arr(streams, nStreams);
    case kAudioObjectPropertyControlList: return r.arr<AudioObjectID>(nullptr, 0);
    case kAudioDevicePropertyDeviceUID: return r.str(kDeviceUID);
    case kAudioDevicePropertyModelUID: return r.str(kModelUID);
    case kAudioDevicePropertyTransportType: return r.val<UInt32>(kAudioDeviceTransportTypeVirtual);
    case kAudioDevicePropertyRelatedDevices: { const AudioObjectID d = kDevice; return r.arr(&d, 1); }
    case kAudioDevicePropertyClockDomain: return r.val<UInt32>(0);
    case kAudioDevicePropertyDeviceIsAlive: return r.val<UInt32>(1);
    case kAudioDevicePropertyDeviceIsRunning: { pthread_mutex_lock(&gMu); const UInt32 v = gRunning > 0; pthread_mutex_unlock(&gMu); return r.val(v); }
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:       // never picked as the Mac's sound output or input
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: return r.val<UInt32>(0);
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset: return r.val<UInt32>(0);
    case kAudioDevicePropertyNominalSampleRate: { pthread_mutex_lock(&gMu); const Float64 v = gRate; pthread_mutex_unlock(&gMu); return r.val(v); }
    case kAudioDevicePropertyAvailableNominalSampleRates: {
      AudioValueRange v[4];
      for (int i = 0; i < 4; i++) v[i] = {kRates[i], kRates[i]};
      return r.arr(v, 4);
    }
    case kAudioDevicePropertyIsHidden: return r.val<UInt32>(0);
    case kAudioDevicePropertyZeroTimeStampPeriod: return r.val<UInt32>(kRing);
    case kAudioDevicePropertyClockIsStable: return r.val<UInt32>(1);
    case kAudioDevicePropertyPreferredChannelsForStereo: { const UInt32 v[2] = {1, 2}; return r.arr(v, 2); }
    case kAudioDevicePropertyPreferredChannelLayout: {
      const UInt32 n = offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription);
      *r.size = n;
      if (r.sizeOnly) return noErr;
      if (r.cap < n) return kAudioHardwareBadPropertySizeError;
      AudioChannelLayout *l = (AudioChannelLayout *)r.data;
      std::memset(l, 0, n);
      l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
      l->mNumberChannelDescriptions = kChannels;
      l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
      l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
      return noErr;
    }
    default: return kAudioHardwareUnknownPropertyError;
  }
}

OSStatus stream_prop(AudioObjectID id, const AudioObjectPropertyAddress &a, Reply &r) {
  const bool in = id == kStreamIn;
  switch (a.mSelector) {
    case kAudioObjectPropertyBaseClass: return r.val<AudioClassID>(kAudioObjectClassID);
    case kAudioObjectPropertyClass: return r.val<AudioClassID>(kAudioStreamClassID);
    case kAudioObjectPropertyOwner: return r.val<AudioObjectID>(kDevice);
    case kAudioObjectPropertyOwnedObjects: return r.arr<AudioObjectID>(nullptr, 0);
    case kAudioStreamPropertyIsActive: { pthread_mutex_lock(&gMu); const UInt32 v = gActive[in ? 0 : 1]; pthread_mutex_unlock(&gMu); return r.val(v); }
    case kAudioStreamPropertyDirection: return r.val<UInt32>(in ? 1 : 0);
    case kAudioStreamPropertyTerminalType: return r.val<UInt32>(kAudioStreamTerminalTypeLine);
    case kAudioStreamPropertyStartingChannel: return r.val<UInt32>(1);
    case kAudioStreamPropertyLatency: return r.val<UInt32>(0);
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: { pthread_mutex_lock(&gMu); const Float64 rate = gRate; pthread_mutex_unlock(&gMu); return r.val(format(rate)); }
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
      AudioStreamRangedDescription v[4];
      for (int i = 0; i < 4; i++) { v[i].mFormat = format(kRates[i]); v[i].mSampleRateRange = {kRates[i], kRates[i]}; }
      return r.arr(v, 4);
    }
    default: return kAudioHardwareUnknownPropertyError;
  }
}

OSStatus get(AudioObjectID id, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, Reply &r) {
  if (!a) return kAudioHardwareIllegalOperationError;
  switch (id) {
    case kPlugIn: return plugin_prop(*a, qs, q, r);
    case kDevice: return device_prop(*a, r);
    case kStreamIn:
    case kStreamOut: return stream_prop(id, *a, r);
    default: return kAudioHardwareBadObjectError;
  }
}

OSStatus request_rate(Float64 rate) {
  if (!rate_ok(rate)) return kAudioHardwareIllegalOperationError;
  pthread_mutex_lock(&gMu);
  const bool same = rate == gRate;
  gPendingRate = rate;
  pthread_mutex_unlock(&gMu);
  if (same) return noErr;
  return gHost ? gHost->RequestDeviceConfigurationChange(gHost, kDevice, 0, nullptr) : (OSStatus)kAudioHardwareUnspecifiedError;
}

// ---- the driver interface -------------------------------------------------------------------------------------------
AudioServerPlugInDriverInterface gInterface;
AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
AudioServerPlugInDriverRef gDriver = &gInterfacePtr;

HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out) {
  if (!out) return kAudioHardwareIllegalOperationError;
  CFUUIDRef want = CFUUIDCreateFromUUIDBytes(nullptr, iid);
  const bool ok = CFEqual(want, IUnknownUUID) || CFEqual(want, kAudioServerPlugInDriverInterfaceUUID);
  CFRelease(want);
  if (!ok) { *out = nullptr; return E_NOINTERFACE; }
  gRefs++;
  *out = gDriver;
  return S_OK;
}
ULONG AddRef(void *) { return ++gRefs; }
ULONG Release(void *) { const UInt32 n = gRefs.load(); if (n > 0) return --gRefs; return 0; }

OSStatus Initialize(AudioServerPlugInDriverRef, AudioServerPlugInHostRef host) {
  gHost = host;
  pthread_mutex_lock(&gMu);
  set_ticks();
  pthread_mutex_unlock(&gMu);
  return noErr;
}
OSStatus CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *) { return kAudioHardwareUnsupportedOperationError; }
OSStatus DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID) { return kAudioHardwareUnsupportedOperationError; }
OSStatus AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *) { return noErr; }
OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *) { return noErr; }
OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID dev, UInt64, void *) {
  if (dev != kDevice) return kAudioHardwareBadObjectError;
  pthread_mutex_lock(&gMu);
  gRate = gPendingRate;
  set_ticks();
  pthread_mutex_unlock(&gMu);
  return noErr;
}
OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *) { return noErr; }

Boolean HasProperty(AudioServerPlugInDriverRef, AudioObjectID id, pid_t, const AudioObjectPropertyAddress *a) {
  UInt32 size = 0;
  Reply r{0, &size, nullptr, true};
  return get(id, a, 0, nullptr, r) == noErr;
}
OSStatus IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID id, pid_t, const AudioObjectPropertyAddress *a, Boolean *out) {
  UInt32 size = 0;
  Reply r{0, &size, nullptr, true};
  const OSStatus e = get(id, a, 0, nullptr, r);
  if (e) return e;
  const UInt32 s = a->mSelector;
  *out = (id == kDevice && s == kAudioDevicePropertyNominalSampleRate) ||
         ((id == kStreamIn || id == kStreamOut) && (s == kAudioStreamPropertyIsActive || s == kAudioStreamPropertyVirtualFormat || s == kAudioStreamPropertyPhysicalFormat));
  return noErr;
}
OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID id, pid_t, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 *out) {
  Reply r{0, out, nullptr, true};
  return get(id, a, qs, q, r);
}
OSStatus GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID id, pid_t, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 cap, UInt32 *outSize, void *out) {
  Reply r{cap, outSize, out, false};
  return get(id, a, qs, q, r);
}
OSStatus SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID id, pid_t, const AudioObjectPropertyAddress *a, UInt32, const void *, UInt32 size, const void *data) {
  if (!a || !data) return kAudioHardwareIllegalOperationError;
  if (id == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
    if (size < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
    return request_rate(*(const Float64 *)data);
  }
  if (id == kStreamIn || id == kStreamOut) {
    if (a->mSelector == kAudioStreamPropertyIsActive) {
      if (size < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
      pthread_mutex_lock(&gMu);
      gActive[id == kStreamIn ? 0 : 1] = *(const UInt32 *)data != 0;
      pthread_mutex_unlock(&gMu);
      if (gHost) gHost->PropertiesChanged(gHost, id, 1, a);
      return noErr;
    }
    if (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat) {
      if (size < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
      const AudioStreamBasicDescription &f = *(const AudioStreamBasicDescription *)data;
      const AudioStreamBasicDescription mine = format(f.mSampleRate);
      if (f.mFormatID != mine.mFormatID || f.mFormatFlags != mine.mFormatFlags || f.mChannelsPerFrame != kChannels || f.mBitsPerChannel != 32)
        return kAudioDeviceUnsupportedFormatError;
      return request_rate(f.mSampleRate);
    }
  }
  return kAudioHardwareUnknownPropertyError;
}

OSStatus StartIO(AudioServerPlugInDriverRef, AudioObjectID dev, UInt32) {
  if (dev != kDevice) return kAudioHardwareBadObjectError;
  pthread_mutex_lock(&gMu);
  if (gRunning++ == 0) {
    gStamps = 0;
    gAnchor = mach_absolute_time();
    std::memset(gRing, 0, sizeof(gRing));
    gWriteEnd = -1;
  }
  pthread_mutex_unlock(&gMu);
  return noErr;
}
OSStatus StopIO(AudioServerPlugInDriverRef, AudioObjectID dev, UInt32) {
  if (dev != kDevice) return kAudioHardwareBadObjectError;
  pthread_mutex_lock(&gMu);
  if (gRunning > 0) gRunning--;
  pthread_mutex_unlock(&gMu);
  return noErr;
}
OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID dev, UInt32, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
  if (dev != kDevice) return kAudioHardwareBadObjectError;
  pthread_mutex_lock(&gMu);
  const Float64 ticksPerRing = gTicksPerFrame * kRing;
  if (gAnchor + UInt64(double(gStamps + 1) * ticksPerRing) <= mach_absolute_time()) gStamps++;
  *sampleTime = Float64(gStamps) * kRing;
  *hostTime = gAnchor + UInt64(double(gStamps) * ticksPerRing);
  *seed = 1;
  pthread_mutex_unlock(&gMu);
  return noErr;
}
OSStatus WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32 op, Boolean *willDo, Boolean *inPlace) {
  *willDo = op == kAudioServerPlugInIOOperationReadInput || op == kAudioServerPlugInIOOperationWriteMix;
  *inPlace = true;
  return noErr;
}
OSStatus BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *) { return noErr; }
OSStatus EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *) { return noErr; }

void zero_ring(SInt64 from, SInt64 n) {
  for (SInt64 i = 0; i < std::min<SInt64>(n, kRing); i++) {
    Float32 *f = gRing + size_t((from + i) % kRing) * kChannels;
    for (UInt32 c = 0; c < kChannels; c++) f[c] = 0;
  }
}
OSStatus DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID dev, AudioObjectID, UInt32, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *buf, void *) {
  if (dev != kDevice) return kAudioHardwareBadObjectError;
  if (!cycle || !buf || frames > kRing) return noErr;
  Float32 *io = (Float32 *)buf;
  if (op == kAudioServerPlugInIOOperationWriteMix) {
    const SInt64 t = SInt64(cycle->mOutputTime.mSampleTime);
    if (t < 0) return noErr;
    if (gWriteEnd < 0 || t < gWriteEnd - SInt64(kRing)) zero_ring(0, kRing);  // first write, or time went back
    else if (t > gWriteEnd) zero_ring(gWriteEnd, t - gWriteEnd);           // frames skipped since the last write
    for (UInt32 i = 0; i < frames; i++) std::memcpy(gRing + size_t((t + i) % kRing) * kChannels, io + size_t(i) * kChannels, 4 * kChannels);
    gWriteEnd = std::max(gWriteEnd, t + SInt64(frames));
  } else if (op == kAudioServerPlugInIOOperationReadInput) {
    const SInt64 t = SInt64(cycle->mInputTime.mSampleTime);
    for (UInt32 i = 0; i < frames; i++) {
      const SInt64 k = t + i;
      Float32 *o = io + size_t(i) * kChannels;
      if (gWriteEnd >= 0 && k >= 0 && k < gWriteEnd && k >= gWriteEnd - SInt64(kRing)) std::memcpy(o, gRing + size_t(k % kRing) * kChannels, 4 * kChannels);
      else std::memset(o, 0, 4 * kChannels);
    }
  }
  return noErr;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void *TimecodeLoopback_Create(CFAllocatorRef, CFUUIDRef type) {
  if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return nullptr;
  gInterface = {nullptr, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
                PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
                GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation,
                EndIOOperation};
  return gDriver;
}
