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

// Отображает вариант имени файла портрета (engine shortname, каким его
// сохранила Dota/сам пайплайн капчура) на канонический идентификатор героя —
// суффикс поля heroes.name (npc_dota_hero_<suffix>), стабильный ключ, который
// не зависит от localized_name. localized_name Valve иногда временно/некорректно
// меняет (напр. в живой БД встречалось "Axe?" вместо "Axe"), из-за чего
// сопоставление по display-имени ломалось. Несколько исходных вариантов файла
// могут указывать на одного героя (алиасы вроде anti_mage/antimage) — тогда
// они схлопываются в один и тот же суффикс. "null" — не герой, а сентинел
// пустого слота выбора (null_*/event_null* варианты заглушки).
static const char* canonicalHeroName(const std::string& stem) {
    static const struct { const char* k; const char* n; } MAP[] = {
        {"pudge2","pudge"},
        {"nevermore1","nevermore"},{"drow_ranger1","drow_ranger"},{"lion1","lion"},{"juggernaut1","juggernaut"},
        {"abaddon","abaddon"},{"alchemist","alchemist"},{"NULL","null"},{"wisp","wisp"},
        {"ancient_apparition","ancient_apparition"},{"kez","kez"},{"largo","largo"},
        {"anti_mage","antimage"},{"antimage","antimage"}, {"timbersaw","shredder"},
        {"arc_warden","arc_warden"},{"axe","axe"},{"bane","bane"},
        {"batrider","batrider"},{"beastmaster","beastmaster"},
        {"bloodseeker","bloodseeker"},{"bounty_hunter","bounty_hunter"},
        {"brewmaster","brewmaster"},{"bristleback","bristleback"},
        {"broodmother","broodmother"},{"centaur","centaur"},
        {"chaos_knight","chaos_knight"},{"chen","chen"},{"clinkz","clinkz"},
        {"crystal_maiden","crystal_maiden"},{"dark_seer","dark_seer"},
        {"dark_willow","dark_willow"},{"dawnbreaker","dawnbreaker"},
        {"dazzle","dazzle"},{"death_prophet","death_prophet"},
        {"disruptor","disruptor"},{"doom_bringer","doom_bringer"},
        {"dragon_knight","dragon_knight"},{"drow_ranger","drow_ranger"},
        {"earth_spirit","earth_spirit"},{"earthshaker","earthshaker"},
        {"elder_titan","elder_titan"},{"ember_spirit","ember_spirit"},
        {"enchantress","enchantress"},{"enigma","enigma"},
        {"faceless_void","faceless_void"},{"grimstroke","grimstroke"},
        {"gyrocopter","gyrocopter"},{"hoodwink","hoodwink"},
        {"huskar","huskar"},{"invoker","invoker"},{"jakiro","jakiro"},
        {"juggernaut","juggernaut"},
        {"keeper_of_the_light","keeper_of_the_light"},{"kunkka","kunkka"},
        {"legion_commander","legion_commander"},{"leshrac","leshrac"},
        {"lich","lich"},{"life_stealer","life_stealer"},{"lina","lina"},
        {"lion","lion"},{"lone_druid","lone_druid"},{"luna","luna"},
        {"lycan","lycan"},{"magnus","magnataur"},{"marci","marci"},
        {"mars","mars"},{"medusa","medusa"},{"meepo","meepo"},{"mirana1","mirana"},
        {"mirana","mirana"},{"monkey_king","monkey_king"},{"monkey_king1","monkey_king"},
        {"morphling","morphling"},{"muerta","muerta"},
        {"naga_siren","naga_siren"},{"natures_prophet","furion"},
        {"necrolyte","necrolyte"},{"nevermore","nevermore"},
        {"shadow_fiend","nevermore"},{"night_stalker","night_stalker"},
        {"nyx_assassin","nyx_assassin"},{"ogre_magi","ogre_magi"},
        {"omniknight","omniknight"},{"oracle","oracle"},
        {"outworld_destroyer","obsidian_destroyer"},
        {"pangolier","pangolier"},{"phantom_assassin","phantom_assassin"},
        {"phantom_lancer","phantom_lancer"},{"phoenix","phoenix"},
        {"primal_beast","primal_beast"},{"puck","puck"},{"pudge","pudge"},{"pudge1","pudge"},
        {"pugna","pugna"},{"queen_of_pain","queenofpain"},
        {"queenofpain","queenofpain"},{"razor","razor"},
        {"rattletrap","rattletrap"},{"riki","riki"},
        {"ringmaster","ringmaster"},{"rubick","rubick"},
        {"sand_king","sand_king"},{"shadow_demon","shadow_demon"},
        {"shadow_shaman","shadow_shaman"},{"silencer","silencer"},
        {"skywrath_mage","skywrath_mage"},{"slardar","slardar"},
        {"slark","slark"},{"snapfire","snapfire"},{"sniper","sniper"},
        {"spectre","spectre"},{"spirit_breaker","spirit_breaker"},
        {"storm_spirit","storm_spirit"},{"sven","sven"},
        {"techies","techies"},{"templar_assassin","templar_assassin"},
        {"terrorblade","terrorblade"},{"tidehunter","tidehunter"},{"terrorblade1","terrorblade"},
        {"tinker","tinker"},{"tiny","tiny"},
        {"treant","treant"},{"troll_warlord","troll_warlord"},
        {"tusk","tusk"},{"underlord","abyssal_underlord"},
        {"abyssal_underlord","abyssal_underlord"},{"undying","undying"},
        {"ursa","ursa"},{"vengefulspirit","vengefulspirit"},
        {"venomancer","venomancer"},{"viper","viper"},{"visage","visage"},
        {"void_spirit","void_spirit"},{"warlock","warlock"},
        {"weaver","weaver"},{"windrunner","windrunner"},
        {"winter_wyvern","winter_wyvern"},{"witch_doctor","witch_doctor"},
        {"wraith_king","skeleton_king"},{"zeus","zuus"},{"zuus","zuus"}, {"null_abaddon","null"},{"null_alchemist","null"},
        {"event_null1","null"},{"event_null2","null"},{"event_null3","null"},
        {"event_null4","null"},{"event_null5","null"},{"event_null6","null"},
        {"event_null7","null"},{"event_null8","null"},{"event_null9","null"},
        {"event_null10","null"},{"event_null11","null"},{"event_null12","null"},
        {"event_null13","null"},{"event_null14","null"},{"event_null15","null"},
        {"event_null16","null"},{"event_null17","null"},{"event_null18","null"},
        {"event_null19","null"},{"event_null20","null"},{"null_abaddon","null"},
        {"null_alchemist","null"},
        {"null_ancient_apparition","null"},
        {"null_anti_mage","null"},
        {"null_arc_warden","null"},{"null_axe","null"},{"null_bane","null"},
        {"null_batrider","null"},{"null_beastmaster","null"},
        {"null_bloodseeker","null"},{"null_bounty_hunter","null"},
        {"null_brewmaster","null"},{"null_bristleback","null"},
        {"null_broodmother","null"},{"null_centaur","null"},
        {"null_chaos_knight","null"},{"null_chen","null"},{"null_clinkz","null"},
        {"null_crystal_maiden","null"},{"null_dark_seer","null"},
        {"null_dark_willow","null"},{"null_dawnbreaker","null"},
        {"null_dazzle","null"},{"null_death_prophet","null"},
        {"null_disruptor","null"},{"null_doom_bringer","null"},
        {"null_dragon_knight","null"},{"null_drow_ranger","null"},
        {"null_earth_spirit","null"},{"null_earthshaker","null"},
        {"null_elder_titan","null"},{"null_ember_spirit","null"},
        {"null_enchantress","null"},{"null_enigma","null"},
        {  "null_faceless_void","null"},{"null_grimstroke","null"},
        {"null_gyrocopter","null"},{"null_hoodwink","null"},
        {"null_huskar","null"},{"null_invoker","null"},
        {"null_wisp","null"},
        {"null_jakiro","null"},
        {"null_juggernaut","null"},
        {"null_keeper_of_the_light","null"},
        {"null_kez","null"},
        {"null_kunkka","null"},
        {"null_largo","null"},
        {"null_legion_commander","null"},{"null_leshrac","null"},
        {"null_lich","null"},{"null_life_stealer","null"},{"null_lina","null"},
        {"null_lion","null"},{"null_lone_druid","null"},{"null_luna","null"},
        {"null_lycan","null"},{"null_magnus","null"},{"null_marci","null"},
        {"null_mars","null"},{"null_medusa","null"},{"null_meepo","null"},
        {"null_mirana","null"},{"null_monkey_king","null"},
        {"null_morphling","null"},{"null_muerta","null"},
        {"null_naga_siren","null"},{"null_natures_prophet","null"},
        {"null_necrolyte","null"},
        {"null_shadow_fiend","null"},{"null_night_stalker","null"},
        {"null_nyx_assassin","null"},{"null_ogre_magi","null"},
        {"null_omniknight","null"},{"null_oracle","null"},
        {"null_outworld_destroyer","null"},
        {"null_pangolier","null"},{"null_phantom_assassin","null"},
        {"null_phantom_lancer","null"},{"null_phoenix","null"},
        {"null_primal_beast","null"},{"null_puck","null"},{"null_pudge","null"},
        {"null_pugna","null"},{"null_queen_of_pain","null"},
        {"null_razor","null"},{"null_rattletrap","null"},{"null_riki","null"},
        {"null_ringmaster","null"},{"null_rubick","null"},
        {"null_sand_king","null"},{"null_shadow_demon","null"},
        {"null_shadow_shaman","null"},{"null_silencer","null"},
        {"null_skywrath_mage","null"},{"null_slardar","null"},
        {"null_slark","null"},{"null_snapfire","null"},{"null_sniper","null"},
        {"null_spectre","null"},{"null_spirit_breaker","null"},
        {"null_storm_spirit","null"},{"null_sven","null"},
        {"null_techies","null"},{"null_templar_assassin","null"},
        {"null_terrorblade","null"},{"null_tidehunter","null"}, {"null_timbersaw","null"},
        {"null_tinker","null"},{"null_tiny","null"},
        {"null_treant","null"},{"null_troll_warlord","null"},
        {"null_tusk","null"},{"null_underlord","null"},
        {"null_undying","null"},{"null_ursa","null"},{"null_vengefulspirit","null"},
        {"null_venomancer","null"},{"null_viper","null"},{"null_visage","null"},
        {"null_void_spirit","null"},{"null_warlock","null"},
        {"null_weaver","null"},{"null_windrunner","null"},
        {"null_winter_wyvern","null"},{"null_witch_doctor","null"},
        {"null_wraith_king","null"},{"null_zeus","null"}
    };
    for (const auto& e : MAP) if (stem == e.k) return e.n;
    return nullptr;
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
