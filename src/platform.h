// Clock, real-time thread, UDP socket and preferences folder for macOS and Windows.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <direct.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ctltc {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kNoSocket = INVALID_SOCKET;
inline void close_socket(sock_t s) { closesocket(s); }
inline void net_init() { static const int once = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d); }(); (void)once; }

// IPv4 interfaces that are up, for Broadcast mode: name, address, directed broadcast address
struct NetIf { std::string name, addr, bcast; };
inline std::vector<NetIf> list_interfaces() {
  std::vector<NetIf> out;
  ULONG size = 16384;
  std::vector<unsigned char> buf(size);
  const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
  ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
  if (r == ERROR_BUFFER_OVERFLOW) { buf.resize(size); r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size); }
  if (r != NO_ERROR) return out;
  for (auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
      if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const uint32_t ip = ntohl(((sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
      const uint32_t mask = u->OnLinkPrefixLength >= 32 ? 0xffffffffu : ~(0xffffffffu >> u->OnLinkPrefixLength);
      char name[256] = {0};
      WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name) - 1, nullptr, nullptr);
      NetIf n; n.name = name;
      char s[32];
      in_addr x; x.s_addr = htonl(ip); inet_ntop(AF_INET, &x, s, sizeof(s)); n.addr = s;
      x.s_addr = htonl(ip | ~mask); inet_ntop(AF_INET, &x, s, sizeof(s)); n.bcast = s;
      out.push_back(n);
    }
  }
  return out;
}

inline double now_s() {
  static const double k = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1.0 / double(f.QuadPart); }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) * k;
}
// high-resolution waitable timer (Windows 10 1803+), else a 1 ms Sleep
inline void wait_until_s(double t) {
  static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  const double left = t - now_s();
  if (left <= 0) return;
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -LONGLONG(left * 1e7);
    if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
  }
  Sleep(DWORD(left * 1e3));
}
inline void make_thread_realtime(double) {
  timeBeginPeriod(1);
  DWORD task = 0;
  AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}
inline std::string prefs_path() {
  const char *base = std::getenv("APPDATA");
  return std::string(base ? base : ".") + "\\CT LTC ArtNet";
}
inline void make_dir(const std::string &p) { _mkdir(p.c_str()); }
constexpr const char *kSep = "\\";
#else
using sock_t = int;
constexpr sock_t kNoSocket = -1;
inline void close_socket(sock_t s) { close(s); }
inline void net_init() {}

struct NetIf { std::string name, addr, bcast; };
inline std::vector<NetIf> list_interfaces() {
  std::vector<NetIf> out;
  ifaddrs *list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  for (ifaddrs *i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK) || !i->ifa_netmask) continue;
    const uint32_t ip = ntohl(((sockaddr_in *)i->ifa_addr)->sin_addr.s_addr), mask = ntohl(((sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr);
    NetIf n; n.name = i->ifa_name;
    char s[32];
    in_addr x; x.s_addr = htonl(ip); inet_ntop(AF_INET, &x, s, sizeof(s)); n.addr = s;
    x.s_addr = htonl(ip | ~mask); inet_ntop(AF_INET, &x, s, sizeof(s)); n.bcast = s;
    out.push_back(n);
  }
  freeifaddrs(list);
  return out;
}

// mach_absolute_time for both threads, so mach_wait_until hits the deadline
inline double mach_scale() {
  static const double k = [] { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return double(tb.numer) / double(tb.denom) * 1e-9; }();
  return k;
}
inline double now_s() { return double(mach_absolute_time()) * mach_scale(); }
inline void wait_until_s(double t) { mach_wait_until(uint64_t(t / mach_scale())); }
// time-constraint class: normal threads' sleeps are coalesced and land several ms late
inline void make_thread_realtime(double period_s) {
  const double k = 1.0 / mach_scale();
  thread_time_constraint_policy_data_t pol;
  pol.period = uint32_t(period_s * k);
  pol.computation = uint32_t(0.0003 * k);
  pol.constraint = uint32_t(0.001 * k);
  pol.preemptible = 1;
  thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}
inline std::string prefs_path() {
  const char *home = std::getenv("HOME");
  return std::string(home ? home : "/tmp") + "/Library/Application Support/CT LTC ArtNet";
}
inline void make_dir(const std::string &p) { mkdir(p.c_str(), 0755); }
constexpr const char *kSep = "/";
#endif

}  // namespace ctltc
