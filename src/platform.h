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
#else
#include <arpa/inet.h>
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
#include <string>

namespace ctltc {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kNoSocket = INVALID_SOCKET;
inline void close_socket(sock_t s) { closesocket(s); }
inline void net_init() { static const int once = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d); }(); (void)once; }

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
