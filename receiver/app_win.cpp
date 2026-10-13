// Windows window: painted with GDI like the plugin's. Pickers are popup menus; the latency field is an EDIT control.
#ifdef _WIN32
#include "app.h"

#include <commctrl.h>
#include <windowsx.h>

namespace {

constexpr int kW = 440, kTop = 134, kRow = 33, kRows = 4, kH = kTop + 6 + kRows * kRow;
constexpr UINT_PTR kTimerTick = 1, kTimerRepeat = 2;
constexpr int kIdLatency = 100;

COLORREF rgb(double r, double g, double b) { return RGB(int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5)); }
const COLORREF kGround = rgb(0.055, 0.062, 0.075), kBarBg = rgb(0.10, 0.11, 0.13), kField = rgb(0.13, 0.14, 0.165),
               kDim = rgb(0.60, 0.65, 0.71), kText = rgb(0.92, 0.94, 0.96), kOff = rgb(0.42, 0.46, 0.51),
               kRun = rgb(0.36, 1.0, 0.58), kWarn = rgb(1.0, 0.78, 0.30);

struct Gui {
  rx::App app;
  HWND hwnd = nullptr, lat = nullptr;
  HFONT fTc = nullptr, fUi = nullptr, fMono = nullptr;
  HBRUSH bField = nullptr;
  double scale = 1.0, latShown = 0.0;
  int repeatDir = 0;
  std::string ltcName, mtcName;
  int px(double v) const { return int(v * scale + 0.5); }
  RECT rc(double x, double y, double w, double h) const { return RECT{px(x), px(y), px(x + w), px(y + h)}; }
  double rowY(int i) const { return kTop + 6 + i * kRow; }
};

RECT source_rect(const Gui &g) { return g.rc(84, g.rowY(0), 344, 24); }
RECT rate_rect(const Gui &g) { return g.rc(84, g.rowY(1), 92, 24); }
RECT step_rect(const Gui &g, int dir) { return g.rc(338, g.rowY(1) + (dir > 0 ? 0 : 12), 18, 12); }
RECT ltc_rect(const Gui &g) { return g.rc(84, g.rowY(2), 256, 24); }
RECT chan_rect(const Gui &g) { return g.rc(348, g.rowY(2), 80, 24); }
RECT mtc_rect(const Gui &g) { return g.rc(84, g.rowY(3), 344, 24); }

void names(Gui &g) {
  g.ltcName = rx::App::nameOf(rx::audio_outputs(), g.app.ltcDev);
  g.mtcName = rx::App::nameOf(rx::midi_outputs(), g.app.mtcPort);
}

void show_latency(Gui &g) {
  g.latShown = g.app.set.latencyMs.load();
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f", g.latShown);
  SetWindowTextA(g.lat, b);
}
void commit_latency(Gui &g) {
  char b[32] = {0};
  GetWindowTextA(g.lat, b, sizeof(b));
  for (char *c = b; *c; c++) if (*c == ',') *c = '.';
  g.app.setLatency(std::atof(b));
  show_latency(g);
  SetFocus(g.hwnd);
}
void step_latency(Gui &g, int dir) {
  g.app.setLatency(g.app.set.latencyMs.load() + dir * (GetKeyState(VK_SHIFT) < 0 ? 10.0 : 1.0));
  show_latency(g);
}

// popup menu under a box; returns the picked index or -1
int pick(Gui &g, RECT r, const std::vector<std::string> &items, int checked) {
  HMENU m = CreatePopupMenu();
  for (size_t i = 0; i < items.size(); i++) AppendMenuA(m, MF_STRING | (int(i) == checked ? MF_CHECKED : 0), UINT_PTR(i + 1), items[i].c_str());
  POINT pt{r.left, r.bottom};
  ClientToScreen(g.hwnd, &pt);
  const int k = TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g.hwnd, nullptr);
  DestroyMenu(m);
  return k - 1;
}

void pick_source(Gui &g) {
  const auto src = g.app.listener.sources();
  const std::string cur = g.app.listener.choice();
  std::vector<std::string> items{R::automatic};
  int checked = cur.empty() ? 0 : -1;
  for (size_t i = 0; i < src.size(); i++) { items.push_back(rx::App::sourceLabel(src[i].ip)); if (src[i].ip == cur) checked = int(i) + 1; }
  if (checked < 0) { items.push_back(cur); checked = int(items.size()) - 1; }  // chosen, not sending now
  const int k = pick(g, source_rect(g), items, checked);
  if (k == 0) g.app.setSource("");
  else if (k > 0 && k <= int(src.size())) g.app.setSource(src[k - 1].ip);
}
void pick_device(Gui &g, bool midi) {
  const auto list = midi ? rx::midi_outputs() : rx::audio_outputs();
  const std::string cur = midi ? g.app.mtcPort : g.app.ltcDev;
  std::vector<std::string> items{R::off};
  int checked = cur.empty() ? 0 : -1;
  for (size_t i = 0; i < list.size(); i++) { items.push_back(list[i].name); if (list[i].id == cur) checked = int(i) + 1; }
  const int k = pick(g, midi ? mtc_rect(g) : ltc_rect(g), items, checked);
  if (k < 0) return;
  const std::string id = k == 0 ? "" : list[k - 1].id;
  if (midi) g.app.setMtc(id); else g.app.setLtc(id);
  names(g);
}
void pick_channel(Gui &g) {
  std::vector<std::string> items{R::allChannels};
  for (int c = 1; c <= g.app.audio.channels(); c++) items.push_back(R::channel(c));
  const int k = pick(g, chan_rect(g), items, g.app.ltcCh.load());
  if (k >= 0) g.app.setLtcChannel(k);
}

LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
  Gui &g = *(Gui *)ref;
  if (m == WM_KEYDOWN && w == VK_RETURN) { commit_latency(g); return 0; }
  if (m == WM_CHAR && (w == '\r' || w == '\n')) return 0;
  return DefSubclassProc(h, m, w, l);
}

void text(HDC dc, HFONT f, COLORREF c, const std::string &s, RECT r, UINT fmt) {
  SelectObject(dc, f);
  SetTextColor(dc, c);
  DrawTextA(dc, s.c_str(), -1, &r, fmt | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
}
void fill(HDC dc, RECT r, COLORREF c) {
  HBRUSH b = CreateSolidBrush(c);
  FillRect(dc, &r, b);
  DeleteObject(b);
}
void box(Gui &g, HDC dc, RECT r, const std::string &s, bool lit) {
  fill(dc, r, kField);
  r.left += g.px(6); r.right -= g.px(6);
  text(dc, g.fUi, lit ? kText : kDim, s, r, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}

void paint(Gui &g, HDC dc) {
  const rx::App::Ui u = g.app.ui();
  fill(dc, g.rc(0, 0, kW, kH), kGround);
  SetBkMode(dc, TRANSPARENT);
  text(dc, g.fTc, u.running ? kRun : kOff, u.tc, g.rc(0, 14, kW, 76), DT_CENTER | DT_VCENTER);
  text(dc, g.fUi, u.fault ? kWarn : kDim, u.line, g.rc(0, 94, kW, 20), DT_CENTER | DT_VCENTER);

  fill(dc, g.rc(0, kTop, kW, kH - kTop), kBarBg);
  const char *labels[kRows] = {R::source, R::rate, R::ltc, R::mtc};
  for (int i = 0; i < kRows; i++) text(dc, g.fUi, kDim, labels[i], g.rc(12, g.rowY(i) + 1, 70, 22), DT_LEFT | DT_VCENTER);

  const std::string choice = g.app.listener.choice();
  box(g, dc, source_rect(g), choice.empty() ? R::automatic : rx::App::sourceLabel(choice), !choice.empty());
  const int rsel = g.app.set.rate.load();
  box(g, dc, rate_rect(g), R::rateNames[rsel], rsel != 0);

  text(dc, g.fUi, kDim, R::latency, g.rc(200, g.rowY(1) + 1, 60, 22), DT_LEFT | DT_VCENTER);
  fill(dc, g.rc(262, g.rowY(1), 74, 24), kField);
  for (int dir : {1, -1}) {
    const RECT r = step_rect(g, dir);
    fill(dc, r, kField);
    const int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, a = g.px(3.5);
    POINT tri[3] = {{cx - a, cy + dir * a / 2}, {cx + a, cy + dir * a / 2}, {cx, cy - dir * a / 2 - dir}};
    HBRUSH b = CreateSolidBrush(kDim);
    HGDIOBJ oldB = SelectObject(dc, b), oldP = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, tri, 3);
    SelectObject(dc, oldB); SelectObject(dc, oldP);
    DeleteObject(b);
  }
  text(dc, g.fUi, kDim, R::ms, g.rc(362, g.rowY(1) + 1, 30, 22), DT_LEFT | DT_VCENTER);

  box(g, dc, ltc_rect(g), g.ltcName, !g.app.ltcDev.empty());
  const int ch = g.app.ltcCh.load();
  box(g, dc, chan_rect(g), ch ? R::channel(ch) : R::allChannels, ch != 0);
  box(g, dc, mtc_rect(g), g.mtcName, !g.app.mtcPort.empty());
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Gui *g = (Gui *)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (m == WM_NCCREATE) {
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
    return DefWindowProcW(h, m, w, l);
  }
  if (!g) return DefWindowProcW(h, m, w, l);
  switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      RECT c;
      GetClientRect(h, &c);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
      HGDIOBJ old = SelectObject(mem, bmp);
      paint(*g, mem);
      BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bmp);
      DeleteDC(mem);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_TIMER:
      if (w == kTimerRepeat) { if (g->repeatDir) step_latency(*g, g->repeatDir); return 0; }
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      SetFocus(h);
      auto in = [&](RECT r) { return PtInRect(&r, p) != 0; };
      if (in(source_rect(*g))) pick_source(*g);
      else if (in(rate_rect(*g))) {
        const int k = pick(*g, rate_rect(*g), {R::rateNames[0], R::rateNames[1], R::rateNames[2], R::rateNames[3], R::rateNames[4]}, g->app.set.rate.load());
        if (k >= 0) g->app.setRate(k);
      }
      else if (in(ltc_rect(*g))) pick_device(*g, false);
      else if (in(chan_rect(*g))) pick_channel(*g);
      else if (in(mtc_rect(*g))) pick_device(*g, true);
      for (int dir : {1, -1}) {
        if (!in(step_rect(*g, dir))) continue;
        step_latency(*g, dir);
        g->repeatDir = dir;
        SetCapture(h);
        SetTimer(h, kTimerRepeat, 120, nullptr);
      }
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
      if (g->repeatDir) { g->repeatDir = 0; KillTimer(h, kTimerRepeat); if (m == WM_LBUTTONUP) ReleaseCapture(); }
      return 0;
    case WM_COMMAND:
      if (LOWORD(w) == kIdLatency && HIWORD(w) == EN_KILLFOCUS) show_latency(*g);
      return 0;
    case WM_CTLCOLOREDIT:
      SetTextColor((HDC)w, kText);
      SetBkColor((HDC)w, kField);
      return (LRESULT)g->bField;
    case WM_DESTROY:
      KillTimer(h, kTimerTick);
      g->app.shutdown();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

HFONT font(const wchar_t *face, int px, int weight) {
  return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  timeBeginPeriod(1);
  static Gui g;
  g.scale = GetDpiForSystem() / 96.0;
  g.app.start();
  names(g);

  WNDCLASSW wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.lpszClassName = L"ArtNetTimecodeReceiver";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  RegisterClassW(&wc);

  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, g.px(kW), g.px(kH)};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, GetDpiForSystem());
  wchar_t title[128];
  MultiByteToWideChar(CP_UTF8, 0, R::appName, -1, title, 128);
  g.hwnd = CreateWindowExW(0, wc.lpszClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, &g);
  if (!g.hwnd) return 1;
  g.fTc = font(L"Consolas", g.px(64), FW_SEMIBOLD);
  g.fUi = font(L"Segoe UI", g.px(14), FW_NORMAL);
  g.fMono = font(L"Consolas", g.px(14), FW_NORMAL);
  g.bField = CreateSolidBrush(kField);
  g.lat = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_RIGHT, g.px(266), g.px(g.rowY(1) + 3), g.px(64), g.px(18), g.hwnd,
                          (HMENU)(INT_PTR)kIdLatency, inst, nullptr);
  SendMessageW(g.lat, WM_SETFONT, (WPARAM)g.fMono, TRUE);
  SetWindowSubclass(g.lat, edit_proc, 1, (DWORD_PTR)&g);
  show_latency(g);
  SetTimer(g.hwnd, kTimerTick, 33, nullptr);
  ShowWindow(g.hwnd, show);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
#endif
