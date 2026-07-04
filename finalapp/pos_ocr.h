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
#include "common.h"  // LOG_INFO

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

    // Ниже — блоки для UK/BE/KK/DE/PL/FR/ES. В каждом только те корни, которые
    // ДЕЙСТВИТЕЛЬНО не перехватываются уже отработавшими блоками выше (напр.
    // "легк"/"лёгк"/"центр" для UK/BE не дублируются — их уже ловит RU-блок
    // благодаря общим славянским корням) — иначе получились бы недостижимые ветки.

    // UK (украинский)
    bool ukSupport = containsW(t, L"підтримк");
    if (containsW(t, L"мід"))
        return 2;
    if (containsW(t, L"складн"))
        return 3;
    if (containsW(t, L"жорстк"))
        return 5;
    if (containsW(t, L"м'як") || containsW(t, L"мяк"))
        return 4;
    if (ukSupport) return 4;

    // BE (белорусский)
    bool beSupport = containsW(t, L"падтрымк");
    if (containsW(t, L"складан"))
        return 3;
    if (containsW(t, L"цвёрд"))
        return 5;
    if (beSupport) return 4;

    // KK (казахский)
    bool kkSupport = containsW(t, L"қолдау");
    if (containsW(t, L"орталық"))
        return 2;
    if (containsW(t, L"қиын"))
        return 3;
    if (containsW(t, L"қатаң"))
        return 5;
    if (containsW(t, L"жеңіл"))
        return 1;
    if (containsW(t, L"жұмсақ"))
        return 4;
    if (kkSupport) return 4;

    // DE (немецкий)
    bool deSupport = containsW(t, L"unterst");
    if (containsW(t, L"mitte") || containsW(t, L"mittl"))
        return 2;
    if (containsW(t, L"schwer") || containsW(t, L"schwierig"))
        return 3;
    if (containsW(t, L"hart"))
        return 5;
    if (containsW(t, L"sicher"))
        return deSupport ? 4 : 1;
    if (containsW(t, L"sanft") || containsW(t, L"weich"))
        return 4;
    if (deSupport) return 4;

    // PL (польский)
    bool plSupport = containsW(t, L"wsparci");
    if (containsW(t, L"środk") || containsW(t, L"srodk"))
        return 2;
    if (containsW(t, L"trudn"))
        return 3;
    if (containsW(t, L"tward"))
        return 5;
    if (containsW(t, L"bezpieczn"))
        return plSupport ? 4 : 1;
    if (containsW(t, L"miękk") || containsW(t, L"miekk"))
        return 4;
    if (plSupport) return 4;

    // FR (французский)
    bool frSupport = containsW(t, L"soutien");
    if (containsW(t, L"milieu"))
        return 2;
    if (containsW(t, L"difficile"))
        return 3;
    // "dur" короткое и само по себе неоднозначное — учитываем только вместе с "soutien"
    if (containsW(t, L"renforc") || (frSupport && containsW(t, L"dur")))
        return 5;
    if (containsW(t, L"sûre") || containsW(t, L"sure"))
        return frSupport ? 4 : 1;
    if (containsW(t, L"souple") || containsW(t, L"léger") || containsW(t, L"leger"))
        return 4;
    if (frSupport) return 4;

    // ES (испанский)
    bool esSupport = containsW(t, L"apoyo");
    if (containsW(t, L"medi"))
        return 2;
    if (containsW(t, L"dific") || containsW(t, L"difíc"))
        return 3;
    if (containsW(t, L"duro"))
        return 5;
    if (containsW(t, L"segur"))
        return esSupport ? 4 : 1;
    if (containsW(t, L"suave"))
        return 4;
    if (esSupport) return 4;

    return 0;
}

// ─── PosOcrRecognizer ───────────────────────────────────────────────────────

class PosOcrRecognizer {
public:
    // Порядок — от языков с реальной локализацией Dota 2/наибольшей вероятностью
    // установленного OCR-пакета к менее вероятным. TryCreateFromLanguage не бросает
    // и возвращает nullptr, если языковой OCR-пакет не установлен в Windows — так что
    // отсутствующие языки просто выпадают из списка ниже, без ошибок.
    PosOcrRecognizer() {
        static const wchar_t* kLangs[] = {
            L"en-US", L"ru", L"uk", L"be", L"kk", L"de", L"pl", L"fr", L"es"
        };
        for (auto lang : kLangs) {
            wmo::OcrEngine eng{nullptr};
            try {
                eng = wmo::OcrEngine::TryCreateFromLanguage(wgl::Language(lang));
            } catch (...) {}
            if (eng) engines_.push_back(eng);
        }
        if (engines_.empty()) {
            try {
                auto eng = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
                if (eng) engines_.push_back(eng);
            } catch (...) {}
        }
        LOG_INFO("[pos_ocr] engines available: " << engines_.size()
                 << "/" << (sizeof(kLangs)/sizeof(kLangs[0])));
    }

    bool isAvailable() const { return !engines_.empty(); }

    PosMatch recognize(const uint8_t* bgra, int w, int h) const {
        if (!isAvailable() || w <= 0 || h <= 0) return {0, 0.f};

        constexpr int SCALE = 4;
        auto upscaled = upscaleBgra(bgra, w, h, SCALE);
        int uw = w * SCALE, uh = h * SCALE;

        auto bmp = bgraToSoftwareBitmap(upscaled.data(), uw, uh);
        if (!bmp) return {0, 0.f};

        for (auto& engine : engines_) {
            auto result = tryOcr(engine, bmp);
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

    std::vector<wmo::OcrEngine> engines_;
};

} // namespace dota2
