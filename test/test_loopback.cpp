// Tests for the Timecode Loopback driver: loads the built bundle's binary and drives it as coreaudiod would.
//   ./build/mac/test_loopback <driver binary>
#include <CoreAudio/AudioServerPlugIn.h>
#include <dlfcn.h>
#include <mach/mach_time.h>

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "../receiver/core.h"
#include "../receiver/strings.h"

using namespace rx;

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) { g_fail++; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } \
  } while (0)

static AudioServerPlugInDriverRef D = nullptr;
static int g_changes = 0;
static OSStatus host_changed(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *) { return noErr; }
static OSStatus host_copy(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *out) { *out = nullptr; return noErr; }
static OSStatus host_write(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef) { return noErr; }
static OSStatus host_delete(AudioServerPlugInHostRef, CFStringRef) { return noErr; }
static OSStatus host_request(AudioServerPlugInHostRef, AudioObjectID dev, UInt64 action, void *info) {  // coreaudiod applies it at once here
  g_changes++;
  return (*D)->PerformDeviceConfigurationChange(D, dev, action, info);
}
static const AudioServerPlugInHostInterface g_host = {host_changed, host_copy, host_write, host_delete, host_request};

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector s, AudioObjectPropertyScope sc = kAudioObjectPropertyScopeGlobal) {
  return {s, sc, kAudioObjectPropertyElementMain};
}
template <class T> static T get(AudioObjectID id, AudioObjectPropertySelector s, AudioObjectPropertyScope sc = kAudioObjectPropertyScopeGlobal) {
  T v{};
  UInt32 size = 0;
  const AudioObjectPropertyAddress a = addr(s, sc);
  (*D)->GetPropertyData(D, id, 0, &a, 0, nullptr, sizeof(T), &size, &v);
  return v;
}
static std::string get_str(AudioObjectID id, AudioObjectPropertySelector s) {
  CFStringRef v = get<CFStringRef>(id, s);
  if (!v) return "";
  char b[256] = {0};
  CFStringGetCString(v, b, sizeof(b), kCFStringEncodingUTF8);
  CFRelease(v);
  return b;
}
static std::vector<AudioObjectID> get_ids(AudioObjectID id, AudioObjectPropertySelector s, AudioObjectPropertyScope sc = kAudioObjectPropertyScopeGlobal) {
  const AudioObjectPropertyAddress a = addr(s, sc);
  UInt32 size = 0;
  (*D)->GetPropertyDataSize(D, id, 0, &a, 0, nullptr, &size);
  std::vector<AudioObjectID> v(size / sizeof(AudioObjectID));
  (*D)->GetPropertyData(D, id, 0, &a, 0, nullptr, size, &size, v.data());
  return v;
}
static OSStatus set_rate(Float64 r) {
  const AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate);
  return (*D)->SetPropertyData(D, 2, 0, &a, 0, nullptr, sizeof(r), &r);
}

// one IO cycle as the HAL runs it: input read at t - frames, output written at t + frames
static void cycle(Float64 t, UInt32 frames, float *in, const float *out) {
  AudioServerPlugInIOCycleInfo c = {};
  c.mInputTime.mSampleTime = t - frames;
  c.mOutputTime.mSampleTime = t + frames;
  if (in) (*D)->DoIOOperation(D, 2, 3, 1, kAudioServerPlugInIOOperationReadInput, frames, &c, in, nullptr);
  if (out) {
    std::vector<float> o(out, out + frames * 2);
    (*D)->DoIOOperation(D, 2, 4, 1, kAudioServerPlugInIOOperationWriteMix, frames, &c, o.data(), nullptr);
  }
}
static float sig(long t, int c) { return float(((t * 7 + c * 13) % 2000) - 1000) / 1000.0f; }  // distinct per frame and channel

static void test_properties() {
  std::printf("properties\n");
  const auto devs = get_ids(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList);
  CHECK(devs.size() == 1 && devs[0] == 2, "device list: %zu devices", devs.size());
  const std::string name = get_str(2, kAudioObjectPropertyName), uid = get_str(2, kAudioDevicePropertyDeviceUID);
  std::printf("  device \"%s\" (%s), by \"%s\"\n", name.c_str(), uid.c_str(), get_str(2, kAudioObjectPropertyManufacturer).c_str());
  CHECK(name == R::loopbackName, "name %s", name.c_str());
  {
    CFStringRef q = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    AudioObjectID out = 0;
    UInt32 size = 0;
    const AudioObjectPropertyAddress a = addr(kAudioPlugInPropertyTranslateUIDToDevice);
    (*D)->GetPropertyData(D, kAudioObjectPlugInObject, 0, &a, sizeof(q), &q, sizeof(out), &size, &out);
    CFRelease(q);
    CHECK(out == 2, "UID to device: %u", out);
  }
  const auto in = get_ids(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput), out = get_ids(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
  CHECK(in.size() == 1 && in[0] == 3 && out.size() == 1 && out[0] == 4, "streams: %zu in, %zu out", in.size(), out.size());
  CHECK(get<UInt32>(3, kAudioStreamPropertyDirection) == 1 && get<UInt32>(4, kAudioStreamPropertyDirection) == 0, "stream directions");
  CHECK(get<UInt32>(2, kAudioDevicePropertyDeviceCanBeDefaultDevice) == 0 && get<UInt32>(2, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice) == 0, "never the default device");
  const AudioStreamBasicDescription f = get<AudioStreamBasicDescription>(3, kAudioStreamPropertyVirtualFormat);
  std::printf("  format %.0f Hz, %u ch, %u bit float\n", f.mSampleRate, f.mChannelsPerFrame, f.mBitsPerChannel);
  CHECK(f.mChannelsPerFrame == 2 && f.mBitsPerChannel == 32 && (f.mFormatFlags & kAudioFormatFlagIsFloat), "format");
  const AudioObjectPropertyAddress bogus = addr('zzzz');
  CHECK(!(*D)->HasProperty(D, 2, 0, &bogus), "unknown property reported");
  CHECK(set_rate(44100) == noErr && get<Float64>(2, kAudioDevicePropertyNominalSampleRate) == 44100, "rate change to 44100");
  CHECK(get<AudioStreamBasicDescription>(4, kAudioStreamPropertyVirtualFormat).mSampleRate == 44100, "stream format follows the rate");
  CHECK(set_rate(12345) != noErr && get<Float64>(2, kAudioDevicePropertyNominalSampleRate) == 44100, "unsupported rate refused");
  CHECK(set_rate(48000) == noErr && get<Float64>(2, kAudioDevicePropertyNominalSampleRate) == 48000, "rate back to 48000");
  std::printf("  rates: 44100 and 48000 set, 12345 refused, %d configuration change(s)\n", g_changes);
}

static void test_clock() {
  std::printf("clock\n");
  (*D)->StartIO(D, 2, 1);
  Float64 s0; UInt64 h0, seed;
  (*D)->GetZeroTimeStamp(D, 2, 1, &s0, &h0, &seed);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));  // past one period of 16384 frames at 48 kHz (341 ms)
  Float64 s1; UInt64 h1;
  (*D)->GetZeroTimeStamp(D, 2, 1, &s1, &h1, &seed);
  mach_timebase_info_data_t tb; mach_timebase_info(&tb);
  const double secs = double(h1 - h0) * tb.numer / tb.denom * 1e-9;
  std::printf("  zero time stamps: %.0f frames apart, %.6f s apart (%.1f Hz)\n", s1 - s0, secs, (s1 - s0) / secs);
  CHECK(s1 - s0 == 16384 && std::fabs((s1 - s0) / secs - 48000) < 0.5, "zero time stamps: %.0f frames in %.6f s", s1 - s0, secs);
  (*D)->StopIO(D, 2, 1);
}

static void test_loop() {
  std::printf("loopback\n");
  (*D)->StartIO(D, 2, 1);
  const UInt32 n = 512;
  std::vector<float> in(n * 2), out(n * 2);
  long wrong = 0, silentBefore = 0, frames = 0;
  // nothing written yet: input is silence
  cycle(10000, n, in.data(), nullptr);
  for (float x : in) silentBefore += x == 0;
  // 200 cycles of a known signal; input lags output by two cycles
  for (int k = 0; k < 200; k++) {
    const long t = 20000 + long(k) * n;
    for (UInt32 i = 0; i < n; i++) for (int c = 0; c < 2; c++) out[i * 2 + c] = sig(t + n + i, c);
    cycle(t, n, in.data(), out.data());
    if (k < 2) continue;  // input times not written yet
    for (UInt32 i = 0; i < n; i++) { frames++; for (int c = 0; c < 2; c++) wrong += in[i * 2 + c] != sig(t - n + i, c); }
  }
  std::printf("  %ld frames back sample for sample, %ld wrong; before any output %ld of %u samples silent\n", frames, wrong, silentBefore, n * 2);
  CHECK(wrong == 0 && silentBefore == long(n * 2), "loop: %ld wrong, %ld silent before", wrong, silentBefore);
  // the writer stops: the input goes silent once the written frames are read, and stays silent past a ring's length
  const long tStop = 20000 + 200L * n;
  long late = 0, staleAfter = 0;
  for (int k = 0; k < 80; k++) {
    cycle(tStop + long(k) * n, n, in.data(), nullptr);
    for (UInt32 i = 0; i < n * 2; i++) { if (k < 2) late += in[i] != 0; else staleAfter += in[i] != 0; }
  }
  std::printf("  writer stops: %ld samples of written audio still read, then %ld non-silent samples over %d frames\n", late, staleAfter, 78 * n);
  CHECK(staleAfter == 0, "stale audio after the writer stopped: %ld samples", staleAfter);
  // the writer resumes 3000 frames after its last frame (less than the ring): the gap reads silent, not old ring audio
  const long W = tStop + n, tRes = tStop + 3000;  // last frame written + 1; first resumed write lands at W + 3000
  long gapNoise = 0;
  for (int k = 0; k < 6; k++) {
    const long t = tRes + long(k) * n;
    for (UInt32 i = 0; i < n; i++) for (int c = 0; c < 2; c++) out[i * 2 + c] = sig(t + n + i, c);
    cycle(t, n, nullptr, out.data());
  }
  for (int k = 0; k < 5; k++) { cycle(W + n + long(k) * n, n, in.data(), nullptr); for (float x : in) gapNoise += x != 0; }  // reads [W, W + 2560)
  std::printf("  writer resumes after a 3000 frame gap: %ld non-silent samples read in the gap\n", gapNoise);
  CHECK(gapNoise == 0, "gap after resume: %ld non-silent samples", gapNoise);
  (*D)->StopIO(D, 2, 1);
}

// the receiver's LTC through the loop, decoded on the other side
static void test_ltc() {
  std::printf("LTC through the loop\n");
  (*D)->StartIO(D, 2, 1);
  Timeline tl; Settings set; LtcGen gen(tl, set);
  Decoder dec; const double sr = 48000; dec.reset(sr);
  const UInt32 n = 256;
  std::vector<float> mono(n), out(n * 2), in(n * 2);
  const long c0 = tc_to_count(10, 0, 0, 0, 25, false);
  long frames = 0, jumps = 0, prev = -1, next = 0; Frame fr; int fps = 0;
  for (int k = 0; k < int(4.0 * sr / n); k++) {
    const double t = k * n / sr;  // virtual seconds
    for (; next / 25.0 <= t; next++) tl.packet(c0 + next, 1, next / 25.0);  // packets due by now, at their frame starts
    gen.render(mono.data(), n, sr, t, t);
    for (UInt32 i = 0; i < n; i++) out[i * 2] = out[i * 2 + 1] = mono[i];
    cycle(100000.0 + double(k) * n, n, in.data(), out.data());
    for (UInt32 i = 0; i < n; i++) {
      if (dec.pushRaw(in[i * 2 + 1], fr) != 2) continue;
      const long got = tc_to_count(fr.h, fr.m, fr.s, fr.f, fr.fps, fr.df);
      if (prev >= 0 && got != prev + 1) jumps++;
      prev = got; frames++; fps = fr.fps;
    }
  }
  std::printf("  25 fps for 4 s: %ld frames decoded from the input, %ld discontinuities\n", frames, jumps);
  CHECK(frames > 80 && jumps == 0 && fps == 25, "LTC: %ld frames, %ld jumps, fps %d", frames, jumps, fps);
  (*D)->StopIO(D, 2, 1);
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  void *h = dlopen(argv[1], RTLD_NOW);
  auto create = h ? (void *(*)(CFAllocatorRef, CFUUIDRef))dlsym(h, "TimecodeLoopback_Create") : nullptr;
  if (!create) { std::printf("cannot load %s\n", argv[1]); return 1; }
  void *iu = create(nullptr, kAudioServerPlugInTypeUUID);
  LPVOID out = nullptr;
  if (!iu || (*(IUnknownVTbl **)iu)->QueryInterface(iu, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &out) != S_OK) { std::printf("no driver interface\n"); return 1; }
  D = (AudioServerPlugInDriverRef)out;
  (*D)->Initialize(D, &g_host);
  test_properties();
  test_clock();
  test_loop();
  test_ltc();
  std::printf(g_fail ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fail);
  return g_fail ? 1 : 0;
}
