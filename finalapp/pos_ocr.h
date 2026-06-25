#pragma once
/*
 * pos_ocr.h — OCR-based position recognition (Windows OCR API / WinRT).
 *
 * Replaces the old Pearson-correlation PosRecognizer.
 * Reads text from captured position-indicator bitmaps (icon + label like
 * "Safe Lane" / "Лёгкая линия") and maps keywords to positions 1-5.
 */

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Security.Cryptography.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>
#include <memory>

#include "dhash.h"   // PosMatch

namespace dota2 {

namespace wf  = winrt::Windows::Foundation;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wmo = winrt::Windows::Media::Ocr;
namespace wgl = winrt::Windows::Globalization;
namespace wss = winrt::Windows::Storage::Streams;

// ─── Bilinear upscale (BGRA) ────────────────────────────────────────────────

static std::vector<uint8_t> upscaleBgra(const uint8_t* src, int sw, int sh, int scale) {
    int dw = sw * scale, dh = sh * scale;
    std::vector<uint8_t> dst(static_cast<size_t>(dw) * dh * 4);
    for (int dy = 0; dy < dh; ++dy) {
        float sy = (dy + 0.5f) * sh / dh - 0.5f;
        int y0 = (std::max)(0, (int)sy);
        int y1 = (std::min)(sh - 1, y0 + 1);
        float fy = sy - y0;
        for (int dx = 0; dx < dw; ++dx) {
            float sx = (dx + 0.5f) * sw / dw - 0.5f;
            int x0 = (std::max)(0, (int)sx);
            int x1 = (std::min)(sw - 1, x0 + 1);
            float fx = sx - x0;
            for (int c = 0; c < 4; ++c) {
                float v = src[(y0*sw+x0)*4+c]*(1-fy)*(1-fx)
                        + src[(y0*sw+x1)*4+c]*(1-fy)*fx
                        + src[(y1*sw+x0)*4+c]*fy*(1-fx)
                        + src[(y1*sw+x1)*4+c]*fy*fx;
                dst[(dy*dw+dx)*4+c] = (uint8_t)(std::min)(255.f, (std::max)(0.f, v + 0.5f));
            }
        }
    }
    return dst;
}

// ─── Keyword → position mapping ─────────────────────────────────────────────

static std::wstring toLowerW(const std::wstring& s) {
    std::wstring r = s;
    for (auto& ch : r) ch = towlower(ch);
    return r;
}

static bool containsW(const std::wstring& hay, const wchar_t* needle) {
    return hay.find(needle) != std::wstring::npos;
}

static int textToPosition(const std::wstring& raw) {
    std::wstring t = toLowerW(raw);

    // EN keywords (unambiguous prefixes)
    if (containsW(t, L"safe"))  return 1;
    if (containsW(t, L"mid"))   return 2;
    if (containsW(t, L"off"))   return 3;
    if (containsW(t, L"soft"))  return 4;
    if (containsW(t, L"hard"))  return 5;

    // EN fallback: "support" without soft/hard qualifier → pos 4
    if (containsW(t, L"supp"))  return 4;
    // EN fallback: "lane" without safe/mid/off → pos 1
    if (containsW(t, L"lane"))  return 1;

    // RU keywords
    bool hasSupport = containsW(t, L"поддерж");

    if (containsW(t, L"центр") || containsW(t, L"мид"))
        return 2;

    if (containsW(t, L"сложн"))
        return 3;

    if (containsW(t, L"полн") && hasSupport)
        return 5;

    if (containsW(t, L"жёстк") || containsW(t, L"жестк"))
        return 5;

    if (containsW(t, L"лёгк") || containsW(t, L"легк")) {
        if (hasSupport) return 4;
        return 1;
    }

    if (containsW(t, L"мягк"))
        return 4;

    if (hasSupport) return 4;

    return 0;
}

// ─── PosOcrRecognizer ───────────────────────────────────────────────────────

class PosOcrRecognizer {
public:
    PosOcrRecognizer() {
        try {
            enEngine_ = wmo::OcrEngine::TryCreateFromLanguage(wgl::Language(L"en-US"));
        } catch (...) {}
        try {
            ruEngine_ = wmo::OcrEngine::TryCreateFromLanguage(wgl::Language(L"ru"));
        } catch (...) {}
        if (!enEngine_ && !ruEngine_) {
            try {
                enEngine_ = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
            } catch (...) {}
        }
        std::printf("[pos_ocr] engines: en=%s ru=%s\n",
                    enEngine_ ? "OK" : "N/A",
                    ruEngine_ ? "OK" : "N/A");
    }

    bool isAvailable() const { return enEngine_ || ruEngine_; }

    PosMatch recognize(const uint8_t* bgra, int w, int h) const {
        if (!isAvailable() || w <= 0 || h <= 0) return {0, 0.f};

        constexpr int SCALE = 4;
        auto upscaled = upscaleBgra(bgra, w, h, SCALE);
        int uw = w * SCALE, uh = h * SCALE;

        auto bmp = bgraToSoftwareBitmap(upscaled.data(), uw, uh);
        if (!bmp) return {0, 0.f};

        if (enEngine_) {
            auto result = tryOcr(enEngine_, bmp);
            if (result.pos > 0) return result;
        }
        if (ruEngine_) {
            auto result = tryOcr(ruEngine_, bmp);
            if (result.pos > 0) return result;
        }
        return {0, 0.f};
    }

    template<typename BitmapT>
    PosMatch recognize(const BitmapT& bmp) const {
        if (bmp.empty()) return {0, 0.f};
        return recognize(bmp.pixels.data(), bmp.width, bmp.height);
    }

private:
    wgi::SoftwareBitmap bgraToSoftwareBitmap(const uint8_t* data, int w, int h) const {
        try {
            uint32_t bufSize = static_cast<uint32_t>(w) * h * 4;
            wss::DataWriter writer;
            writer.WriteBytes(winrt::array_view<const uint8_t>(data, data + bufSize));
            wss::IBuffer buf = writer.DetachBuffer();

            return wgi::SoftwareBitmap::CreateCopyFromBuffer(
                buf, wgi::BitmapPixelFormat::Bgra8, w, h,
                wgi::BitmapAlphaMode::Premultiplied);
        } catch (...) {
            return nullptr;
        }
    }

    PosMatch tryOcr(const wmo::OcrEngine& engine, const wgi::SoftwareBitmap& bmp) const {
        try {
            auto ocrResult = engine.RecognizeAsync(bmp).get();
            auto text = ocrResult.Text();
            if (text.empty()) return {0, 0.f};

            int pos = textToPosition(std::wstring(text.c_str()));
            if (pos > 0) return {pos, 1.0f};
        } catch (...) {}
        return {0, 0.f};
    }

    wmo::OcrEngine enEngine_{nullptr};
    wmo::OcrEngine ruEngine_{nullptr};
};

} // namespace dota2
