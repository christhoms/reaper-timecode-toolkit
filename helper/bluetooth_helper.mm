// The plugin's Bluetooth helper: an app with Bluetooth access of its own, so a DAW that has none (REAPER's Info.plist
// carries no Bluetooth usage string; CoreBluetooth there would end the process) can still reach Bluetooth MIDI devices.
// The plugin starts it through LaunchServices and talks to it over ble_wire.h. One helper serves every plugin instance;
// it quits 30 s after the last one leaves. All state lives on the main queue.
#import <Foundation/Foundation.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <map>
#include <set>

#include "ble.h"
#include "ble_wire.h"

using namespace rx;

namespace {

struct Client {
  int fd = -1;
  dispatch_source_t src = nil;
  std::string buf;
  bool scan = false;
  std::set<std::string> wants;
};
std::set<Client *> clients;
std::map<std::string, std::set<Client *>> wanters;  // by UUID
std::map<std::string, bool> upState;                // last state told, by UUID
std::string lastList;
double idleSince = 0;

double now() { return [NSDate timeIntervalSinceReferenceDate]; }
void tell(Client *c, char type, const std::string &payload) { wire::write_all(c->fd, wire::message(type, payload)); }

std::string list_payload() {
  std::string p;
  for (const Device &d : ble::found()) p += 'F' + d.id + d.name + '\0';
  for (const Device &d : ble::held()) p += 'H' + d.id + d.name + '\0';
  return p;
}
void send_lists(bool force) {
  const std::string p = list_payload();
  if (!force && p == lastList) return;
  lastList = p;
  for (Client *c : clients) tell(c, 'L', p);
}
void update_scan() {
  bool any = false;
  for (Client *c : clients) any = any || c->scan;
  ble::scan(any, [] { dispatch_async(dispatch_get_main_queue(), ^{ send_lists(false); }); });
}
void want(Client *c, const std::string &uuid) {
  if (!c->wants.insert(uuid).second) return;
  auto &w = wanters[uuid];
  w.insert(c);
  if (w.size() > 1) {  // already asked for: tell this one where it stands
    auto s = upState.find(uuid);
    if (s != upState.end()) tell(c, 'U', uuid + char(s->second ? 1 : 0));
    return;
  }
  ble::want(uuid, [uuid](bool up) {
    dispatch_async(dispatch_get_main_queue(), ^{
      upState[uuid] = up;
      for (Client *k : wanters[uuid]) tell(k, 'U', uuid + char(up ? 1 : 0));
      send_lists(false);
    });
  });
}
void unwant(Client *c, const std::string &uuid) {
  if (!c->wants.erase(uuid)) return;
  auto &w = wanters[uuid];
  w.erase(c);
  if (!w.empty()) return;
  wanters.erase(uuid);
  upState.erase(uuid);
  ble::drop(uuid);
}
void drop_client(Client *c) {
  for (const std::string &u : std::set<std::string>(c->wants)) unwant(c, u);
  dispatch_source_cancel(c->src);
  clients.erase(c);
  update_scan();
  if (clients.empty()) idleSince = now();
}
void handle(Client *c, char type, const std::string &p) {
  switch (type) {
    case 'S': c->scan = !p.empty() && p[0]; update_scan(); send_lists(true); break;
    case 'W': if (p.size() == wire::kUuid) want(c, p); break;
    case 'D': if (p.size() == wire::kUuid) unwant(c, p); break;
    case 'M':
      if (p.size() > wire::kUuid) ble::send(p.substr(0, wire::kUuid), (const unsigned char *)p.data() + wire::kUuid, int(p.size() - wire::kUuid));
      break;
  }
}
void accept_client(int listener) {
  const int fd = accept(listener, nullptr, nullptr);
  if (fd < 0) return;
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
  Client *c = new Client;
  c->fd = fd;
  c->src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
  dispatch_source_set_event_handler(c->src, ^{
    char b[4096];
    const ssize_t n = read(c->fd, b, sizeof(b));
    if (n <= 0) { drop_client(c); return; }
    c->buf.append(b, size_t(n));
    wire::parse(c->buf, [c](char t, const std::string &p) { handle(c, t, p); });
  });
  dispatch_source_set_cancel_handler(c->src, ^{ close(c->fd); delete c; });
  clients.insert(c);
  dispatch_resume(c->src);
  tell(c, 'L', list_payload());
}

}  // namespace

int main() {
  @autoreleasepool {
    sockaddr_un a;
    if (!wire::address(a)) return 1;
    const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(probe, (sockaddr *)&a, sizeof(a)) == 0) return 0;  // another helper already serves
    close(probe);
    unlink(a.sun_path);
    const int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, (sockaddr *)&a, sizeof(a)) != 0 || listen(listener, 16) != 0) return 1;
    chmod(a.sun_path, 0600);
    dispatch_source_t acc = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)listener, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(acc, ^{ accept_client(listener); });
    dispatch_resume(acc);
    idleSince = now();
    dispatch_source_t tick = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(tick, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 10);
    dispatch_source_set_event_handler(tick, ^{
      send_lists(false);  // advertising devices age out, connections come and go
      if (clients.empty() && now() - idleSince > 30) { unlink(a.sun_path); exit(0); }
    });
    dispatch_resume(tick);
    dispatch_main();
  }
}
