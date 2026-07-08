/*
 * dota2_capture.cpp — захват окна Dota 2 через PrintWindow/GDI.
 * Портреты героев + индикаторы позиций. refreshResolution() для смены разрешения.
 */

#include "dota2_capture.h"
#include "common.h"
#include <algorithm>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

namespace dota2 {

static bool getEncoderClsid(const wchar_t* format, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<uint8_t> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, format) == 0) {
            *clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

struct FindCtx { HWND result; };

static BOOL CALLBACK enumAllCallback(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    char title[256]{};
    char cls[128]{};
    GetWindowTextA(hwnd, title, sizeof(title));
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (title[0] != 0)
        LOG_INFO("  HWND=" << static_cast<void*>(hwnd) << "  class=" << cls << "  title=" << title);
    return TRUE;
}

void ListAllWindows() {
    LOG_INFO("--- Visible windows ---");
    EnumWindows(enumAllCallback, 0);
    LOG_INFO("--- End ---");
}

static BOOL CALLBACK enumCallback(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    char cls[128]{};
    GetClassNameA(hwnd, cls, sizeof(cls));
    char title[256]{};
    GetWindowTextA(hwnd, title, sizeof(title));
    if (title[0] == 0) return TRUE;
    bool classMatch = (strcmp(cls, "SDL_app")  == 0 ||
                       strcmp(cls, "Valve001") == 0);
    bool titleExact = (strcmp(title, "Dota 2") == 0);
    if (classMatch && titleExact) {
        reinterpret_cast<FindCtx*>(lp)->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

Dota2Capture::Dota2Capture(std::filesystem::path output_dir)
    : output_dir_(std::move(output_dir)) {}

bool Dota2Capture::findGameWindow() {
    FindCtx ctx{ nullptr };
    EnumWindows(enumCallback, reinterpret_cast<LPARAM>(&ctx));
    hwnd_ = ctx.result;
    if (!hwnd_) return false;

    if (IsIconic(hwnd_)) { hwnd_ = nullptr; return false; }

    {
        HDC wdc = GetDC(hwnd_);
        int phys_w = GetDeviceCaps(wdc, DESKTOPHORZRES);
        int phys_h = GetDeviceCaps(wdc, DESKTOPVERTRES);

        RECT cr{};
        GetClientRect(hwnd_, &cr);
        int log_w = cr.right  - cr.left;
        int log_h = cr.bottom - cr.top;

        int desk_log_w = GetSystemMetrics(SM_CXSCREEN);
        int desk_log_h = GetSystemMetrics(SM_CYSCREEN);

        if (desk_log_w > 0 && desk_log_h > 0) {
            res_.width  = MulDiv(log_w, phys_w, desk_log_w);
            res_.height = MulDiv(log_h, phys_h, desk_log_h);
        } else {
            res_.width  = log_w;
            res_.height = log_h;
        }
        ReleaseDC(hwnd_, wdc);
    }

    if (res_.width < 640 || res_.height < 480) {
        hwnd_ = nullptr;
        return false;
    }

    // ── Auto-select layout by aspect ratio ───────────────────────────────────
    layout_ = selectStrategyLayout(res_.width, res_.height);
    {
        float ratio = static_cast<float>(res_.width) / static_cast<float>(res_.height);
        const char* name =
            (ratio < 1.467f) ? "4:3" :
            (ratio < 1.689f) ? "16:10" :
            (ratio < 2.056f) ? "16:9" : "21:9";
        LOG_INFO("[capture] Window " << res_.width << "x" << res_.height
                 << "  ratio=" << ratio << "  layout=" << name);
    }

    computeRegions();
    return true;
}

bool Dota2Capture::refreshResolution() {
    if (!hwnd_ || !IsWindow(hwnd_)) return false;
    RECT cr{};
    GetClientRect(hwnd_, &cr);
    int w = cr.right - cr.left;
    int h = cr.bottom - cr.top;

    HDC wdc = GetDC(hwnd_);
    int phys_w = GetDeviceCaps(wdc, DESKTOPHORZRES);
    int phys_h = GetDeviceCaps(wdc, DESKTOPVERTRES);
    ReleaseDC(hwnd_, wdc);

    int desk_w = GetSystemMetrics(SM_CXSCREEN);
    int desk_h = GetSystemMetrics(SM_CYSCREEN);
    int newW = (desk_w > 0 && desk_h > 0) ? MulDiv(w, phys_w, desk_w) : w;
    int newH = (desk_w > 0 && desk_h > 0) ? MulDiv(h, phys_h, desk_h) : h;

    if (newW == res_.width && newH == res_.height) return false;
    if (newW < 640 || newH < 480) return false;

    res_.width  = newW;
    res_.height = newH;
    layout_ = selectStrategyLayout(newW, newH);
    computeRegions();
    LOG_INFO("[capture] Resolution changed: " << newW << "x" << newH);
    return true;
}

void Dota2Capture::computeRegions() {
    regions_.clear();
    const int W = res_.width;
    const int H = res_.height;
    const HudLayout& L = layout_;

    auto toX = [&](float f) { return static_cast<int>(f * W + 0.5f); };
    auto toY = [&](float f) { return static_cast<int>(f * H + 0.5f); };

    const float pwf = L.portrait_w;
    const float phf = L.portrait_h;
    const float pgf = L.portrait_gap;
    const float ryf = L.radiant_y_start;

    const float rxf = L.radiant_x_start;
    for (int i = 0; i < 5; ++i) {
        const float leftf   = rxf + i * (pwf + pgf);
        const float rightf  = leftf + pwf;
        PortraitRegion pr;
        pr.slot        = i;
        pr.rect.left   = toX(leftf);
        pr.rect.top    = toY(ryf);
        pr.rect.right  = toX(rightf);
        pr.rect.bottom = toY(ryf + phf);
        regions_.push_back(pr);
    }

    const float drxf = L.dire_x_start;
    for (int i = 0; i < 5; ++i) {
        const float leftf   = drxf + i * (pwf + pgf);
        const float rightf  = leftf + pwf;
        PortraitRegion pr;
        pr.slot        = 5 + i;
        pr.rect.left   = toX(leftf);
        pr.rect.top    = toY(ryf);
        pr.rect.right  = toX(rightf);
        pr.rect.bottom = toY(ryf + phf);
        regions_.push_back(pr);
    }

    computePosRegions();
}

void Dota2Capture::computePosRegions() {
    posRegions_.clear();
    const int W = res_.width;
    const int H = res_.height;
    const HudLayout& L = layout_;

    auto toX = [&](float f) { return static_cast<int>(f * W + 0.5f); };
    auto toY = [&](float f) { return static_cast<int>(f * H + 0.5f); };

    const float pw = L.pos_w;
    const float ph = L.pos_h;
    const float pg = L.pos_gap;
    const float py = L.pos_y;

    for (int i = 0; i < 5; ++i) {
        const float leftf  = L.pos_x_start + i * (pw + pg);
        PortraitRegion pr;
        pr.slot        = i;
        pr.rect.left   = toX(leftf);
        pr.rect.top    = toY(py);
        pr.rect.right  = toX(leftf + pw);
        pr.rect.bottom = toY(py + ph);
        posRegions_.push_back(pr);
    }
    for (int i = 0; i < 5; ++i) {
        const float leftf  = L.pos_x_start_dire + i * (pw + pg);
        PortraitRegion pr;
        pr.slot        = 5 + i;
        pr.rect.left   = toX(leftf);
        pr.rect.top    = toY(py);
        pr.rect.right  = toX(leftf + pw);
        pr.rect.bottom = toY(py + ph);
        posRegions_.push_back(pr);
    }
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

Bitmap Dota2Capture::captureWindow() {
    if (!IsWindow(hwnd_)) { hwnd_ = nullptr; return {}; }
    if (IsIconic(hwnd_)) return {};

    const int W = res_.width;
    const int H = res_.height;
    if (W <= 0 || H <= 0) return {};

    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);
    HBITMAP hBmp = CreateCompatibleBitmap(screenDC, W, H);
    HBITMAP hOld = static_cast<HBITMAP>(SelectObject(memDC, hBmp));

    const UINT flags = 0x00000001u | PW_RENDERFULLCONTENT;
    if (!PrintWindow(hwnd_, memDC, flags))
        PrintWindow(hwnd_, memDC, PW_RENDERFULLCONTENT);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi); bi.biWidth = W; bi.biHeight = -H;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;

    Bitmap result;
    result.width  = W;
    result.height = H;
    result.pixels.resize(static_cast<size_t>(W) * H * 4);
    GetDIBits(memDC, hBmp, 0, static_cast<UINT>(H),
              result.pixels.data(),
              reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    SelectObject(memDC, hOld);
    DeleteObject(hBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return result;
}

Bitmap Dota2Capture::captureFullWindow() { return captureWindow(); }

int Dota2Capture::capturePortraits() {
    if (!hwnd_) return 0;
    portraits_.clear();
    portraits_.resize(10);
    posPortraits_.clear();
    posPortraits_.resize(10);
    if (IsIconic(hwnd_)) return 0;
    const int W = res_.width, H = res_.height;
    if (W<=0||H<=0) return 0;

    HDC screenDC = GetDC(nullptr);
    HDC fullDC   = CreateCompatibleDC(screenDC);
    HBITMAP hFull    = CreateCompatibleBitmap(screenDC, W, H);
    HBITMAP hFullOld = static_cast<HBITMAP>(SelectObject(fullDC, hFull));

    const UINT flags = 0x00000001u | PW_RENDERFULLCONTENT;
    if (!PrintWindow(hwnd_, fullDC, flags))
        PrintWindow(hwnd_, fullDC, PW_RENDERFULLCONTENT);

    // Вырезка одного региона из fullDC в Bitmap
    auto extractRegion = [&](const PortraitRegion& reg, Bitmap& dst) {
        const RECT& r = reg.rect;
        const int pw = r.right-r.left, ph = r.bottom-r.top;
        if (pw<=0||ph<=0) return;

        HDC smallDC  = CreateCompatibleDC(screenDC);
        HBITMAP hBmp = CreateCompatibleBitmap(screenDC, pw, ph);
        HBITMAP hOld = static_cast<HBITMAP>(SelectObject(smallDC, hBmp));
        BitBlt(smallDC, 0, 0, pw, ph, fullDC, r.left, r.top, SRCCOPY);

        BITMAPINFOHEADER bi{};
        bi.biSize=sizeof(bi); bi.biWidth=pw; bi.biHeight=-ph;
        bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;

        dst.width=pw; dst.height=ph;
        dst.pixels.resize(static_cast<size_t>(pw)*ph*4);
        GetDIBits(smallDC,hBmp,0,static_cast<UINT>(ph),
                  dst.pixels.data(),
                  reinterpret_cast<BITMAPINFO*>(&bi),DIB_RGB_COLORS);

        SelectObject(smallDC,hOld); DeleteObject(hBmp); DeleteDC(smallDC);
    };

    int count = 0;
    for (const auto& reg : regions_) {
        extractRegion(reg, portraits_[reg.slot]);
        if (callback_) callback_(reg.slot, portraits_[reg.slot]);
        if (!portraits_[reg.slot].empty()) ++count;
    }

    for (const auto& reg : posRegions_) {
        extractRegion(reg, posPortraits_[reg.slot]);
    }

    SelectObject(fullDC,hFullOld); DeleteObject(hFull);
    DeleteDC(fullDC); ReleaseDC(nullptr,screenDC);
    return count;
}

bool Dota2Capture::saveBitmapAsPng(const Bitmap& bmp,
                                    const std::filesystem::path& path) const {
    if (bmp.empty()) return false;
    Gdiplus::Bitmap gdiBmp(bmp.width, bmp.height, PixelFormat32bppARGB);
    Gdiplus::BitmapData bdata{};
    Gdiplus::Rect lock_rect(0,0,bmp.width,bmp.height);
    gdiBmp.LockBits(&lock_rect,Gdiplus::ImageLockModeWrite,PixelFormat32bppARGB,&bdata);
    std::memcpy(bdata.Scan0,bmp.pixels.data(),
                static_cast<size_t>(bmp.width)*bmp.height*4);
    gdiBmp.UnlockBits(&bdata);
    CLSID pngClsid{};
    if (!getEncoderClsid(L"image/png",&pngClsid)) return false;
    std::wstring wpath = path.wstring();
    return gdiBmp.Save(wpath.c_str(),&pngClsid)==Gdiplus::Ok;
}

void Dota2Capture::savePortraits(const std::filesystem::path& dir) const {
    std::filesystem::path out = dir.empty() ? output_dir_ : dir;
    std::filesystem::create_directories(out);
    static const char* teamNames[] = {
        "radiant","radiant","radiant","radiant","radiant",
        "dire","dire","dire","dire","dire"
    };
    for (size_t i=0;i<portraits_.size();++i) {
        if (portraits_[i].empty()) continue;
        char name[64];
        std::snprintf(name,sizeof(name),"%s_%zu.png",teamNames[i],i);
        saveBitmapAsPng(portraits_[i],out/name);
    }
}

void Dota2Capture::saveDebugRegions(const std::filesystem::path& dir) const {
    std::filesystem::path out = dir.empty() ? output_dir_ : dir;
    std::filesystem::create_directories(out);
    static const char* teamNames[] = {
        "radiant","radiant","radiant","radiant","radiant",
        "dire","dire","dire","dire","dire"
    };
    for (size_t i=0;i<portraits_.size();++i) {
        if (portraits_[i].empty()) continue;
        char name[64];
        std::snprintf(name,sizeof(name),"%s_hero_%zu.png",teamNames[i], i % 5);
        saveBitmapAsPng(portraits_[i],out/name);
    }
    for (size_t i=0;i<posPortraits_.size();++i) {
        if (posPortraits_[i].empty()) continue;
        char name[64];
        std::snprintf(name,sizeof(name),"%s_pos_%zu.png",teamNames[i], i % 5);
        saveBitmapAsPng(posPortraits_[i],out/name);
    }
}

void Dota2Capture::runLoop(int interval_ms) {
    running_ = true;
    while (running_) {
        if (!hwnd_ || !IsWindow(hwnd_))
            findGameWindow();
        if (hwnd_) {
            int n = capturePortraits();
            if (n > 0) savePortraits();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}

} // namespace dota2