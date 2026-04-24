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

static const char* friendlyName(const std::string& stem) {
    static const struct { const char* k; const char* n; } MAP[] = {
        {"abaddon","Abaddon"},{"alchemist","Alchemist"},{"NULL","NULL"}
        {"ancient_apparition","Ancient Apparition"},
        {"anti_mage","Anti-Mage"},{"antimage","Anti-Mage"},
        {"arc_warden","Arc Warden"},{"axe","Axe"},{"bane","Bane"},
        {"batrider","Batrider"},{"beastmaster","Beastmaster"},
        {"bloodseeker","Bloodseeker"},{"bounty_hunter","Bounty Hunter"},
        {"brewmaster","Brewmaster"},{"bristleback","Bristleback"},
        {"broodmother","Broodmother"},{"centaur","Centaur Warrunner"},
        {"chaos_knight","Chaos Knight"},{"chen","Chen"},{"clinkz","Clinkz"},
        {"crystal_maiden","Crystal Maiden"},{"dark_seer","Dark Seer"},
        {"dark_willow","Dark Willow"},{"dawnbreaker","Dawnbreaker"},
        {"dazzle","Dazzle"},{"death_prophet","Death Prophet"},
        {"disruptor","Disruptor"},{"doom_bringer","Doom"},
        {"dragon_knight","Dragon Knight"},{"drow_ranger","Drow Ranger"},
        {"earth_spirit","Earth Spirit"},{"earthshaker","Earthshaker"},
        {"elder_titan","Elder Titan"},{"ember_spirit","Ember Spirit"},
        {"enchantress","Enchantress"},{"enigma","Enigma"},
        {"faceless_void","Faceless Void"},{"grimstroke","Grimstroke"},
        {"gyrocopter","Gyrocopter"},{"hoodwink","Hoodwink"},
        {"huskar","Huskar"},{"invoker","Invoker"},{"jakiro","Jakiro"},
        {"juggernaut","Juggernaut"},
        {"keeper_of_the_light","Keeper of the Light"},{"kunkka","Kunkka"},
        {"legion_commander","Legion Commander"},{"leshrac","Leshrac"},
        {"lich","Lich"},{"life_stealer","Lifestealer"},{"lina","Lina"},
        {"lion","Lion"},{"lone_druid","Lone Druid"},{"luna","Luna"},
        {"lycan","Lycan"},{"magnus","Magnus"},{"marci","Marci"},
        {"mars","Mars"},{"medusa","Medusa"},{"meepo","Meepo"},
        {"mirana","Mirana"},{"monkey_king","Monkey King"},
        {"morphling","Morphling"},{"muerta","Muerta"},
        {"naga_siren","Naga Siren"},{"natures_prophet","Nature's Prophet"},
        {"necrolyte","Necrophos"},{"nevermore","Shadow Fiend"},
        {"shadow_fiend","Shadow Fiend"},{"night_stalker","Night Stalker"},
        {"nyx_assassin","Nyx Assassin"},{"ogre_magi","Ogre Magi"},
        {"omniknight","Omniknight"},{"oracle","Oracle"},
        {"outworld_destroyer","Outworld Destroyer"},
        {"pangolier","Pangolier"},{"phantom_assassin","Phantom Assassin"},
        {"phantom_lancer","Phantom Lancer"},{"phoenix","Phoenix"},
        {"primal_beast","Primal Beast"},{"puck","Puck"},{"pudge","Pudge"},
        {"pugna","Pugna"},{"queen_of_pain","Queen of Pain"},
        {"queenofpain","Queen of Pain"},{"razor","Razor"},
        {"rattletrap","Clockwerk"},{"riki","Riki"},
        {"ringmaster","Ringmaster"},{"rubick","Rubick"},
        {"sand_king","Sand King"},{"shadow_demon","Shadow Demon"},
        {"shadow_shaman","Shadow Shaman"},{"silencer","Silencer"},
        {"skywrath_mage","Skywrath Mage"},{"slardar","Slardar"},
        {"slark","Slark"},{"snapfire","Snapfire"},{"sniper","Sniper"},
        {"spectre","Spectre"},{"spirit_breaker","Spirit Breaker"},
        {"storm_spirit","Storm Spirit"},{"sven","Sven"},
        {"techies","Techies"},{"templar_assassin","Templar Assassin"},
        {"terrorblade","Terrorblade"},{"tidehunter","Tidehunter"},
        {"tinker","Tinker"},{"tiny","Tiny"},
        {"treant","Treant Protector"},{"troll_warlord","Troll Warlord"},
        {"tusk","Tusk"},{"underlord","Underlord"},
        {"abyssal_underlord","Underlord"},{"undying","Undying"},
        {"ursa","Ursa"},{"vengefulspirit","Vengeful Spirit"},
        {"venomancer","Venomancer"},{"viper","Viper"},{"visage","Visage"},
        {"void_spirit","Void Spirit"},{"warlock","Warlock"},
        {"weaver","Weaver"},{"windrunner","Windranger"},
        {"winter_wyvern","Winter Wyvern"},{"witch_doctor","Witch Doctor"},
        {"wraith_king","Wraith King"},{"zeus","Zeus"},{"zuus","Zeus"},
    };
    for (const auto& e : MAP) if (stem == e.k) return e.n;
    return nullptr;
}

static std::vector<fs::path> findPortraits(const fs::path& root) {
    std::vector<fs::path> cands = {
        root/"game"/"dota"/"pak01_dir"/"panorama"/"images"/"heroes",
        root/"game"/"dota"/"panorama"/"images"/"heroes",
        root/"dota"/"resource"/"flash3"/"images"/"heroes",
    };
    for (const auto& p : cands) {
        if (!fs::exists(p)) continue;
        std::vector<fs::path> r;
        for (const auto& e : fs::directory_iterator(p)) {
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".png") r.push_back(e.path());
        }
        if (!r.empty()) {
            std::printf("Found %zu portraits in: %s\n", r.size(), p.string().c_str());
            return r;
        }
    }
    return {};
}

int main(int argc, char* argv[]) {
    Gdiplus::GdiplusStartupInput gi; ULONG_PTR gt;
    Gdiplus::GdiplusStartup(&gt, &gi, nullptr);

    std::string dota_root, custom_dir, output = "hero_hashes.h";

    CropRect crop = {0.0f, 0.0f, 1.0f, 1.0f};

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--dota" && i+1 < argc) dota_root  = argv[++i];
        else if (a == "--dir"  && i+1 < argc) custom_dir = argv[++i];
        else if (a == "--out"  && i+1 < argc) output     = argv[++i];
        else if (a == "--crop" && i+4 < argc) {
            crop.x0 = std::stof(argv[++i]);
            crop.y0 = std::stof(argv[++i]);
            crop.x1 = std::stof(argv[++i]);
            crop.y1 = std::stof(argv[++i]);
        }
        else if (a == "--help") {
            std::puts(
                "build_hero_db.exe  — generates hero_hashes.h\n"
                "\n"
                "RECOMMENDED: hash already-captured HUD portraits\n"
                "  1. Run dota2_portraits.exe --loop during a game\n"
                "  2. Rename saved PNGs to hero names:  anti_mage.png, axe.png ...\n"
                "  3. build_hero_db.exe --dir C:\\portraits\\\n"
                "\n"
                "FROM GAME FILES (with crop to match HUD region):\n"
                "  build_hero_db.exe --dota \"C:\\...\\dota 2 beta\" --crop 0 0 1 0.72\n"
                "  --crop x0 y0 x1 y1   fractions of source image (default: 0 0 1 1)\n"
                "\n"
                "OPTIONS:\n"
                "  --dota <path>   Dota 2 install root\n"
                "  --dir  <path>   folder with hero PNG files\n"
                "  --out  <file>   output filename (default: hero_hashes.h)\n"
                "  --crop x0 y0 x1 y1  crop before hashing (0..1 fractions)\n"
            );
            return 0;
        }
    }

    if (dota_root.empty() && custom_dir.empty()) {
        const char* paths[] = {
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\dota 2 beta",
            "C:\\Program Files\\Steam\\steamapps\\common\\dota 2 beta",
            "D:\\Steam\\steamapps\\common\\dota 2 beta",
            "D:\\SteamLibrary\\steamapps\\common\\dota 2 beta",
            nullptr
        };
        for (int i = 0; paths[i]; ++i) {
            if (fs::exists(paths[i])) {
                dota_root = paths[i];
                std::printf("Auto-detected: %s\n", dota_root.c_str());

                if (crop.x1 == 1.0f && crop.y1 == 1.0f) {
                    crop = {0.0f, 0.0f, 1.0f, 0.72f};
                    std::puts("Applying default HUD crop: top 72% of portrait");
                }
                break;
            }
        }
    }

    std::vector<fs::path> pngs;
    if (!custom_dir.empty()) {
        for (const auto& e : fs::directory_iterator(custom_dir)) {
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".png") pngs.push_back(e.path());
        }
        std::printf("Found %zu PNGs in %s\n", pngs.size(), custom_dir.c_str());
    } else if (!dota_root.empty()) {
        pngs = findPortraits(dota_root);
    }

    if (pngs.empty()) {
        std::fputs(
            "ERROR: no PNG files found.\n"
            "  See --help for usage.\n", stderr);
        return 1;
    }

    std::printf("Crop: x=%.2f..%.2f  y=%.2f..%.2f\n",
                crop.x0, crop.x1, crop.y0, crop.y1);

    struct Entry { std::string stem, display; Matrix8 mat; };
    std::vector<Entry> entries;

    for (const auto& p : pngs) {
        std::string stem = p.stem().string();
        std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);

        const char* disp = friendlyName(stem);
        std::string display = disp ? disp : stem;
        if (!display.empty())
            display[0] = static_cast<char>(toupper((unsigned char)display[0]));

        Matrix8 m = matrixPng(p, crop);
        if (m.empty()) {
            std::printf("  SKIP  %s  (load failed)\n", stem.c_str());
            continue;
        }
        entries.push_back({stem, display, m});
        std::printf("  OK    %s\n", display.c_str());
    }

    if (entries.empty()) {
        std::fputs("ERROR: nothing hashed.\n", stderr);
        return 1;
    }

    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b){ return a.display < b.display; });

    std::ofstream out(output);
    out << "#pragma once\n"
        << "// AUTO-GENERATED by build_hero_db.exe — " << entries.size() << " heroes\n"
        << "// Crop used: x=" << crop.x0 << ".." << crop.x1
        <<           "  y=" << crop.y0 << ".." << crop.y1 << "\n"
        << "#include \"dhash.h\"\n\n"
        << "namespace dota2 {\n\n"
        << "static const HeroHashEntry g_hero_db[] = {\n";

    for (const auto& e : entries) {
        out << "    { \"" << e.display << "\", { {";
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

    std::printf("\nWrote %zu hashes to %s\n", entries.size(), output.c_str());
    Gdiplus::GdiplusShutdown(gt);
    return 0;
}
