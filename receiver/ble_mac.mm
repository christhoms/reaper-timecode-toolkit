// Bluetooth LE MIDI through CoreBluetooth in this process (the receiver app, the Bluetooth helper). This process
// connects the device and writes MIDI to its BLE MIDI characteristic itself, reconnecting whenever the device returns.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include <mach/mach_time.h>

#include <deque>
#include <map>
#include <mutex>

#include "ble.h"

namespace rx {
namespace {
double host_seconds(uint64_t t) {
  static const double k = [] { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return double(tb.numer) / double(tb.denom) * 1e-9; }();
  return double(t) * k;
}
}  // namespace
}  // namespace rx

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
Ble &bt() { static Ble *b = new Ble; return *b; }
void notify_changed() {
  std::function<void()> f;
  { std::lock_guard<std::mutex> g(bt().mu); f = bt().changed; }
  if (f) f();
}

// on q from here
void ble_start() {  // the first one asks for Bluetooth access
  Ble &b = bt();
  if (b.central) return;
  b.delegate = [RXBle new];
  b.central = [[CBCentralManager alloc] initWithDelegate:b.delegate queue:b.q];
}
void ble_connect_now(const std::string &uuid) {
  Ble &b = bt();
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
  Ble &b = bt();
  if (b.central.state != CBManagerStatePoweredOn) return;
  if (b.scan) [b.central scanForPeripheralsWithServices:@[ midi_service() ] options:nil];
  for (auto &l : b.links) if (!l.second.io) ble_connect_now(l.first);
}
void ble_seen(CBPeripheral *p, const std::string &name) {
  Ble &b = bt();
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
  Ble &b = bt();
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
  Ble &b = bt();
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
  dispatch_async(bt().q, ^{
    bt().links[uuid].state = state;
    ble_start();
    ble_powered();
  });
}
void ble_drop(std::string uuid) {
  dispatch_async(bt().q, ^{
    Ble &b = bt();
    auto it = b.links.find(uuid);
    if (it == b.links.end()) return;
    if (it->second.p) [b.central cancelPeripheralConnection:it->second.p];
    b.links.erase(it);
    { std::lock_guard<std::mutex> g(b.mu); b.held.erase(uuid); }
  });
}
void ble_send(std::string uuid, NSData *packet) {
  dispatch_async(bt().q, ^{
    auto it = bt().links.find(uuid);
    if (it == bt().links.end() || !it->second.io) return;
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
  [rx::bt().central cancelPeripheralConnection:p];  // not a MIDI device after all
}
- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)s error:(NSError *)error {
  for (CBCharacteristic *c in s.characteristics)
    if ([c.UUID isEqual:rx::midi_io()]) { rx::ble_up(p, c); return; }
  [rx::bt().central cancelPeripheralConnection:p];
}
- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral *)p {
  auto it = rx::bt().links.find(rx::uuid_of(p));
  if (it != rx::bt().links.end()) rx::ble_flush(it->second);
}
@end

namespace rx {
namespace ble {

void scan(bool on, std::function<void()> changed) {
  {
    std::lock_guard<std::mutex> g(rx::bt().mu);
    rx::bt().changed = on ? std::move(changed) : nullptr;
  }
  dispatch_async(rx::bt().q, ^{
    Ble &b = rx::bt();
    b.scan = on;
    if (on) ble_start();
    if (!b.central) return;
    if (on) ble_powered();
    else if (b.central.state == CBManagerStatePoweredOn) [b.central stopScan];
  });
}
void want(const std::string &uuid, std::function<void(bool)> state) { ble_want(uuid, std::move(state)); }
void drop(const std::string &uuid) { ble_drop(uuid); }
void send(const std::string &uuid, const unsigned char *packet, int n) { ble_send(uuid, [NSData dataWithBytes:packet length:(NSUInteger)n]); }
std::vector<Device> found() {
  std::vector<Device> out;
  std::lock_guard<std::mutex> g(rx::bt().mu);
  const double now = host_seconds(mach_absolute_time());
  for (auto &f : rx::bt().found) if (now - f.second.seen < 10.0) out.push_back({f.first, f.second.name});
  return out;
}
void shutdown() {}
std::vector<Device> held() {
  std::vector<Device> out;
  std::lock_guard<std::mutex> g(rx::bt().mu);
  for (auto &h : rx::bt().held) out.push_back({h.first, h.second});
  return out;
}

}  // namespace ble
}  // namespace rx
