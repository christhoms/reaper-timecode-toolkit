// Windows GUI: one child window, painted with GDI. The two text fields are EDIT controls; the offset stepper and
// the source selector are painted and hit-tested here so they match the dark surface.
#ifdef _WIN32
#include "plugin.h"

#include <commctrl.h>
#include <windowsx.h>

namespace {

constexpr UINT_PTR kTimerTick = 1, kTimerRepeat = 2;
constexpr int kIdIp = 100, kIdOffset = 101;

COLORREF rgb(double r, double g, double b) { return RGB(int(r * 255 + 0.5), int(g * 255 + 0.5), int(b * 255 + 0.5)); }
const COLORREF kGround = rgb(0.055, 0.062, 0.075), kBarBg = rgb(0.10, 0.11, 0.13), kField = rgb(0.13, 0.14, 0.165),
               kDim = rgb(0.60, 0.65, 0.71), kText = rgb(0.92, 0.94, 0.96), kSel = rgb(0.30, 0.32, 0.36), kRed = rgb(1.0, 0.42, 0.40);

COLORREF ui_color(UiColor c) {
  switch (c) {
    case kUiLtc: return rgb(0.36, 1.0, 0.58);
    case kUiDaw: return rgb(0.45, 0.78, 1.0);
    case kUiCoast: case kUiWarn: return rgb(1.0, 0.78, 0.30);
    case kUiOff: return rgb(0.42, 0.46, 0.51);
    default: return kDim;
  }
}

struct Gui {
  Plugin *plug = nullptr;
  HWND hwnd = nullptr, ip = nullptr, off = nullptr;
  HFONT fTc = nullptr, fUi = nullptr, fMono = nullptr;
  HBRUSH bField = nullptr;
  double scale = 1.0, offShown = 0.0;
  bool ipBad = false;
  int ticks = 0, repeatDir = 0;
  int px(double v) const { return int(v * scale + 0.5); }
  RECT rc(double x, double y, double w, double h) const { return RECT{px(x), px(y), px(x + w), px(y + h)}; }
};

const double kBarY = kGuiH - kGuiBar;
RECT seg_rect(const Gui &g, int i) { return g.rc(84 + i * 74, kBarY + 72, 73, 24); }
RECT step_rect(const Gui &g, int dir) { return g.rc(160, kBarY + (dir > 0 ? 39 : 51), 18, 12); }

HINSTANCE module() {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&module, &m);
  return m;
}

void show_offset(Gui &g, double ms) {
  g.offShown = ms;
  char b[32];
  std::snprintf(b, sizeof(b), "%.1f", ms);
  SetWindowTextA(g.off, b);
}

void commit_ip(Gui &g) {
  char b[64] = {0};
  GetWindowTextA(g.ip, b, sizeof(b));
  std::string s = b;
  while (!s.empty() && s.back() == ' ') s.pop_back();
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  g.ipBad = !g.plug->setIP(s);
  if (!g.ipBad) { SetWindowTextA(g.ip, s.c_str()); SetFocus(g.hwnd); }
  InvalidateRect(g.ip, nullptr, TRUE);
}

void commit_offset(Gui &g) {
  char b[32] = {0};
  GetWindowTextA(g.off, b, sizeof(b));
  for (char *c = b; *c; c++) if (*c == ',') *c = '.';
  g.plug->setOffsetFromGui(ctltc::clamp_offset(std::atof(b)));
  show_offset(g, g.plug->sender.offsetMs());
  SetFocus(g.hwnd);
}

void step_offset(Gui &g, int dir) {
  const bool frame = GetKeyState(VK_SHIFT) < 0;  // Shift: one frame
  const double step = frame ? g.plug->frameDur.load() * 1e3 : 1.0;
  g.plug->setOffsetFromGui(ctltc::clamp_offset(g.offShown + dir * step));
  show_offset(g, g.plug->sender.offsetMs());
}

LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
  Gui &g = *(Gui *)ref;
  if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DefSubclassProc(h, m, w, l);
  if (m == WM_KEYDOWN && w == VK_RETURN) { h == g.ip ? commit_ip(g) : commit_offset(g); return 0; }
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

void paint(Gui &g, HDC dc) {
  const UiState u = ui_state(*g.plug);
  fill(dc, g.rc(0, 0, kGuiW, kGuiH), kGround);
  SetBkMode(dc, TRANSPARENT);
  text(dc, g.fTc, ui_color(u.tcColor), u.tc, g.rc(0, 14, kGuiW, 76), DT_CENTER | DT_VCENTER);
  text(dc, g.fUi, kDim, u.line, g.rc(0, 94, kGuiW, 20), DT_CENTER | DT_VCENTER);

  fill(dc, g.rc(0, kBarY, kGuiW, kGuiBar), kBarBg);
  text(dc, g.fUi, kDim, "Art-Net to", g.rc(12, kBarY + 7, 70, 22), DT_LEFT | DT_VCENTER);
  text(dc, g.fUi, kDim, "Offset", g.rc(12, kBarY + 40, 70, 22), DT_LEFT | DT_VCENTER);
  text(dc, g.fUi, kDim, "Source", g.rc(12, kBarY + 73, 70, 22), DT_LEFT | DT_VCENTER);
  text(dc, g.fUi, kDim, "ms", g.rc(184, kBarY + 40, 30, 22), DT_LEFT | DT_VCENTER);
  if (!u.offsetNote.empty()) text(dc, g.fUi, kDim, u.offsetNote, g.rc(216, kBarY + 40, 120, 22), DT_LEFT | DT_VCENTER);
  text(dc, g.fUi, ui_color(u.sendColor), u.send, g.rc(kGuiW - 160, kBarY + 7, 148, 22), DT_RIGHT | DT_VCENTER);

  fill(dc, g.rc(84, kBarY + 6, 154, 24), kField);   // field grounds behind the borderless EDITs
  fill(dc, g.rc(84, kBarY + 39, 74, 24), kField);

  for (int dir : {1, -1}) {  // stepper
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

  static const char *names[3] = {"Auto", "LTC only", "DAW only"};
  const int mode = g.plug->mode.load();
  for (int i = 0; i < 3; i++) {
    const RECT r = seg_rect(g, i);
    fill(dc, r, i == mode ? kSel : kField);
    text(dc, g.fUi, i == mode ? kText : kDim, names[i], r, DT_CENTER | DT_VCENTER);
  }
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  Gui *g = (Gui *)GetWindowLongPtrW(h, GWLP_USERDATA);
  if (m == WM_NCCREATE) {
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)l)->lpCreateParams);
    return DefWindowProcW(h, m, w, l);
  }
  if (!g || !g->plug) return DefWindowProcW(h, m, w, l);
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
      if (w == kTimerRepeat) { if (g->repeatDir) step_offset(*g, g->repeatDir); return 0; }
      if ((g->ticks++ % 30) == 0) g->plug->refreshProject();
      if (g->plug->sender.offsetMs() != g->offShown && GetFocus() != g->off) show_offset(*g, g->plug->sender.offsetMs());
      InvalidateRect(h, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      SetFocus(h);
      for (int i = 0; i < 3; i++) { const RECT r = seg_rect(*g, i); if (PtInRect(&r, p)) g->plug->setModeFromGui(i); }
      for (int dir : {1, -1}) {
        const RECT r = step_rect(*g, dir);
        if (!PtInRect(&r, p)) continue;
        step_offset(*g, dir);
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
      if (LOWORD(w) == kIdIp && HIWORD(w) == EN_CHANGE && g->ipBad) { g->ipBad = false; InvalidateRect(g->ip, nullptr, TRUE); }
      if (LOWORD(w) == kIdOffset && HIWORD(w) == EN_KILLFOCUS) show_offset(*g, g->plug->sender.offsetMs());
      return 0;
    case WM_CTLCOLOREDIT: {
      HDC dc = (HDC)w;
      SetTextColor(dc, ((HWND)l == g->ip && g->ipBad) ? kRed : kText);
      SetBkColor(dc, kField);
      return (LRESULT)g->bField;
    }
  }
  return DefWindowProcW(h, m, w, l);
}

const wchar_t *kClass = L"CTLTCArtNetView";

HFONT font(const wchar_t *face, int px, int weight) {
  return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

void destroy_window(Gui &g) {
  if (g.hwnd) { KillTimer(g.hwnd, kTimerTick); DestroyWindow(g.hwnd); g.hwnd = g.ip = g.off = nullptr; }
  for (HGDIOBJ o : {(HGDIOBJ)g.fTc, (HGDIOBJ)g.fUi, (HGDIOBJ)g.fMono, (HGDIOBJ)g.bField}) if (o) DeleteObject(o);
  g.fTc = g.fUi = g.fMono = nullptr;
  g.bField = nullptr;
}

}  // namespace

const char *gui_api() { return CLAP_WINDOW_API_WIN32; }

bool gui_make(Plugin *s) {
  if (!s->view) { Gui *g = new Gui(); g->plug = s; s->view = g; }
  return true;
}

void gui_free(Plugin *s) {
  Gui *g = (Gui *)s->view;
  if (!g) return;
  destroy_window(*g);
  s->view = nullptr;
  delete g;
}

bool gui_parent(Plugin *s, const clap_window_t *win) {
  Gui *g = (Gui *)s->view;
  if (!g || !win || !win->win32) return false;
  destroy_window(*g);
  g->scale = s->guiScale;

  WNDCLASSW wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = module();
  wc.lpszClassName = kClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);  // fails harmlessly when already registered

  g->fTc = font(L"Consolas", g->px(64), FW_SEMIBOLD);
  g->fUi = font(L"Segoe UI", g->px(14), FW_NORMAL);
  g->fMono = font(L"Consolas", g->px(14), FW_NORMAL);
  g->bField = CreateSolidBrush(kField);

  g->hwnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, g->px(kGuiW), g->px(kGuiH), (HWND)win->win32, nullptr, module(), g);
  if (!g->hwnd) return false;
  auto edit = [&](int id, double x, double y, double w, DWORD style) {
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | style, g->px(x), g->px(y), g->px(w), g->px(18), g->hwnd, (HMENU)(INT_PTR)id, module(), nullptr);
    SendMessageW(e, WM_SETFONT, (WPARAM)g->fMono, TRUE);
    SetWindowSubclass(e, edit_proc, 1, (DWORD_PTR)g);
    return e;
  };
  g->ip = edit(kIdIp, 90, kBarY + 9, 142, ES_LEFT);
  g->off = edit(kIdOffset, 88, kBarY + 42, 64, ES_RIGHT);
  SendMessageW(g->ip, EM_SETCUEBANNER, TRUE, (LPARAM)L"IP address");
  SetWindowTextA(g->ip, s->ip.c_str());
  show_offset(*g, s->sender.offsetMs());
  SetTimer(g->hwnd, kTimerTick, 33, nullptr);
  return true;
}

void gui_visible(Plugin *s, bool on) {
  Gui *g = (Gui *)s->view;
  if (g && g->hwnd) ShowWindow(g->hwnd, on ? SW_SHOW : SW_HIDE);
}
#endif
