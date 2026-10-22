// macOS MTC output: Core MIDI (any destination, or a source this process creates) and Bluetooth LE MIDI devices (ble.h).
// A Bluetooth device CoreMIDI has online goes through CoreMIDI; any other one is written directly by the ble backend.
#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <set>

#include "ble.h"
#include "core.h"
#include "io.h"
#include "strings.h"

namespace rx {
namespace {
constexpr const char *kVirtual = "virtual";
constexpr const char *kBle = "ble:";  // + the peripheral's UUID: a Bluetooth MIDI device, connected or not
std::string &app_name() { static std::string n = R::virtualPort; return n; }
std::string str(CFStringRef s) {
  if (!s) return "";
  char b[512] = {0};
  CFStringGetCString(s, b, sizeof(b), kCFStringEncodingUTF8);
  return b;
}
CFStringRef cfstr(const std::string &s) { return CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8); }
MIDIClientRef client() {
  static MIDIClientRef c = [] {
    MIDIClientRef r = 0;
    CFStringRef n = cfstr(app_name());
    MIDIClientCreate(n, nullptr, nullptr, &r);
    CFRelease(n);
    return r;
  }();
  return c;
}
std::string sprop(MIDIObjectRef o, CFStringRef key) {
  CFStringRef s = nullptr;
  if (!o || MIDIObjectGetStringProperty(o, key, &s) != noErr || !s) return "";
  const std::string r = str(s);
  CFRelease(s);
  return r;
}
SInt32 iprop(MIDIObjectRef o, CFStringRef key) {
  SInt32 v = 0;
  if (o) MIDIObjectGetIntegerProperty(o, key, &v);
  return v;
}
MIDIDeviceRef device_of(MIDIEndpointRef e) {
  MIDIEntityRef en = 0;
  MIDIDeviceRef d = 0;
  if (MIDIEndpointGetEntity(e, &en) == noErr && en) MIDIEntityGetDevice(en, &d);
  return d;
}
// the Bluetooth driver's devices carry the peripheral's UUID (keys not in the SDK headers; read from macOS 26)
std::string ble_uuid(MIDIDeviceRef d) { return sprop(d, CFSTR("BLE MIDI Device UUID")); }
bool remote_peripheral(MIDIDeviceRef d) { return iprop(d, CFSTR("MIDI Remote Peripheral")) != 0; }
MIDIEndpointRef ble_destination(const std::string &uuid) {
  const ItemCount n = MIDIGetNumberOfDestinations();
  for (ItemCount i = 0; i < n; i++) {
    MIDIEndpointRef e = MIDIGetDestination(i);
    if (e && ble_uuid(device_of(e)) == uuid) return e;
  }
  return 0;
}
double host_seconds(uint64_t t) {
  static const double k = [] { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return double(tb.numer) / double(tb.denom) * 1e-9; }();
  return double(t) * k;
}
}  // namespace

void midi_set_name(const char *name) { app_name() = name; }
void bluetooth_scan(bool on, std::function<void()> changed) { ble::scan(on, std::move(changed)); }

std::vector<Device> midi_outputs() {
  std::vector<Device> out{{kVirtual, app_name()}};
  client();
  std::set<std::string> have;  // Bluetooth UUIDs listed
  const ItemCount n = MIDIGetNumberOfDestinations();
  for (ItemCount i = 0; i < n; i++) {
    MIDIEndpointRef e = MIDIGetDestination(i);
    SInt32 uid = 0;
    if (!e || MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid) != noErr) continue;
    const std::string u = ble_uuid(device_of(e));
    if (!u.empty()) have.insert(u);
    out.push_back({u.empty() ? std::to_string(uid) : kBle + u, sprop(e, kMIDIPropertyDisplayName)});
  }
  for (const Device &h : ble::held())  // connected by this process or the helper
    if (have.insert(h.id).second) out.push_back({kBle + h.id, h.name});
  const ItemCount nd = MIDIGetNumberOfDevices();  // connected to this Mac before, offline now
  for (ItemCount i = 0; i < nd; i++) {
    MIDIDeviceRef d = MIDIGetDevice(i);
    const std::string u = ble_uuid(d);
    if (u.empty() || have.count(u) || !remote_peripheral(d) || !iprop(d, kMIDIPropertyOffline)) continue;
    have.insert(u);
    out.push_back({kBle + u, R::bluetooth(sprop(d, kMIDIPropertyName))});
  }
  for (const Device &f : ble::found())  // advertising now
    if (have.insert(f.id).second) out.push_back({kBle + f.id, R::bluetooth(f.name)});
  return out;
}

namespace {
struct Link {  // a Bluetooth device's state, told from the backend
  std::mutex mu;
  bool up = false, failed = false, live = true;
};
}  // namespace

struct MidiOut::Impl {
  MIDIPortRef port = 0;
  MIDIEndpointRef dest = 0, source = 0;
  std::string ble;  // UUID of a Bluetooth device written directly
  std::shared_ptr<Link> link = std::make_shared<Link>();
  std::mutex mu;
};
MidiOut::MidiOut() : p_(new Impl) {}
MidiOut::~MidiOut() { close(); delete p_; }
bool MidiOut::open(const std::string &id) {
  close();
  std::lock_guard<std::mutex> g(p_->mu);
  if (id == kVirtual) {
    CFStringRef name = cfstr(app_name());
    const OSStatus r = MIDISourceCreate(client(), name, &p_->source);
    CFRelease(name);
    return r == noErr;
  }
  if (id.rfind(kBle, 0) == 0) {
    const std::string uuid = id.substr(std::strlen(kBle));
    if ((p_->dest = ble_destination(uuid))) return MIDIOutputPortCreate(client(), CFSTR("out"), &p_->port) == noErr;
    std::shared_ptr<Link> link = std::make_shared<Link>();
    p_->link = link;
    p_->ble = uuid;
    ble::want(uuid, [link](bool up) {
      std::lock_guard<std::mutex> lg(link->mu);
      link->up = up;
      if (link->live) link->failed = !up;
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      std::lock_guard<std::mutex> lg(link->mu);
      if (link->live && !link->up) link->failed = true;  // still trying: shown, not given up
    });
    return true;
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
  {
    std::lock_guard<std::mutex> lg(p_->link->mu);
    p_->link->live = false;
    p_->link->failed = false;
  }
  if (!p_->ble.empty()) { ble::drop(p_->ble); p_->ble.clear(); }
}
bool MidiOut::failed() const {
  std::lock_guard<std::mutex> g(p_->mu);
  std::lock_guard<std::mutex> lg(p_->link->mu);
  return p_->link->failed;
}
void MidiOut::send(const unsigned char *b, int n) {
  std::lock_guard<std::mutex> g(p_->mu);
  if (n <= 0 || n > 64) return;
  if (!p_->ble.empty()) {
    unsigned char pk[72];
    const unsigned ms = unsigned(host_seconds(mach_absolute_time()) * 1000.0) & 8191;
    ble::send(p_->ble, pk, ble_midi_packet(b, n, ms, pk));
    return;
  }
  Byte buf[256];
  MIDIPacketList *list = (MIDIPacketList *)buf;
  MIDIPacket *pk = MIDIPacketListInit(list);
  pk = MIDIPacketListAdd(list, sizeof(buf), pk, 0, (ByteCount)n, b);
  if (!pk) return;
  if (p_->source) MIDIReceived(p_->source, list);
  else if (p_->port && p_->dest) MIDISend(p_->port, p_->dest, list);
}

}  // namespace rx
