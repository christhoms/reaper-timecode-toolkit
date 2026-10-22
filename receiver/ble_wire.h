// The Bluetooth helper's socket (macOS): a Unix stream socket in the user's temporary folder. Each message is a 2 byte
// little-endian length (of what follows), a type byte and its payload. UUIDs are the 36 character CoreBluetooth form.
//   to the helper    'S' on (1 byte)  scan or stop       'W' uuid  connect and keep connected     'D' uuid  let go
//                    'M' uuid packet  one BLE MIDI packet
//   to the plugin    'L' records      devices: 'F' (advertising) or 'H' (connected), uuid, name, 0
//                    'U' uuid up      a wanted device connected (1) or not (0)
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace rx {
namespace wire {

constexpr size_t kUuid = 36;

inline std::string socket_path() {
  char d[PATH_MAX] = {0};
  if (!confstr(_CS_DARWIN_USER_TEMP_DIR, d, sizeof(d))) std::strcpy(d, "/tmp/");
  return std::string(d) + "uk.co.christhoms.rtt-bluetooth.sock";
}
inline bool address(sockaddr_un &a) {
  const std::string p = socket_path();
  std::memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  if (p.size() >= sizeof(a.sun_path)) return false;
  std::memcpy(a.sun_path, p.c_str(), p.size() + 1);
  return true;
}
inline std::vector<unsigned char> message(char type, const std::string &payload) {
  const size_t n = payload.size() + 1;
  std::vector<unsigned char> m{(unsigned char)(n & 0xff), (unsigned char)(n >> 8), (unsigned char)type};
  m.insert(m.end(), payload.begin(), payload.end());
  return m;
}
inline bool write_all(int fd, const std::vector<unsigned char> &m) {
  size_t k = 0;
  while (k < m.size()) {
    const ssize_t w = ::write(fd, m.data() + k, m.size() - k);
    if (w <= 0) return false;
    k += size_t(w);
  }
  return true;
}
// whole messages from a stream: appends what arrived to buf, hands each complete one to f(type, payload)
template <class F> void parse(std::string &buf, F f) {
  size_t k = 0;
  while (buf.size() - k >= 3) {
    const size_t n = (unsigned char)buf[k] | (size_t((unsigned char)buf[k + 1]) << 8);
    if (n == 0) { k = buf.size(); break; }  // malformed: drop the rest
    if (buf.size() - k - 2 < n) break;
    f(buf[k + 2], buf.substr(k + 3, n - 1));
    k += 2 + n;
  }
  buf.erase(0, k);
}

}  // namespace wire
}  // namespace rx
