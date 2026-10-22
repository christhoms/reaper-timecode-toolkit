// Bluetooth LE MIDI devices for MTC (macOS). ble_mac.mm drives CoreBluetooth in this process. ble_client_mac.mm asks the
// Bluetooth helper (an app with Bluetooth access of its own) instead, for a host process that has none: a DAW running the
// plugin. Callbacks run on any thread.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "io.h"

namespace rx {
namespace ble {

void scan(bool on, std::function<void()> changed);                   // changed: a device appeared, or one connected
void want(const std::string &uuid, std::function<void(bool)> state);  // connect, and again whenever the device returns
void drop(const std::string &uuid);
void send(const std::string &uuid, const unsigned char *packet, int n);  // one BLE MIDI packet
std::vector<Device> found();  // advertising now
std::vector<Device> held();   // connected here (or by the helper)
void shutdown();              // before the code is unloaded (the plugin); nothing to do in ble_mac.mm

}  // namespace ble
}  // namespace rx
