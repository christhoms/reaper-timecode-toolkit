// Bluetooth LE MIDI through the Bluetooth helper (helper/bluetooth_helper.mm, ble_wire.h), for a host process without
// Bluetooth access: the plugin. A connection thread starts on first use, starts the helper when it is not running
// (LaunchServices, so the helper answers for its own Bluetooth access) and tells it again what is wanted after a restart.
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#include "ble.h"
#include "ble_wire.h"

namespace rx {
namespace {

struct Client {
  std::mutex mu;  // all below
  int fd = -1;
  bool scan = false;
  std::function<void()> changed;
  std::map<std::string, std::function<void(bool)>> wants;
  std::vector<Device> found, held;
  std::chrono::steady_clock::time_point lastLaunch{};
  std::thread th;
  std::atomic<bool> stop{false};
};
Client &cl() { static Client *c = new Client; return *c; }

std::string helper_path() {  // Contents/Helpers/RTTBluetooth.app in the bundle this binary sits in
  Dl_info i;
  if (!dladdr((const void *)&helper_path, &i) || !i.dli_fname) return "";
  const std::string p = i.dli_fname;
  const size_t k = p.rfind("/MacOS/");
  return k == std::string::npos ? "" : p.substr(0, k) + "/Helpers/RTTBluetooth.app";
}
void launch_helper() {
  NSString *path = [NSString stringWithUTF8String:helper_path().c_str()];
  if (!path.length) return;
  NSURL *url = [NSURL fileURLWithPath:path];
  dispatch_async(dispatch_get_main_queue(), ^{
    NSWorkspaceOpenConfiguration *c = [NSWorkspaceOpenConfiguration configuration];
    c.activates = NO;
    c.addsToRecentItems = NO;
    c.promptsUserIfNeeded = NO;
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:url configuration:c completionHandler:nil];
  });
}
void tell(Client &c, char type, const std::string &payload, bool mayDrop = false) {  // c.mu held
  if (c.fd < 0) return;
  const std::vector<unsigned char> m = wire::message(type, payload);
  if (mayDrop) send(c.fd, m.data(), m.size(), MSG_DONTWAIT);  // MTC: never wait on a stalled helper
  else wire::write_all(c.fd, m);
}
void handle(char type, const std::string &p) {
  Client &c = cl();
  if (type == 'L') {
    std::vector<Device> f, h;
    size_t k = 0;
    while (k + 1 + wire::kUuid <= p.size()) {
      const char kind = p[k];
      const std::string uuid = p.substr(k + 1, wire::kUuid);
      const size_t end = p.find('\0', k + 1 + wire::kUuid);
      if (end == std::string::npos) break;
      (kind == 'H' ? h : f).push_back({uuid, p.substr(k + 1 + wire::kUuid, end - k - 1 - wire::kUuid)});
      k = end + 1;
    }
    std::function<void()> changed;
    {
      std::lock_guard<std::mutex> g(c.mu);
      c.found = f;
      c.held = h;
      changed = c.changed;
    }
    if (changed) changed();
  } else if (type == 'U' && p.size() == wire::kUuid + 1) {
    std::function<void(bool)> state;
    {
      std::lock_guard<std::mutex> g(c.mu);
      auto it = c.wants.find(p.substr(0, wire::kUuid));
      if (it != c.wants.end()) state = it->second;
    }
    if (state) state(p[wire::kUuid] != 0);
  }
}
void run() {
  Client &c = cl();
  while (!c.stop) {
    sockaddr_un a;
    if (!wire::address(a)) return;
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (sockaddr *)&a, sizeof(a)) != 0) {
      if (fd >= 0) close(fd);
      {
        std::lock_guard<std::mutex> g(c.mu);
        const auto now = std::chrono::steady_clock::now();
        if (now - c.lastLaunch > std::chrono::seconds(5)) { c.lastLaunch = now; launch_helper(); }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      continue;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    {
      std::lock_guard<std::mutex> g(c.mu);
      c.fd = fd;
      if (c.scan) tell(c, 'S', std::string(1, '\1'));
      for (auto &w : c.wants) tell(c, 'W', w.first);
    }
    std::string buf;
    char b[4096];
    for (;;) {
      const ssize_t n = read(fd, b, sizeof(b));
      if (n <= 0) break;
      buf.append(b, size_t(n));
      wire::parse(buf, handle);
    }
    std::vector<std::function<void(bool)>> down;
    {
      std::lock_guard<std::mutex> g(c.mu);
      c.fd = -1;
      c.found.clear();
      c.held.clear();
      for (auto &w : c.wants) down.push_back(w.second);
    }
    close(fd);
    for (auto &f : down) f(false);
  }
}
void start() {  // c.mu held
  Client &c = cl();
  if (!c.th.joinable()) c.th = std::thread(run);
}

}  // namespace

namespace ble {

void scan(bool on, std::function<void()> changed) {
  Client &c = cl();
  std::lock_guard<std::mutex> g(c.mu);
  c.scan = on;
  c.changed = on ? std::move(changed) : nullptr;
  if (on) start();
  tell(c, 'S', std::string(1, on ? '\1' : '\0'));
}
void want(const std::string &uuid, std::function<void(bool)> state) {
  Client &c = cl();
  std::lock_guard<std::mutex> g(c.mu);
  c.wants[uuid] = std::move(state);
  start();
  tell(c, 'W', uuid);
}
void drop(const std::string &uuid) {
  Client &c = cl();
  std::lock_guard<std::mutex> g(c.mu);
  c.wants.erase(uuid);
  tell(c, 'D', uuid);
}
void send(const std::string &uuid, const unsigned char *packet, int n) {
  Client &c = cl();
  std::lock_guard<std::mutex> g(c.mu);
  tell(c, 'M', uuid + std::string((const char *)packet, size_t(n)), true);
}
std::vector<Device> found() { std::lock_guard<std::mutex> g(cl().mu); return cl().found; }
std::vector<Device> held() { std::lock_guard<std::mutex> g(cl().mu); return cl().held; }
void shutdown() {  // the host unloads the plugin: the thread must not outlive the code
  Client &c = cl();
  c.stop = true;
  {
    std::lock_guard<std::mutex> g(c.mu);
    if (c.fd >= 0) ::shutdown(c.fd, SHUT_RDWR);
  }
  if (c.th.joinable()) c.th.join();
  c.stop = false;  // a host may initialise the plugin again without unloading it
}

}  // namespace ble
}  // namespace rx
