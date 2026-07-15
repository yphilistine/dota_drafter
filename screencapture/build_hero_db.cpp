#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include "dhash.h"

#include <cstdio>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cctype>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace fs = std::filesystem;
using namespace dota2;

struct CropRect { float x0, y0, x1, y1; };

static Matrix8 matrixPng(const fs::path& path, CropRect crop) {
    Gdiplus::Bitmap src(path.wstring().c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return {};

    const int sw = static_cast<int>(src.GetWidth());
    const int sh = static_cast<int>(src.GetHeight());
    if (sw <= 0 || sh <= 0) return {};

    int cx0 = static_cast<int>(crop.x0 * sw);
    int cy0 = static_cast<int>(crop.y0 * sh);
    int cx1 = static_cast<int>(crop.x1 * sw);
    int cy1 = static_cast<int>(crop.y1 * sh);
    cx0 = (std::max)(0,  (std::min)(cx0, sw));
    cy0 = (std::max)(0,  (std::min)(cy0, sh));
    cx1 = (std::max)(cx0+1, (std::min)(cx1, sw));
    cy1 = (std::max)(cy0+1, (std::min)(cy1, sh));
    const int cw = cx1 - cx0, ch = cy1 - cy0;

    Gdiplus::Bitmap dst(cw, ch, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.DrawImage(&src, Gdiplus::Rect(0,0,cw,ch), cx0,cy0,cw,ch, Gdiplus::UnitPixel);
    }

    Gdiplus::BitmapData bd{};
    Gdiplus::Rect lr(0, 0, cw, ch);
    dst.LockBits(&lr, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd);
    Matrix8 m = computeMatrix(reinterpret_cast<const uint8_t*>(bd.Scan0), cw, ch);
    dst.UnlockBits(&bd);
    return m;
}


static const char* canonicalHeroName(const std::string& stem) {
    std::string s = stem;
    s.pop_back();
    if(s.find("null")!= std::string::npos) return "null";
    return s.c_str();
}

int main(int argc, char* argv[]) {
    Gdiplus::GdiplusStartupInput gi; ULONG_PTR gt;
    Gdiplus::GdiplusStartup(&gt, &gi, nullptr);

    std::string custom_dir, output = "hero_hashes.dat";

    CropRect crop = {0.0f, 0.0f, 1.0f, 1.0f};

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--dir"  && i+1 < argc) custom_dir = argv[++i];
        else if (a == "--out"  && i+1 < argc) output     = argv[++i];
        else if (a == "--crop" && i+4 < argc) {
            crop.x0 = std::stof(argv[++i]);
            crop.y0 = std::stof(argv[++i]);
            crop.x1 = std::stof(argv[++i]);
            crop.y1 = std::stof(argv[++i]);
        }
        else if (a == "--help") {
            std::puts(
                "build_hero_db.exe  — generates hero_hashes.dat\n"
                "\n"
                "  1. Run dota2_portraits.exe --loop during a game\n"
                "  2. Rename saved PNGs to hero names:  anti_mage.png, axe.png ...\n"
                "  3. build_hero_db.exe --dir C:\\portraits\\\n"
                "\n"
                "OPTIONS:\n"
                "  --dir  <path>   folder with hero PNG files\n"
                "  --out  <file>   output filename (default: hero_hashes.dat)\n"
                "  --crop x0 y0 x1 y1  crop before hashing (0..1 fractions)\n"
            );
            return 0;
        }
    }

    if (custom_dir.empty()) {
        std::fputs(
            "ERROR: --dir is required.\n"
            "  See --help for usage.\n", stderr);
        return 1;
    }

    std::vector<fs::path> pngs;
    for (const auto& e : fs::directory_iterator(custom_dir)) {
        auto ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".png") pngs.push_back(e.path());
    }
    std::printf("Found %zu PNGs in %s\n", pngs.size(), custom_dir.c_str());

    if (pngs.empty()) {
        std::fputs(
            "ERROR: no PNG files found.\n"
            "  See --help for usage.\n", stderr);
        return 1;
    }

    std::printf("Crop: x=%.2f..%.2f  y=%.2f..%.2f\n",
                crop.x0, crop.x1, crop.y0, crop.y1);

    // key: каноническое имя героя (совпадает с heroes.name без префикса
    // npc_dota_hero_) — используется рантаймом для сопоставления, стабильно
    // независимо от localized_name.
    struct Entry { std::string key; Matrix8 mat; };
    std::vector<Entry> entries;

    for (const auto& p : pngs) {
        std::string stem = p.stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);

        const char* canon = canonicalHeroName(stem);
        std::string key = canon ? canon : stem;

        Matrix8 m = matrixPng(p, crop);
        if (m.empty()) {
            std::printf("  SKIP  %s  (load failed)\n", stem.c_str());
            continue;
        }
        entries.push_back({key, m});
        std::printf("  OK    %s  ->  %s\n", stem.c_str(), key.c_str());
    }

    if (entries.empty()) {
        std::fputs("ERROR: nothing hashed.\n", stderr);
        return 1;
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b){ return a.key < b.key; });

    // ── .dat (бинарный, для рантайма) ──
    {
        std::string datPath = output;
        auto dot = datPath.rfind('.');
        if (dot != std::string::npos) datPath = datPath.substr(0, dot);
        datPath += ".dat";

        std::ofstream out(datPath, std::ios::binary);
        uint32_t count = static_cast<uint32_t>(entries.size());
        out.write(reinterpret_cast<const char*>(&count), 4);
        for (const auto& e : entries) {
            uint16_t nameLen = static_cast<uint16_t>(e.key.size());
            out.write(reinterpret_cast<const char*>(&nameLen), 2);
            out.write(e.key.data(), nameLen);
            out.write(reinterpret_cast<const char*>(e.mat.v), 64 * sizeof(float));
        }
        out.close();
        std::printf("\nWrote %zu hashes to %s\n", entries.size(), datPath.c_str());
    }

    // ── .h (C++ header, для компиляции) ──
    {
        std::string hPath = output;
        auto dot = hPath.rfind('.');
        if (dot != std::string::npos) hPath = hPath.substr(0, dot);
        hPath += ".h";

        std::ofstream out(hPath);
        out << "#pragma once\n"
            << "#include \"dhash.h\"\n\n"
            << "namespace dota2 {\n\n"
            << "static const HeroHashEntry g_hero_db[] = {\n";
        for (const auto& e : entries) {
            out << "    { \"" << e.key << "\", { {";
            for (int i = 0; i < 64; ++i) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.6ff", e.mat.v[i]);
                out << buf;
                if (i < 63) out << ",";
            }
            out << "} } },\n";
        }
        out << "};\n\n"
            << "static constexpr size_t g_hero_db_size =\n"
            << "    sizeof(g_hero_db) / sizeof(g_hero_db[0]);\n\n"
            << "} // namespace dota2\n";
        out.close();
        std::printf("Wrote %zu hashes to %s\n", entries.size(), hPath.c_str());
    }
    Gdiplus::GdiplusShutdown(gt);
    return 0;
}
