// 3P Compressor - native Win32 + GDI+ front-end for the 3P engine.
// One button does everything:  Browse  ->  Compress (any file) / Extract (.3p file)  ->  Cancel while running.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <atomic>
#include <cstdio>
#include <string>
#include "engine3p.hpp"

namespace {

const UINT WM_P3_PROGRESS = WM_APP + 1;
const UINT WM_P3_DONE = WM_APP + 2;
const int kLogicalW = 640, kLogicalH = 400;

typedef unsigned long long u64;
namespace fs = std::filesystem;

// ------------------------------------------------------------------ application state
enum Mode { kHome, kReady, kBusy };

struct App {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    ULONG_PTR gdiToken = 0;
    float scale = 1.0f;
    Mode mode = kHome;
    bool extract = false;
    fs::path file;
    std::wstring shownName;            // original file name (for .3p: read from the metadata)
    u64 before = 0, after = 0;
    std::atomic<u64> progressPpm{0};   // 0..1,000,000
    std::atomic<u64> outBytes{0};
    std::atomic<bool> cancel{false};
    double progress = 0.0;
    std::wstring info;
    bool infoIsError = false;
    HANDLE thread = nullptr;
    DWORD startTick = 0;
    bool hover = false, pressed = false;
    RECT button = {0, 0, 0, 0};
} g;

// ------------------------------------------------------------------ small helpers
std::wstring widen(const std::string& s) {                 // UTF-8 -> UTF-16
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring ascii(const char* s) { std::wstring w; while (*s) w += (wchar_t)(unsigned char)*s++; return w; }

std::wstring fmtSize(u64 bytes) {
    if (bytes == 0) return L"0 GB";
    static const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)bytes; int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    char buf[48];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", bytes);
    else if (v >= 100.0) std::snprintf(buf, sizeof buf, "%.0f %s", v, unit[u]);
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, unit[u]);
    return ascii(buf);
}
std::wstring fmtDuration(double sec) {
    char buf[48];
    if (sec < 90) std::snprintf(buf, sizeof buf, "%d s", (int)(sec + 0.5));
    else if (sec < 5400) std::snprintf(buf, sizeof buf, "%d min", (int)(sec / 60.0 + 0.5));
    else std::snprintf(buf, sizeof buf, "%d h %d min", (int)(sec / 3600), (int)(((long long)sec % 3600) / 60));
    return ascii(buf);
}
bool hasExt3p(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e == L".3p";
}
int S(int v) { return (int)(v * g.scale + 0.5f); }
float Sf(float v) { return v * g.scale; }

// ------------------------------------------------------------------ drawing (GDI+)
void roundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h) {
    float r = h / 2.0f, d = r * 2.0f;
    if (w < d) { r = w / 2.0f; d = w; }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}
void fillPill(Gdiplus::Graphics& gr, float x, float y, float w, float h, const Gdiplus::Color& c) {
    if (w <= 0.5f) return;
    Gdiplus::GraphicsPath path; roundedRect(path, x, y, w, h);
    Gdiplus::SolidBrush br(c); gr.FillPath(&br, &path);
}
void drawText(Gdiplus::Graphics& gr, const std::wstring& text, const wchar_t* family, float px, int style,
              const Gdiplus::Color& color, float x, float y, float w, float h) {
    Gdiplus::FontFamily ff(family);
    const Gdiplus::FontFamily* use = &ff;
    if (ff.GetLastStatus() != Gdiplus::Ok) use = Gdiplus::FontFamily::GenericSansSerif();   // font not installed
    Gdiplus::Font font(use, px, style, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    Gdiplus::SolidBrush br(color);
    gr.DrawString(text.c_str(), (INT)text.size(), &font, Gdiplus::RectF(x, y, w, h), &fmt, &br);
}

double percentSmaller() {
    if (g.mode == kHome && g.before == 0) return 0.0;
    double v = 0.0;
    if (g.extract) { if (g.after > 0) v = 1.0 - (double)g.before / (double)g.after; }
    else {
        double processed = (double)g.before * g.progress;
        if (processed > 0.0) v = 1.0 - (double)g.after / processed;
    }
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

void paint(HDC hdc, int W, int H) {
    Gdiplus::Bitmap back(W, H, PixelFormat32bppPARGB);
    Gdiplus::Graphics gr(&back);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    // background: deep slate-blue gradient with a few faint panels (echoes the artwork in the design)
    Gdiplus::LinearGradientBrush bg(Gdiplus::Point(0, 0), Gdiplus::Point(0, H), Gdiplus::Color(255, 64, 90, 118), Gdiplus::Color(255, 40, 62, 86));
    gr.FillRectangle(&bg, 0, 0, W, H);
    unsigned seed = 7;
    for (int i = 0; i < 18; i++) {
        seed = seed * 1664525u + 1013904223u; float x = (float)(seed >> 8 & 1023) / 1023.0f * W;
        seed = seed * 1664525u + 1013904223u; float y = (float)(seed >> 8 & 1023) / 1023.0f * H;
        seed = seed * 1664525u + 1013904223u; float w = Sf(60.0f + (float)(seed >> 8 & 127));
        seed = seed * 1664525u + 1013904223u; float h = Sf(30.0f + (float)(seed >> 8 & 63));
        Gdiplus::SolidBrush faint(Gdiplus::Color(9, 255, 255, 255));
        gr.FillRectangle(&faint, x - w / 2, y - h / 2, w, h);
    }

    const float cx = W / 2.0f;
    const Gdiplus::Color white(255, 255, 255, 255), soft(255, 205, 220, 236);

    drawText(gr, L"3P Compressor", L"Segoe UI Light", Sf(40), Gdiplus::FontStyleRegular, white, 0, Sf(26), (float)W, Sf(60));

    if (!g.shownName.empty()) {
        drawText(gr, L"Selected file:", L"Segoe UI", Sf(14), Gdiplus::FontStyleRegular, soft, 0, Sf(96), (float)W, Sf(20));
        drawText(gr, g.shownName, L"Segoe UI Semibold", Sf(20), Gdiplus::FontStyleRegular, white, Sf(60), Sf(114), (float)W - Sf(120), Sf(30));
    }

    // the one button
    const wchar_t* label = g.mode == kBusy ? L"Cancel" : (g.mode == kReady ? (g.extract ? L"Extract" : L"Compress") : L"Browse");
    float bw = Sf(190), bh = Sf(46), bx = cx - bw / 2, by = Sf(158);
    g.button.left = (LONG)bx; g.button.top = (LONG)by; g.button.right = (LONG)(bx + bw); g.button.bottom = (LONG)(by + bh);
    Gdiplus::Color face = g.pressed ? Gdiplus::Color(255, 214, 222, 232) : (g.hover ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 240, 244, 248));
    if (g.mode == kBusy) face = g.pressed ? Gdiplus::Color(255, 170, 186, 204) : (g.hover ? Gdiplus::Color(255, 205, 218, 232) : Gdiplus::Color(255, 186, 202, 220));
    fillPill(gr, bx, by + Sf(2), bw, bh, Gdiplus::Color(40, 0, 0, 0));            // soft shadow
    fillPill(gr, bx, by, bw, bh, face);
    drawText(gr, label, L"Segoe UI", Sf(19), Gdiplus::FontStyleRegular, Gdiplus::Color(255, 44, 60, 80), bx, by, bw, bh);

    // status / info line
    std::wstring line = g.info;
    if (g.mode == kBusy) {
        double elapsed = (GetTickCount() - g.startTick) / 1000.0;
        std::wstring pct = std::to_wstring((int)(g.progress * 100.0)) + L"%";
        line = (g.extract ? L"Extracting  " : L"Compressing  ") + pct;
        double processed = (double)(g.extract ? g.after : g.before) * g.progress;
        if (elapsed > 2.0 && g.progress > 0.002) {
            double rate = processed / elapsed;
            line += L"   \u00B7   " + fmtSize((u64)rate) + L"/s   \u00B7   " + fmtDuration(elapsed * (1.0 - g.progress) / g.progress) + L" left";
        }
    }
    if (!line.empty())
        drawText(gr, line, L"Segoe UI", Sf(15), Gdiplus::FontStyleRegular, g.infoIsError ? Gdiplus::Color(255, 255, 178, 168) : soft,
                 Sf(30), Sf(214), (float)W - Sf(60), Sf(26));

    // before / after columns
    float lx = W * 0.265f, rx = W * 0.735f, colW = Sf(210);
    u64 afterShown = g.extract ? g.after : (g.mode == kBusy ? g.outBytes.load() : g.after);
    drawText(gr, fmtSize(g.before), L"Segoe UI Semibold", Sf(34), Gdiplus::FontStyleRegular, white, lx - colW / 2, Sf(252), colW, Sf(44));
    drawText(gr, L"before", L"Segoe UI", Sf(17), Gdiplus::FontStyleRegular, soft, lx - colW / 2, Sf(296), colW, Sf(24));
    drawText(gr, fmtSize(afterShown), L"Segoe UI Semibold", Sf(34), Gdiplus::FontStyleRegular, white, rx - colW / 2, Sf(252), colW, Sf(44));
    drawText(gr, L"after", L"Segoe UI", Sf(17), Gdiplus::FontStyleRegular, soft, rx - colW / 2, Sf(296), colW, Sf(24));

    // bars: left = input processed (yellow), right = output size relative to the original (green)
    float barW = Sf(190), barH = Sf(15), barY = Sf(334);
    Gdiplus::Color track(70, 214, 226, 240);
    fillPill(gr, lx - barW / 2, barY, barW, barH, track);
    fillPill(gr, rx - barW / 2, barY, barW, barH, track);
    double leftFill = g.progress, rightFill = 0.0;
    if (g.extract) rightFill = g.progress;
    else if (g.before > 0) rightFill = std::min(1.0, (double)afterShown / (double)g.before);
    if (g.mode == kHome && g.before == 0) { leftFill = 0; rightFill = 0; }
    fillPill(gr, lx - barW / 2, barY, (float)(barW * leftFill), barH, Gdiplus::Color(255, 238, 200, 122));
    fillPill(gr, rx - barW / 2, barY, (float)(barW * rightFill), barH, Gdiplus::Color(255, 128, 222, 152));

    // centre: percentage smaller
    double pct = percentSmaller();
    drawText(gr, std::to_wstring((int)(pct * 100.0 + 0.5)) + L"%", L"Segoe UI Semibold", Sf(30), Gdiplus::FontStyleRegular, white, cx - Sf(60), Sf(308), Sf(120), Sf(36));
    drawText(gr, L"smaller", L"Segoe UI", Sf(16), Gdiplus::FontStyleRegular, soft, cx - Sf(60), Sf(342), Sf(120), Sf(22));

    Gdiplus::Graphics screen(hdc);
    screen.DrawImage(&back, 0, 0);
}

// ------------------------------------------------------------------ logic
void invalidate() { InvalidateRect(g.hwnd, nullptr, FALSE); }

void setHome(const std::wstring& message, bool isError) {
    g.mode = kHome; g.info = message; g.infoIsError = isError; invalidate();
}

void selectFile(const fs::path& path) {
    if (g.mode == kBusy) return;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { setHome(L"That is not a file.", true); return; }
    u64 size = (u64)fs::file_size(path, ec);
    if (hasExt3p(path)) {
        p3::Result r = p3::inspect_file(path);
        if (r.status != p3::Status::Ok) { g.file.clear(); g.shownName.clear(); g.before = g.after = 0; g.progress = 0; setHome(L"This is not a valid .3p file.", true); return; }
        g.extract = true; g.shownName = widen(r.storedName); g.before = size; g.after = r.outSize;
    } else {
        g.extract = false; g.shownName = path.filename().wstring(); g.before = size; g.after = 0;
    }
    g.file = path; g.progress = 0.0; g.progressPpm = 0; g.outBytes = 0; g.info.clear(); g.infoIsError = false; g.mode = kReady;
    invalidate();
}

void browse() {
    wchar_t buf[32768] = {0};
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = 32768;
    ofn.lpstrTitle = L"Choose a file to compress or a .3p file to extract";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) selectFile(fs::path(std::wstring(buf)));
}

DWORD WINAPI jobThread(LPVOID) {
    p3::Options opt;
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) {
        u64 budget = (u64)(ms.ullAvailPhys * 0.6);
        opt.memoryBudget = budget < ((u64)600 << 20) ? ((u64)600 << 20) : budget;
    }
    p3::ProgressFn progress = [](double f, p3::u64 out) {
        g.progressPpm = (u64)(f * 1000000.0); g.outBytes = out;
        PostMessageW(g.hwnd, WM_P3_PROGRESS, 0, 0);
        return !g.cancel.load();
    };
    p3::Result* r = new p3::Result(g.extract ? p3::restore_file(g.file, opt, progress) : p3::compress_file(g.file, opt, progress));
    PostMessageW(g.hwnd, WM_P3_DONE, 0, (LPARAM)r);
    return 0;
}

void startJob() {
    if (g.mode != kReady) return;
    g.cancel = false; g.progress = 0; g.progressPpm = 0; g.outBytes = 0; g.info.clear(); g.infoIsError = false;
    g.startTick = GetTickCount(); g.mode = kBusy;
    g.thread = CreateThread(nullptr, 0, jobThread, nullptr, 0, nullptr);
    if (!g.thread) { g.mode = kReady; g.info = L"Could not start the worker thread."; g.infoIsError = true; }
    invalidate();
}

void onDone(p3::Result* r) {
    if (g.thread) { WaitForSingleObject(g.thread, INFINITE); CloseHandle(g.thread); g.thread = nullptr; }
    if (r->status == p3::Status::Ok) {
        g.progress = 1.0; g.progressPpm = 1000000;
        if (g.extract) { g.before = r->inSize; g.after = r->outSize; g.info = L"Restored as  " + r->outPath.filename().wstring(); }
        else { g.before = r->inSize; g.after = r->outSize; g.outBytes = r->outSize; g.info = L"Saved as  " + r->outPath.filename().wstring(); }
        g.infoIsError = false; g.mode = kHome;                 // button becomes "Browse" again
    } else if (r->status == p3::Status::Cancelled) {
        g.progress = 0; g.info = L"Cancelled."; g.infoIsError = false; g.mode = kReady;
    } else {
        g.info = widen(r->message); g.infoIsError = true; g.progress = 0; g.mode = kHome;
    }
    delete r;
    invalidate();
}

void onButton() {
    if (g.mode == kBusy) { g.cancel = true; g.info = L"Cancelling..."; invalidate(); }
    else if (g.mode == kReady) startJob();
    else browse();
}

bool inButton(int x, int y) { POINT p = {x, y}; return PtInRect(&g.button, p) != 0; }

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        paint(dc, rc.right - rc.left, rc.bottom - rc.top);
        EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        bool over = inButton((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        if (over != g.hover) { g.hover = over; invalidate(); }
        if (over) SetCursor(LoadCursor(nullptr, IDC_HAND));
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t);
        return 0; }
    case WM_MOUSELEAVE: g.hover = false; g.pressed = false; invalidate(); return 0;
    case WM_LBUTTONDOWN: if (inButton((int)(short)LOWORD(lp), (int)(short)HIWORD(lp))) { g.pressed = true; SetCapture(h); invalidate(); } SetFocus(h); return 0;
    case WM_LBUTTONUP: {
        bool was = g.pressed; g.pressed = false; ReleaseCapture(); invalidate();
        if (was && inButton((int)(short)LOWORD(lp), (int)(short)HIWORD(lp))) onButton();
        return 0; }
    case WM_KEYDOWN:
        if (wp == VK_RETURN || wp == VK_SPACE) { onButton(); return 0; }
        if (wp == VK_ESCAPE && g.mode == kBusy) { onButton(); return 0; }
        break;
    case WM_SETCURSOR: if (g.hover && LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; } break;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp; wchar_t path[MAX_PATH * 4];
        if (DragQueryFileW(drop, 0, path, MAX_PATH * 4)) selectFile(fs::path(std::wstring(path)));
        DragFinish(drop); return 0; }
    case WM_P3_PROGRESS: g.progress = (double)g.progressPpm.load() / 1000000.0; invalidate(); return 0;
    case WM_P3_DONE: onDone((p3::Result*)lp); return 0;
    case WM_CLOSE:
        if (g.mode == kBusy) {
            if (MessageBoxW(h, L"A job is still running. Cancel it and quit?", L"3P Compressor", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            g.cancel = true;
            if (g.thread) { WaitForSingleObject(g.thread, INFINITE); CloseHandle(g.thread); g.thread = nullptr; }
        }
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void enableDarkTitleBar(HWND h) {                           // best effort (Windows 10 2004+ / 11)
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
    SetAttr fn = (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute");
    BOOL on = TRUE;
    if (fn) { if (FAILED(fn(h, 20, &on, sizeof on))) fn(h, 19, &on, sizeof on); }
    FreeLibrary(dwm);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g.inst = inst;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g.gdiToken, &gsi, nullptr);

    HDC screen = GetDC(nullptr);
    g.scale = (float)GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, screen);

    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = wndProc; wc.hInstance = inst; wc.lpszClassName = L"P3CompressorWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = {0, 0, S(kLogicalW), S(kLogicalH)};
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_ACCEPTFILES);
    int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2;
    g.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"3P Compressor", style, x, y, w, hgt, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 1;
    enableDarkTitleBar(g.hwnd);
    DragAcceptFiles(g.hwnd, TRUE);
    ShowWindow(g.hwnd, show);

    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);     // "Open with" / double-click on a .3p
    if (argv && argc > 1) selectFile(fs::path(std::wstring(argv[1])));
    if (argv) LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Gdiplus::GdiplusShutdown(g.gdiToken);
    return (int)msg.wParam;
}
