#include "dota2_capture.h"
#include "dhash.h"

#if __has_include("hero_hashes.h")
#  include "hero_hashes.h"
#  define HERO_DB_LOADED 1
#endif

#include <iostream>
#include <string>
#include <thread>
#include <cstring>
#include <cstdio>
#include <algorithm>

using namespace dota2;
using std::max;

static GdiplusSession gdiplusSession;

static void enableDpiAwareness() {

    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI* SetPMDAC_t)(void*);
        if (auto fn = reinterpret_cast<SetPMDAC_t>(
                GetProcAddress(u32, "SetProcessDpiAwarenessContext"))) {

            if (fn(reinterpret_cast<void*>(-4))) return;

            fn(reinterpret_cast<void*>(-3));
            return;
        }
    }

    if (HMODULE shc = LoadLibraryW(L"shcore.dll")) {
        typedef HRESULT(WINAPI* SPDA_t)(int);
        if (auto fn = reinterpret_cast<SPDA_t>(
                GetProcAddress(shc, "SetProcessDpiAwareness")))
            fn(2);
    }
}

static HeroRecognizer* g_recognizer = nullptr;

static void bringThisProgramToFront() {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd || !IsWindow(hwnd)) return;
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    DWORD fg_tid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD our_tid = GetCurrentThreadId();
    if (fg_tid != our_tid)
        AttachThreadInput(our_tid, fg_tid, TRUE);

    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);

    if (fg_tid != our_tid)
        AttachThreadInput(our_tid, fg_tid, FALSE);
}

static void updateOverlayPosition(HWND hwnd, Dota2Capture* cap) {
    if (!cap || !cap->isWindowFound()) {
        const int width = 160;
        const int height = 34;
        SetWindowPos(hwnd, HWND_TOPMOST, 10, 10, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return;
    }

    RECT gameRect;
    if (GetWindowRect(cap->gameWindowHandle(), &gameRect)) {
        int gameWidth = gameRect.right - gameRect.left;
        int gameHeight = gameRect.bottom - gameRect.top;

        int width = max(80, gameWidth / 10);
        int height = max(24, gameHeight / 25);

        int x = gameRect.left + 10;
        int y = gameRect.top + 10 + height;

        SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    } else {
        const int width = 160;
        const int height = 34;
        SetWindowPos(hwnd, HWND_TOPMOST, 10, 10, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

static LRESULT CALLBACK overlayWndProc(HWND hwnd,
                                      UINT msg,
                                      WPARAM wParam,
                                      LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        const auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hwnd, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        SetTimer(hwnd, 1, 100, nullptr);
        auto* cap = reinterpret_cast<Dota2Capture*>(
            GetWindowLongPtr(hwnd, GWLP_USERDATA));
        updateOverlayPosition(hwnd, cap);
        return 0;
    }
    case WM_TIMER: {
        if (wParam != 1) break;
        auto* cap = reinterpret_cast<Dota2Capture*>(
            GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!cap) break;

        if (!cap->isWindowFound())
            cap->findGameWindow();

        HWND fg = GetForegroundWindow();
        if (cap->isWindowFound() && fg == cap->gameWindowHandle()) {
            if (!IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            updateOverlayPosition(hwnd, cap);
        } else {
            if (IsWindowVisible(hwnd))
                ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        bringThisProgramToFront();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);

        int btnWidth = rect.right - rect.left;
        int btnHeight = rect.bottom - rect.top;

        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 24));
        FillRect(dc, &rect, bg);
        DeleteObject(bg);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));

        int fontSize = max(8, btnHeight / 2);
        HFONT font = CreateFontW(fontSize, 0, 0, 0, FW_SEMIBOLD,
                                 FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE,
                                 L"Segoe UI");
        HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, font));
        DrawTextW(dc, L"Bring app", -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        DeleteObject(font);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI overlayThread(LPVOID param) {
    auto* cap = reinterpret_cast<Dota2Capture*>(param);
    HINSTANCE hinst = GetModuleHandle(nullptr);
    static constexpr wchar_t kOverlayClass[] = L"Dota2CaptureOverlay";

    WNDCLASSW wc{};
    wc.lpfnWndProc   = overlayWndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = kOverlayClass;
    wc.hCursor       = LoadCursor(nullptr, IDC_HAND);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    if (!RegisterClassW(&wc))
        return 1;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                kOverlayClass,
                                L"",
                                WS_POPUP,
                                10, 10, 160, 34,
                                nullptr,
                                nullptr,
                                hinst,
                                cap);
    if (!hwnd)
        return 1;

    ShowWindow(hwnd, SW_HIDE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(hwnd);
    UnregisterClassW(kOverlayClass, hinst);
    return 0;
}

static void saveDebugOverlay(Dota2Capture& cap,
                             const std::filesystem::path& outDir)
{
    Bitmap full = cap.captureFullWindow();
    if (full.empty()) {
        std::puts("[debug] captureFullWindow() empty — is Dota 2 running?");
        return;
    }

    const int W = full.width;
    const int H = full.height;

    Gdiplus::Bitmap gdiBmp(W, H, PixelFormat32bppARGB);
    {
        Gdiplus::BitmapData bd{};
        Gdiplus::Rect lr(0, 0, W, H);
        gdiBmp.LockBits(&lr, Gdiplus::ImageLockModeWrite,
                        PixelFormat32bppARGB, &bd);
        std::memcpy(bd.Scan0, full.pixels.data(),
                    static_cast<size_t>(W) * H * 4);
        gdiBmp.UnlockBits(&bd);
    }

    {
        Gdiplus::Graphics   g(&gdiBmp);
        Gdiplus::Pen        redPen(Gdiplus::Color(255, 255, 0, 0), 2.0f);
        Gdiplus::SolidBrush fillBrush(Gdiplus::Color(60, 255, 0, 0));
        Gdiplus::Font       font(L"Arial", 10.0f);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 0));
        Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(200, 0, 0, 0));

        for (const auto& reg : cap.regions()) {
            const RECT& r = reg.rect;
            float x  = static_cast<float>(r.left);
            float y  = static_cast<float>(r.top);
            float rw = static_cast<float>(r.right  - r.left);
            float rh = static_cast<float>(r.bottom - r.top);

            g.FillRectangle(&fillBrush, x, y, rw, rh);
            g.DrawRectangle(&redPen,    x, y, rw, rh);

            wchar_t label[8];
            std::swprintf(label, 8, L"%s%d",
                          (reg.slot < 5) ? L"R" : L"D",
                          (reg.slot < 5) ? reg.slot : reg.slot - 5);
            g.DrawString(label, -1, &font, Gdiplus::PointF(x+3,y+3), &shadowBrush);
            g.DrawString(label, -1, &font, Gdiplus::PointF(x+2,y+2), &textBrush);
        }
    }

    std::filesystem::create_directories(outDir);
    std::filesystem::path outPath = outDir / "debug_fullscreen.png";

    UINT num = 0, sz = 0;
    Gdiplus::GetImageEncodersSize(&num, &sz);
    std::vector<uint8_t> buf(sz);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, sz, codecs);

    CLSID pngClsid{};
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0)
            { pngClsid = codecs[i].Clsid; break; }

    if (gdiBmp.Save(outPath.wstring().c_str(), &pngClsid) == Gdiplus::Ok) {
        std::printf("[debug] Saved: %s\n", outPath.string().c_str());
        std::puts("[debug] Check if RED boxes cover the portraits.");
        std::puts("[debug] If offset — edit HudLayout in dota2_capture.h and rebuild.");
        std::printf("[debug] Window: %dx%d\n", W, H);
        for (const auto& reg : cap.regions()) {
            const RECT& r = reg.rect;
            std::printf("  [%s #%d] left=%ld top=%ld  size=%ldx%ld\n",
                        (reg.slot < 5) ? "Radiant" : "Dire",
                        (reg.slot < 5) ? reg.slot : reg.slot - 5,
                        r.left, r.top,
                        r.right - r.left, r.bottom - r.top);
        }
    } else {
        std::puts("[debug] Failed to save PNG.");
    }
}

static void printPortrait(int slot, const Bitmap& bmp) {
    const char* team = (slot < 5) ? "Radiant" : "Dire";
    int idx = (slot < 5) ? slot : slot - 5;

    if (g_recognizer && g_recognizer->size() > 0) {
        HeroMatch m = g_recognizer->recognize(bmp);
        std::printf("[%s #%d]  %-26s  score=%.3f%s\n",
                    team, idx,
                    m.name ? m.name : "unknown",
                    m.score,
                    m.confident() ? "" : "  (low confidence)");
    } else {
        std::printf("[%s #%d] %dx%d px\n", team, idx, bmp.width, bmp.height);
    }
}

static void interactiveMenu(Dota2Capture& cap,
                            const std::filesystem::path& outDir)
{
    std::puts("\n=== Dota 2 Portrait Extractor ===");
    std::puts("  f  - find game window");
    std::puts("  c  - capture portraits once");
    std::puts("  s  - save last captured portraits");
    std::puts("  d  - debug: save full window + region overlay PNG");
    std::puts("  l  - start capture loop ('q' to stop)");
    std::puts("  q  - quit");
    std::puts("=================================\n");

    std::string line;
    while (true) {
        std::fputs("> ", stdout);
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        switch (line[0]) {
        case 'f': {
            if (cap.findGameWindow()) {
                auto r = cap.gameResolution();
                std::printf("Found Dota 2 window: %dx%d\n", r.width, r.height);
            } else std::puts("Not found.");
            break;
        }
        case 'c': {
            if (!cap.isWindowFound()) { std::puts("No window - run 'f' first."); break; }
            std::printf("Extracted %d portraits.\n", cap.capturePortraits());
            break;
        }
        case 's':
            cap.savePortraits();
            std::puts("Saved.");
            break;
        case 'd': {
            if (!cap.isWindowFound()) cap.findGameWindow();
            if (!cap.isWindowFound()) { std::puts("Window not found."); break; }
            saveDebugOverlay(cap, outDir);
            break;
        }
        case 'l': {
            if (!cap.isWindowFound()) cap.findGameWindow();
            if (!cap.isWindowFound()) { std::puts("Window not found."); break; }
            std::puts("Loop started ('q' + Enter to stop)...");
            std::thread t([&]{ cap.runLoop(500); });
            std::string cmd;
            while (std::getline(std::cin, cmd))
                if (!cmd.empty() && cmd[0] == 'q') break;
            cap.stopLoop(); t.join();
            std::puts("Stopped.");
            break;
        }
        case 'q': return;
        default:  std::puts("Unknown command.");
        }
    }
}

int main(int argc, char* argv[]) {
    enableDpiAwareness();
    std::filesystem::path outDir = "portraits";
    bool singleShot = false;
    bool debugMode  = false;
    int  loopMs     = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "--once")  { singleShot = true; }
        else if (arg == "--debug") { debugMode  = true; }
        else if (arg == "--loop" && i + 1 < argc) { loopMs = std::stoi(argv[++i]); }
        else if (arg == "--out"  && i + 1 < argc) { outDir = argv[++i]; }
    }

    Dota2Capture cap(outDir);

#ifdef HERO_DB_LOADED
    static HeroRecognizer recognizer(g_hero_db, g_hero_db_size);
    g_recognizer = &recognizer;
    std::printf("[db] %zu hero hashes loaded\n", recognizer.size());
#else
    std::puts("[db] hero_hashes.h not found — run build_hero_db.exe");
#endif

    cap.setCallback(printPortrait);

    HANDLE overlayThreadHandle = CreateThread(nullptr, 0, overlayThread, &cap, 0, nullptr);

    bool listWindows = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--list-windows") listWindows = true;

    if (listWindows) {
        dota2::ListAllWindows();
        return 0;
    }

    if (debugMode) {
        if (!cap.findGameWindow()) { std::fputs("Window not found.\n", stderr); return 1; }
        saveDebugOverlay(cap, outDir);
        return 0;
    }
    if (singleShot) {
        if (!cap.findGameWindow()) { std::fputs("Window not found.\n", stderr); return 1; }
        int n = cap.capturePortraits();
        cap.savePortraits();
        std::printf("Done. %d portraits saved to '%s'\n", n, outDir.string().c_str());
        return 0;
    }
    if (loopMs > 0) {
        if (!cap.findGameWindow()) { std::fputs("Window not found.\n", stderr); return 1; }
        std::printf("Looping every %d ms. Ctrl-C to stop.\n", loopMs);
        cap.runLoop(loopMs);
        return 0;
    }

    interactiveMenu(cap, outDir);

    PostThreadMessage(GetThreadId(overlayThreadHandle), WM_QUIT, 0, 0);
    WaitForSingleObject(overlayThreadHandle, 1000);
    CloseHandle(overlayThreadHandle);

    return 0;
}
