#include "dota2_capture.h"
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
        std::printf("  HWND=%p  class=%-30s  title=%s",
                    static_cast<void*>(hwnd), cls, title);
    return TRUE;
}

void ListAllWindows() {
    std::puts("--- Visible windows ---");
    EnumWindows(enumAllCallback, 0);
    std::puts("--- End ---");
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
        HDC wdc  = GetDC(hwnd_);
        HDC mdc  = CreateCompatibleDC(wdc);
        HBITMAP hb = CreateCompatibleBitmap(wdc, 1, 1);
        int phys_w = GetDeviceCaps(wdc, DESKTOPHORZRES);
        int phys_h = GetDeviceCaps(wdc, DESKTOPVERTRES);
        DeleteObject(hb);
        DeleteDC(mdc);

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

    layout_ = selectStrategyLayout(res_.width, res_.height);
    {
        float ratio = static_cast<float>(res_.width) / static_cast<float>(res_.height);
        const char* name =
            (ratio < 1.467f) ? "4:3" :
            (ratio < 1.689f) ? "16:10" :
            (ratio < 2.056f) ? "16:9" : "21:9";
        std::printf("[capture] Window %dx%d  ratio=%.3f  layout=%s\n",
                    res_.width, res_.height, ratio, name);
    }

    computeRegions();
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

static Bitmap captureRect(HWND hwnd, RECT r) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return {};
    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);
    HBITMAP hBmp = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP hOld = static_cast<HBITMAP>(SelectObject(memDC, hBmp));
    HDC tmpDC = CreateCompatibleDC(screenDC);
    RECT cr{}; GetClientRect(hwnd, &cr);
    HBITMAP hTmp    = CreateCompatibleBitmap(screenDC, cr.right, cr.bottom);
    HBITMAP hTmpOld = static_cast<HBITMAP>(SelectObject(tmpDC, hTmp));
    const UINT flags = 0x00000001u | PW_RENDERFULLCONTENT;
    if (!PrintWindow(hwnd, tmpDC, flags)) PrintWindow(hwnd, tmpDC, PW_RENDERFULLCONTENT);
    BitBlt(memDC, 0, 0, w, h, tmpDC, r.left, r.top, SRCCOPY);
    SelectObject(tmpDC, hTmpOld); DeleteObject(hTmp); DeleteDC(tmpDC);
    BITMAPINFOHEADER bi{};
    bi.biSize=sizeof(bi); bi.biWidth=w; bi.biHeight=-h;
    bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;
    Bitmap result; result.width=w; result.height=h;
    result.pixels.resize(static_cast<size_t>(w)*h*4);
    GetDIBits(memDC,hBmp,0,static_cast<UINT>(h),result.pixels.data(),
              reinterpret_cast<BITMAPINFO*>(&bi),DIB_RGB_COLORS);
    SelectObject(memDC,hOld); DeleteObject(hBmp); DeleteDC(memDC);
    ReleaseDC(nullptr,screenDC);
    return result;
}

Bitmap Dota2Capture::cropBitmap(const Bitmap& src, RECT r) const {
    r.left   = (std::max)(r.left,   static_cast<LONG>(0));
    r.top    = (std::max)(r.top,    static_cast<LONG>(0));
    r.right  = (std::min)(r.right,  static_cast<LONG>(src.width));
    r.bottom = (std::min)(r.bottom, static_cast<LONG>(src.height));
    const int cw = r.right-r.left, ch = r.bottom-r.top;
    if (cw<=0||ch<=0) return {};
    Bitmap dst; dst.width=cw; dst.height=ch;
    dst.pixels.resize(static_cast<size_t>(cw)*ch*4);
    for (int y=0;y<ch;++y) {
        const uint8_t* srcRow=src.pixels.data()+((r.top+y)*src.width+r.left)*4;
        std::memcpy(dst.pixels.data()+y*cw*4, srcRow, static_cast<size_t>(cw)*4);
    }
    return dst;
}

int Dota2Capture::capturePortraits() {
    if (!hwnd_) return 0;
    portraits_.clear();
    portraits_.resize(10);
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

    int count = 0;
    for (const auto& reg : regions_) {
        const RECT& r = reg.rect;
        const int pw = r.right-r.left, ph = r.bottom-r.top;
        if (pw<=0||ph<=0) continue;

        HDC smallDC  = CreateCompatibleDC(screenDC);
        HBITMAP hBmp = CreateCompatibleBitmap(screenDC, pw, ph);
        HBITMAP hOld = static_cast<HBITMAP>(SelectObject(smallDC, hBmp));
        BitBlt(smallDC, 0, 0, pw, ph, fullDC, r.left, r.top, SRCCOPY);

        BITMAPINFOHEADER bi{};
        bi.biSize=sizeof(bi); bi.biWidth=pw; bi.biHeight=-ph;
        bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;

        Bitmap& portrait = portraits_[reg.slot];
        portrait.width=pw; portrait.height=ph;
        portrait.pixels.resize(static_cast<size_t>(pw)*ph*4);
        GetDIBits(smallDC,hBmp,0,static_cast<UINT>(ph),
                  portrait.pixels.data(),
                  reinterpret_cast<BITMAPINFO*>(&bi),DIB_RGB_COLORS);

        SelectObject(smallDC,hOld); DeleteObject(hBmp); DeleteDC(smallDC);
        if (callback_) callback_(reg.slot, portrait);
        ++count;
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

}
