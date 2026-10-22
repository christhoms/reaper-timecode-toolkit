// Windows outputs: WASAPI shared mode (event driven) for LTC, WinMM for MTC.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <propidl.h>

#include <cstring>
#include <mutex>
#include <thread>

#include "io.h"

namespace rx {
namespace {

const GUID kClsEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const GUID kIidEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const GUID kIidAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const GUID kIidRenderClient = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::string utf8(const wchar_t *w) {
  if (!w) return "";
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, 0);
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}
std::wstring wide(const std::string &s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, 0);
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

struct Com {
  bool ok;
  Com() : ok(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
  ~Com() { if (ok) CoUninitialize(); }
};
template <class T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

}  // namespace

std::vector<Device> audio_outputs() {
  std::vector<Device> out;
  std::thread([&] {  // own thread: the caller's COM apartment is the window's
    Com com;
    IMMDeviceEnumerator *en = nullptr;
    IMMDeviceCollection *col = nullptr;
    if (FAILED(CoCreateInstance(kClsEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, (void **)&en))) return;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
      UINT n = 0;
      col->GetCount(&n);
      for (UINT i = 0; i < n; i++) {
        IMMDevice *d = nullptr;
        if (FAILED(col->Item(i, &d))) continue;
        Device dev;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) { dev.id = utf8(id); CoTaskMemFree(id); }
        IPropertyStore *ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
          PROPVARIANT v;
          PropVariantInit(&v);
          if (SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) dev.name = utf8(v.pwszVal);
          PropVariantClear(&v);
          ps->Release();
        }
        if (dev.name.empty()) dev.name = dev.id;
        if (!dev.id.empty()) out.push_back(dev);
        d->Release();
      }
    }
    release(col);
    release(en);
  }).join();
  return out;
}

struct AudioOut::Impl {
  std::thread th;
  std::atomic<bool> run{false}, failed{false};
  std::atomic<int> ch{0};
  Fill fill;
  std::string id;

  void loop() {
    Com com;
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *dev = nullptr;
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    WAVEFORMATEX *wf = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = false;
    UINT32 bufFrames = 0;
    REFERENCE_TIME latency = 0;
    bool isFloat = false;
    do {
      if (FAILED(CoCreateInstance(kClsEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, (void **)&en))) break;
      if (FAILED(en->GetDevice(wide(id).c_str(), &dev))) break;
      if (FAILED(dev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, (void **)&client))) break;
      if (FAILED(client->GetMixFormat(&wf))) break;
      isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE *)wf)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
      if (!((isFloat && wf->wBitsPerSample == 32) || (!isFloat && wf->wBitsPerSample == 16))) break;
      if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, wf, nullptr))) break;
      if (FAILED(client->SetEventHandle(ev))) break;
      if (FAILED(client->GetBufferSize(&bufFrames))) break;
      if (FAILED(client->GetService(kIidRenderClient, (void **)&render))) break;
      client->GetStreamLatency(&latency);
      if (FAILED(client->Start())) break;
      ok = true;
    } while (false);
    if (!ok) failed = true;
    DWORD task = 0;
    HANDLE mm = ok ? AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) : nullptr;
    std::vector<float> tmp;
    if (ok) ch = wf->nChannels;
    while (ok && run) {
      if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) {
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) { failed = true; break; }  // device went away
        continue;
      }
      UINT32 pad = 0;
      if (FAILED(client->GetCurrentPadding(&pad))) { failed = true; break; }
      const UINT32 n = bufFrames - pad;
      if (!n) continue;
      BYTE *data = nullptr;
      if (FAILED(render->GetBuffer(n, &data))) { failed = true; break; }
      const int c = wf->nChannels;
      const double delay = double(pad) / wf->nSamplesPerSec + double(latency) * 1e-7;
      if (isFloat) {
        fill((float *)data, n, c, wf->nSamplesPerSec, delay);
      } else {
        tmp.resize(size_t(n) * c);
        fill(tmp.data(), n, c, wf->nSamplesPerSec, delay);
        short *o = (short *)data;
        for (size_t i = 0; i < tmp.size(); i++) o[i] = (short)(tmp[i] * 32767.0f);
      }
      render->ReleaseBuffer(n, 0);
    }
    if (client && ok) client->Stop();
    if (mm) AvRevertMmThreadCharacteristics(mm);
    ch = 0;
    if (wf) CoTaskMemFree(wf);
    release(render); release(client); release(dev); release(en);
    CloseHandle(ev);
  }
};

AudioOut::AudioOut() : p_(new Impl) {}
AudioOut::~AudioOut() { stop(); delete p_; }
bool AudioOut::start(const std::string &id, Fill fill) {
  stop();
  p_->id = id; p_->fill = std::move(fill); p_->failed = false; p_->run = true;
  p_->th = std::thread([this] { p_->loop(); });
  return true;
}
void AudioOut::stop() {
  p_->run = false;
  if (p_->th.joinable()) p_->th.join();
  p_->failed = false;
}
int AudioOut::channels() const { return p_->ch.load(); }
bool AudioOut::failed() const { return p_->failed.load(); }

// ---- MIDI ---------------------------------------------------------------------------------------------------------
std::vector<Device> midi_outputs() {
  std::vector<Device> out;
  const UINT n = midiOutGetNumDevs();
  for (UINT i = 0; i < n; i++) {
    MIDIOUTCAPSW caps;
    if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
    const std::string name = utf8(caps.szPname);
    out.push_back({name, name});
  }
  return out;
}

struct MidiOut::Impl {
  HMIDIOUT h = nullptr;
  std::mutex mu;
};
MidiOut::MidiOut() : p_(new Impl) {}
MidiOut::~MidiOut() { close(); delete p_; }
bool MidiOut::open(const std::string &id) {
  close();
  const UINT n = midiOutGetNumDevs();
  for (UINT i = 0; i < n; i++) {
    MIDIOUTCAPSW caps;
    if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR || utf8(caps.szPname) != id) continue;
    std::lock_guard<std::mutex> g(p_->mu);
    return midiOutOpen(&p_->h, i, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
  }
  return false;
}
void MidiOut::close() {
  std::lock_guard<std::mutex> g(p_->mu);
  if (p_->h) { midiOutReset(p_->h); midiOutClose(p_->h); p_->h = nullptr; }
}
bool MidiOut::failed() const { return false; }
void bluetooth_scan(bool, std::function<void()>) {}
void midi_set_name(const char *) {}
void MidiOut::send(const unsigned char *b, int n) {
  std::lock_guard<std::mutex> g(p_->mu);
  if (!p_->h || n <= 0) return;
  if (b[0] != 0xF0) {
    DWORD m = 0;
    for (int i = 0; i < n && i < 3; i++) m |= DWORD(b[i]) << (8 * i);
    midiOutShortMsg(p_->h, m);
    return;
  }
  MIDIHDR hdr;
  std::memset(&hdr, 0, sizeof(hdr));
  hdr.lpData = (LPSTR)b;
  hdr.dwBufferLength = hdr.dwBytesRecorded = (DWORD)n;
  if (midiOutPrepareHeader(p_->h, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) return;
  midiOutLongMsg(p_->h, &hdr, sizeof(hdr));
  for (int i = 0; i < 50 && midiOutUnprepareHeader(p_->h, &hdr, sizeof(hdr)) == MIDIERR_STILLPLAYING; i++) Sleep(1);
}

}  // namespace rx
#endif
