// macOS outputs: AUHAL for LTC, Core MIDI for MTC (any destination, a source this app creates, or a Bluetooth device).
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreBluetooth/CoreBluetooth.h>
#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "core.h"
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
constexpr const char *kBle = "ble:";  // + the peripheral's UUID: a Bluetooth MIDI device, connected or not
MIDIClientRef client() {
  static MIDIClientRef c = [] { MIDIClientRef r = 0; MIDIClientCreate(CFSTR("Art-Net Timecode Receiver"), nullptr, nullptr, &r); return r; }();
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
}  // namespace
}  // namespace rx

// Bluetooth LE MIDI. A device CoreMIDI has online goes through CoreMIDI. Any other this app connects with CoreBluetooth
// and writes MIDI to the device's BLE MIDI characteristic itself, reconnecting whenever the device comes back.
@interface RXBle : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@end

namespace rx {
namespace {
CBUUID *midi_service() { return [CBUUID UUIDWithString:@"03B80E5A-EDE8-4B33-A751-6CE34EC4C700"]; }
CBUUID *midi_io() { return [CBUUID UUIDWithString:@"7772E5DB-3868-4112-A1A9-F2669D106BF3"]; }
std::string uuid_of(CBPeripheral *p) { return p.identifier.UUIDString.UTF8String; }

struct Found { std::string name; double seen; };
struct BleLink {  // a device this app holds, or wants to
  CBPeripheral *p = nil;
  CBCharacteristic *io = nil;       // set once connected and its MIDI characteristic found
  std::deque<NSData *> queue;       // packets waiting for the radio
  std::function<void(bool)> state;  // true on each connection, false on a failure or disconnect
};
struct Ble {
  std::mutex mu;  // found, held, changed
  std::map<std::string, Found> found;        // advertising, by UUID
  std::map<std::string, std::string> held;   // connected by this app: UUID, name
  std::function<void()> changed;
  // the rest on q only
  dispatch_queue_t q = dispatch_queue_create("uk.co.christhoms.artnet-timecode-receiver.ble", DISPATCH_QUEUE_SERIAL);
  CBCentralManager *central = nil;
  RXBle *delegate = nil;
  bool scan = false;
  std::map<std::string, CBPeripheral *> peripherals;  // seen, by UUID
  std::map<std::string, BleLink> links;               // by UUID
};
Ble &ble() { static Ble *b = new Ble; return *b; }
void notify_changed() {
  std::function<void()> f;
  { std::lock_guard<std::mutex> g(ble().mu); f = ble().changed; }
  if (f) f();
}

// on q from here
void ble_start() {  // the first one asks for Bluetooth access
  Ble &b = ble();
  if (b.central) return;
  b.delegate = [RXBle new];
  b.central = [[CBCentralManager alloc] initWithDelegate:b.delegate queue:b.q];
}
void ble_connect_now(const std::string &uuid) {
  Ble &b = ble();
  auto it = b.links.find(uuid);
  if (it == b.links.end() || b.central.state != CBManagerStatePoweredOn) return;
  CBPeripheral *p = it->second.p ?: (b.peripherals.count(uuid) ? b.peripherals[uuid] : nil);
  if (!p) {
    NSUUID *u = [[NSUUID alloc] initWithUUIDString:[NSString stringWithUTF8String:uuid.c_str()]];
    if (u) p = [b.central retrievePeripheralsWithIdentifiers:@[ u ]].firstObject;
  }
  if (!p) return;  // unknown to this Mac until a scan sees it
  it->second.p = p;
  p.delegate = b.delegate;
  if (p.state == CBPeripheralStateDisconnected) [b.central connectPeripheral:p options:nil];  // no time limit: waits for the device
}
void ble_powered() {
  Ble &b = ble();
  if (b.central.state != CBManagerStatePoweredOn) return;
  if (b.scan) [b.central scanForPeripheralsWithServices:@[ midi_service() ] options:nil];
  for (auto &l : b.links) if (!l.second.io) ble_connect_now(l.first);
}
void ble_seen(CBPeripheral *p, const std::string &name) {
  Ble &b = ble();
  const std::string uuid = uuid_of(p);
  b.peripherals[uuid] = p;
  bool fresh;
  {
    std::lock_guard<std::mutex> g(b.mu);
    fresh = !b.found.count(uuid);
    b.found[uuid] = {name, host_seconds(mach_absolute_time())};
  }
  if (b.links.count(uuid)) ble_connect_now(uuid);
  if (fresh) notify_changed();
}
void ble_up(CBPeripheral *p, CBCharacteristic *io) {
  Ble &b = ble();
  auto it = b.links.find(uuid_of(p));
  if (it == b.links.end()) { [b.central cancelPeripheralConnection:p]; return; }
  it->second.io = io;
  {
    std::lock_guard<std::mutex> g(b.mu);
    b.held[it->first] = p.name ? p.name.UTF8String : it->first;
    b.found.erase(it->first);
  }
  if (it->second.state) it->second.state(true);
  notify_changed();
}
void ble_down(CBPeripheral *p) {  // failed or lost: try again, the connection waits for the device
  Ble &b = ble();
  auto it = b.links.find(uuid_of(p));
  if (it == b.links.end()) return;
  it->second.io = nil;
  it->second.queue.clear();
  { std::lock_guard<std::mutex> g(b.mu); b.held.erase(it->first); }
  if (it->second.state) it->second.state(false);
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), b.q, ^{ ble_connect_now(uuid_of(p)); });
}
void ble_flush(BleLink &l) {
  while (!l.queue.empty() && l.io && l.p.canSendWriteWithoutResponse) {
    [l.p writeValue:l.queue.front() forCharacteristic:l.io type:CBCharacteristicWriteWithoutResponse];
    l.queue.pop_front();
  }
}
// off q
void ble_want(std::string uuid, std::function<void(bool)> state) {  // by value: blocks capture a reference as a reference
  dispatch_async(ble().q, ^{
    ble().links[uuid].state = state;
    ble_start();
    ble_powered();
  });
}
void ble_drop(std::string uuid) {
  dispatch_async(ble().q, ^{
    Ble &b = ble();
    auto it = b.links.find(uuid);
    if (it == b.links.end()) return;
    if (it->second.p) [b.central cancelPeripheralConnection:it->second.p];
    b.links.erase(it);
    { std::lock_guard<std::mutex> g(b.mu); b.held.erase(uuid); }
  });
}
void ble_send(std::string uuid, NSData *packet) {
  dispatch_async(ble().q, ^{
    auto it = ble().links.find(uuid);
    if (it == ble().links.end() || !it->second.io) return;
    BleLink &l = it->second;
    if (l.queue.size() >= 64) l.queue.pop_front();  // the radio fell behind: the oldest goes
    l.queue.push_back(packet);
    ble_flush(l);
  });
}
}  // namespace
}  // namespace rx

@implementation RXBle
- (void)centralManagerDidUpdateState:(CBCentralManager *)central { rx::ble_powered(); }
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)p advertisementData:(NSDictionary<NSString *, id> *)ad RSSI:(NSNumber *)rssi {
  NSString *name = ad[CBAdvertisementDataLocalNameKey] ?: p.name;
  if (name.length) rx::ble_seen(p, name.UTF8String);
}
- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)p { [p discoverServices:@[ rx::midi_service() ]]; }
- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)error { rx::ble_down(p); }
- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)error { rx::ble_down(p); }
- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)error {
  for (CBService *s in p.services)
    if ([s.UUID isEqual:rx::midi_service()]) { [p discoverCharacteristics:@[ rx::midi_io() ] forService:s]; return; }
  [rx::ble().central cancelPeripheralConnection:p];  // not a MIDI device after all
}
- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)s error:(NSError *)error {
  for (CBCharacteristic *c in s.characteristics)
    if ([c.UUID isEqual:rx::midi_io()]) { rx::ble_up(p, c); return; }
  [rx::ble().central cancelPeripheralConnection:p];
}
- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral *)p {
  auto it = rx::ble().links.find(rx::uuid_of(p));
  if (it != rx::ble().links.end()) rx::ble_flush(it->second);
}
@end

namespace rx {

void bluetooth_scan(bool on, std::function<void()> changed) {
  {
    std::lock_guard<std::mutex> g(ble().mu);
    ble().changed = on ? std::move(changed) : nullptr;
  }
  dispatch_async(ble().q, ^{
    Ble &b = ble();
    b.scan = on;
    if (on) ble_start();
    if (!b.central) return;
    if (on) ble_powered();
    else if (b.central.state == CBManagerStatePoweredOn) [b.central stopScan];
  });
}

std::vector<Device> midi_outputs() {
  std::vector<Device> out{{kVirtual, R::virtualPort}};
  client();
  std::set<std::string> have;  // Bluetooth UUIDs listed
  const ItemCount n = MIDIGetNumberOfDestinations();
  for (ItemCount i = 0; i < n; i++) {
    MIDIEndpointRef e = MIDIGetDestination(i);
    SInt32 uid = 0;
    CFStringRef name = nullptr;
    if (!e || MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &uid) != noErr) continue;
    MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name);
    const std::string u = ble_uuid(device_of(e));
    if (!u.empty()) have.insert(u);
    out.push_back({u.empty() ? std::to_string(uid) : kBle + u, str(name)});
    if (name) CFRelease(name);
  }
  std::lock_guard<std::mutex> g(ble().mu);
  for (auto &h : ble().held)  // connected by this app
    if (have.insert(h.first).second) out.push_back({kBle + h.first, h.second});
  const ItemCount nd = MIDIGetNumberOfDevices();  // connected to this Mac before, offline now
  for (ItemCount i = 0; i < nd; i++) {
    MIDIDeviceRef d = MIDIGetDevice(i);
    const std::string u = ble_uuid(d);
    if (u.empty() || have.count(u) || !remote_peripheral(d) || !iprop(d, kMIDIPropertyOffline)) continue;
    have.insert(u);
    out.push_back({kBle + u, R::bluetooth(sprop(d, kMIDIPropertyName))});
  }
  const double now = host_seconds(mach_absolute_time());  // advertising now
  for (auto &f : ble().found)
    if (now - f.second.seen < 10.0 && have.insert(f.first).second) out.push_back({kBle + f.first, R::bluetooth(f.second.name)});
  return out;
}

namespace {
struct Link {  // a Bluetooth device's state, told from the Bluetooth queue
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
    CFStringRef name = CFStringCreateWithCString(nullptr, R::virtualPort, kCFStringEncodingUTF8);
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
    ble_want(uuid, [link](bool up) {
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
  if (!p_->ble.empty()) { ble_drop(p_->ble); p_->ble.clear(); }
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
    ble_send(p_->ble, [NSData dataWithBytes:pk length:(NSUInteger)ble_midi_packet(b, n, ms, pk)]);
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
