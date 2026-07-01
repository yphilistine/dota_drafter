/*
 * mainGUI.cpp — ImGui D3D11 GUI + фоновый оркестратор (фазы 1-3).
 *
 * Потоковая модель:
 *   GUI-поток      — WinMain / message loop / ImGui render
 *   Оркестратор    — управление потоками, portrait→GUI sync, one-shot inference
 *   GSI-поток      — runGsiServer (постоянно)
 *   Фаза 1a        — runDataFetcherInit (при старте, без accountId)
 *   Фаза 1b        — runDataFetcher (при вводе Friend ID)
 *   Portrait        — runPortraitCapture (HERO_SELECTION + 5с хвост, без accountId)
 *   Picker          — runPickerGui (HERO_SELECTION/DRAFT + 5с хвост, требует accountId)
 */

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <gdiplus.h>

#include "shared_types.h"
#include "common.h"
#include "playerdatafetcher.h"
#include "portrait_runner.h"
#include "version.h"
#include "version_utils.h"
#include "updater.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstring>
#include <cctype>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

// ─── D3D11 инициализация ──────────────────────────────────────────────────────
static ID3D11Device*           g_Device    = nullptr;
static ID3D11DeviceContext*    g_Context   = nullptr;
static IDXGISwapChain*         g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV       = nullptr;
static HWND                    g_Hwnd      = nullptr;

static void RenderFrame();   // определена ниже; нужна раньше для WM_SIZE
static void PresentFrame();  // определена ниже; форс-рендер во время live-resize

static void CreateRTV() {
    ID3D11Texture2D* back = nullptr;
    g_SwapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_Device->CreateRenderTargetView(back, nullptr, &g_RTV);
    back->Release();
}
static bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = hwnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL lvl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &sd, &g_SwapChain, &g_Device, &lvl, &g_Context)))
        return false;
    CreateRTV();
    return true;
}
static void CleanupD3D() {
    if (g_RTV)       { g_RTV->Release();      g_RTV       = nullptr; }
    if (g_SwapChain) { g_SwapChain->Release(); g_SwapChain = nullptr; }
    if (g_Context)   { g_Context->Release();   g_Context   = nullptr; }
    if (g_Device)    { g_Device->Release();    g_Device    = nullptr; }
}
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_Device && wp != SIZE_MINIMIZED) {
            if (g_RTV) { g_RTV->Release(); g_RTV = nullptr; }
            g_SwapChain->ResizeBuffers(0,LOWORD(lp),HIWORD(lp),DXGI_FORMAT_UNKNOWN,0);
            CreateRTV();
            // Живой рендер сразу при изменении размера: во время интерактивного
            // resize (тянем рамку окна) Windows крутит собственный модальный цикл
            // сообщений внутри DefWindowProc и не возвращается в наш while(PeekMessage)
            // до отпускания мыши. Без этого кадр не обновляется, и DWM растягивает
            // старый бэкбуфер под новый размер — визуально это выглядит как смазанные
            // полосы (особенно заметно при растягивании окна за левый край, когда DWM
            // ещё и сдвигает/масштабирует всю поверхность).
            if (ImGui::GetCurrentContext() && LOWORD(lp) > 0 && HIWORD(lp) > 0)
                PresentFrame();
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

// ─── Палитра ─────────────────────────────────────────────────────────────────
static const ImVec4 kBg     = {0.04f, 0.04f, 0.04f, 1.f};
static const ImVec4 kCard   = {0.09f, 0.09f, 0.09f, 1.f};
static const ImVec4 kCard2  = {0.15f, 0.15f, 0.15f, 1.f};
static const ImVec4 kText   = {0.98f, 0.98f, 0.98f, 1.f};
static const ImVec4 kMuted  = {0.63f, 0.63f, 0.63f, 1.f};
static const ImVec4 kBorder = {1.f,   1.f,   1.f,   0.10f};
static const ImVec4 kGreen  = {0.39f, 0.78f, 0.47f, 1.f};
static const ImVec4 kRed    = {0.88f, 0.45f, 0.35f, 1.f};
static const ImVec4 kAmber  = {0.88f, 0.73f, 0.35f, 1.f};

inline ImU32 C(ImVec4 v)           { return ImGui::ColorConvertFloat4ToU32(v); }
inline ImU32 Ca(ImVec4 v, float a) { v.w = a; return ImGui::ColorConvertFloat4ToU32(v); }

// ─── Общее состояние потоков ──────────────────────────────────────────────────
static GameInfo            g_gameInfo;
static SharedPortraitState g_portraitState;
static GuiPickerState      g_pickerState;

// Данные игрока: пишутся фоновыми потоками, читаются GUI
struct PlayerState {
    std::mutex  mtx;
    long long   accountId     = 0;
    char        name[128]     = {};
    bool        hasPlayer     = false;
    bool        phase1Running = false;
    bool        phase1Done    = false;
    bool        phase1Error   = false;
    char        phase1Msg[256] = {};
    bool        idChanged     = false;  // сигнал оркестратору сбросить фазу 3
    std::vector<uint8_t> avatarData;    // сырые байты изображения (из HTTP)
    bool        avatarDataReady = false;
};
static PlayerState g_player;

// Текстура аватара (только GUI-поток)
static ID3D11ShaderResourceView* g_avatarSRV = nullptr;

// Кэш портретов героев: heroId → D3D11 текстура (PNG из assets/, файлы именованы
// по стабильному heroes.name, а не localized_name — который Valve иногда меняет).
static std::map<int, ID3D11ShaderResourceView*> g_heroPortraits;

// Конфигурация (из переменных окружения / умолчаний, затем read-only)
static std::string g_stratzToken;

// Баннер обновления: данные требуют более новую версию приложения
static bool g_schemaBannerNeeded = false;
static const char* DB_PATH    = "playerandlivestats.db";
static const char* MODEL_PATH = "draft_helper_abstract";

// heroId → localized_name, для текста в GUI (только для отображения — не для
// сопоставления, см. portrait_runner.cpp). Заполняется лениво из heroes:
// на момент первого вызова таблица могла ещё не наполниться (runDataFetcherInit
// работает в фоне), поэтому пробуем снова, пока она пустая.
static std::map<int, std::string> g_heroDisplayNames;
static std::string heroDisplayName(int heroId, const char* fallback) {
    if (g_heroDisplayNames.empty()) {
        sqlite3* db = nullptr;
        if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db, "SELECT id, localized_name FROM heroes",
                                   -1, &st, nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW) {
                    int id = sqlite3_column_int(st, 0);
                    const char* nm = (const char*)sqlite3_column_text(st, 1);
                    if (nm) g_heroDisplayNames[id] = nm;
                }
                sqlite3_finalize(st);
            }
            sqlite3_close(db);
        }
    }
    auto it = g_heroDisplayNames.find(heroId);
    return it != g_heroDisplayNames.end() ? it->second : (fallback ? fallback : "");
}

// ─── Meta Heroes: живая стата по героям (таблица `stats`, живой STRATZ heroStats) ─
// Тот же ленивый паттерн, что и heroDisplayName(): читаем DB_PATH напрямую из GUI-потока,
// повторяем попытку, пока фаза 1a (фоновый поток) не наполнит таблицу.
struct MetaHeroRow { char name[64] = {}; int heroId = 0; int games = 0; float winRate = 0.f; };
// Ключ 0 = агрегат по всем позициям (используется, когда позиция игрока ещё не определена).
static std::map<int, std::vector<MetaHeroRow>> g_metaHeroStats;
static bool g_metaHeroStatsLoaded = false;

static void loadMetaHeroStatsIfNeeded() {
    if (g_metaHeroStatsLoaded) return;
    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) { if (db) sqlite3_close(db); return; }
    sqlite3_stmt* st = nullptr;
    bool any = false;
    if (sqlite3_prepare_v2(db,
            "SELECT s.hero_id, s.pos, s.games, s.wins, h.localized_name "
            "FROM stats s JOIN heroes h ON h.id = s.hero_id "
            "WHERE s.pos BETWEEN 1 AND 5", -1, &st, nullptr) == SQLITE_OK) {
        struct Raw { int heroId, games, wins; std::string name; };
        std::map<int, std::vector<Raw>> byPos;      // pos(1-5) -> raw rows
        std::map<int, std::pair<int,int>> aggByHero; // heroId -> (games, wins) summed over all pos
        std::map<int, std::string> nameByHero;
        while (sqlite3_step(st) == SQLITE_ROW) {
            any = true;
            int hid  = sqlite3_column_int(st, 0);
            int pos  = sqlite3_column_int(st, 1);
            int g    = sqlite3_column_int(st, 2);
            int w    = sqlite3_column_int(st, 3);
            const char* nm = (const char*)sqlite3_column_text(st, 4);
            byPos[pos].push_back({hid, g, w, nm ? nm : ""});
            auto& agg = aggByHero[hid];
            agg.first  += g;
            agg.second += w;
            nameByHero[hid] = nm ? nm : "";
        }
        sqlite3_finalize(st);

        if (any) {
            auto toRow = [](int hid, int g, int w, const std::string& nm) {
                MetaHeroRow r;
                r.heroId  = hid;
                r.games   = g;
                r.winRate = g > 0 ? (float)w / g : 0.f;
                std::snprintf(r.name, sizeof(r.name), "%s", nm.c_str());
                return r;
            };
            for (auto& [pos, rows] : byPos) {
                long long total = 0;
                for (auto& r : rows) total += r.games;
                long long floor = (long long)(total * 0.01);
                std::vector<MetaHeroRow> kept;
                for (auto& r : rows) if (r.games >= floor) kept.push_back(toRow(r.heroId, r.games, r.wins, r.name));
                std::sort(kept.begin(), kept.end(), [](auto&a, auto&b){ return a.games > b.games; });
                g_metaHeroStats[pos] = std::move(kept);
            }
            long long totalAll = 0;
            for (auto& [hid, gw] : aggByHero) totalAll += gw.first;
            long long aggFloor = (long long)(totalAll * 0.01);
            std::vector<MetaHeroRow> aggRows;
            for (auto& [hid, gw] : aggByHero)
                if (gw.first >= aggFloor) aggRows.push_back(toRow(hid, gw.first, gw.second, nameByHero[hid]));
            std::sort(aggRows.begin(), aggRows.end(), [](auto&a, auto&b){ return a.games > b.games; });
            g_metaHeroStats[0] = std::move(aggRows); // 0 = агрегат по всем позициям (ордер по сумме матчей)

            g_metaHeroStatsLoaded = true;
        }
    }
    sqlite3_close(db);
}
// Портреты + пикер: активны HERO_SELECTION + 5с. GUI показывает результат до конца игры.
static constexpr int PHASE3_TAIL_SEC = 5;

// Управление потоками
static std::atomic<bool> g_pickerRunning{false};
static std::atomic<bool> g_portraitRunning{false};
static std::atomic<bool> g_orchestratorRunning{true};
static std::atomic<bool> g_posRefreshNeeded{false};
static std::thread       g_pickerThread;
static std::thread       g_portraitThread;
static std::thread       g_orchestratorThread;

// Состояние GUI (только из GUI-потока)
static bool  s_editMode     = false;
static char  s_inputBuf[32] = {};

// ─── SQLite: таблица player_info ──────────────────────────────────────────────
static void createPlayerInfoTable(sqlite3* db) {
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS player_info ("
        "  account_id  INTEGER PRIMARY KEY,"
        "  player_name TEXT DEFAULT ''"
        ");",
        nullptr, nullptr, nullptr);
}

static long long loadSavedPlayer(sqlite3* db, char nameOut[128]) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT account_id, player_name FROM player_info LIMIT 1",
            -1, &st, nullptr) != SQLITE_OK)
        return 0;
    long long id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
        const char* n = (const char*)sqlite3_column_text(st, 1);
        if (n && nameOut) std::snprintf(nameOut, 128, "%s", n);
    }
    sqlite3_finalize(st);
    return id;
}

static void savePlayerInfo(sqlite3* db, long long accountId, const char* name) {
    sqlite3_exec(db, "DELETE FROM player_info;", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO player_info (account_id, player_name) VALUES (?, ?)",
            -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, accountId);
    sqlite3_bind_text(st,  2, name ? name : "", -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// ─── Таблица livepicks ────────────────────────────────────────────────────────
static void createLivePicksIfNotExists(sqlite3* db) {
    sqlite3_exec(db, R"(
        CREATE TABLE IF NOT EXISTS livepicks (
            match_id       INTEGER NOT NULL DEFAULT 0,
            our_account_id INTEGER NOT NULL DEFAULT 0,
            our_side       INTEGER NOT NULL DEFAULT 1,
            our_slot       INTEGER NOT NULL DEFAULT 1,
            updated_at     INTEGER NOT NULL DEFAULT 0,
            r1_hero INTEGER DEFAULT 0, r1_pos INTEGER DEFAULT 0,
            r2_hero INTEGER DEFAULT 0, r2_pos INTEGER DEFAULT 0,
            r3_hero INTEGER DEFAULT 0, r3_pos INTEGER DEFAULT 0,
            r4_hero INTEGER DEFAULT 0, r4_pos INTEGER DEFAULT 0,
            r5_hero INTEGER DEFAULT 0, r5_pos INTEGER DEFAULT 0,
            d1_hero INTEGER DEFAULT 0, d1_pos INTEGER DEFAULT 0,
            d2_hero INTEGER DEFAULT 0, d2_pos INTEGER DEFAULT 0,
            d3_hero INTEGER DEFAULT 0, d3_pos INTEGER DEFAULT 0,
            d4_hero INTEGER DEFAULT 0, d4_pos INTEGER DEFAULT 0,
            d5_hero INTEGER DEFAULT 0, d5_pos INTEGER DEFAULT 0
        );
    )", nullptr, nullptr, nullptr);
}

static void initLivePicksRow(sqlite3* db, long long matchId,
                              long long accountId, int ourSide, int ourSlot) {
    sqlite3_exec(db, "DELETE FROM livepicks;", nullptr, nullptr, nullptr);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO livepicks "
            "(match_id,our_account_id,our_side,our_slot,updated_at) "
            "VALUES (?,?,?,?,?);",
            -1, &st, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, matchId);
    sqlite3_bind_int64(st, 2, accountId);
    sqlite3_bind_int(st,   3, ourSide);
    sqlite3_bind_int(st,   4, ourSlot);
    sqlite3_bind_int64(st, 5,
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    sqlite3_step(st);
    sqlite3_finalize(st);
}

// ─── Создание D3D11 текстуры из байтов изображения (JPEG/PNG) ─────────────────
static ID3D11ShaderResourceView* createTextureFromImageData(
    const uint8_t* data, size_t size)
{
    if (!data || size == 0 || !g_Device) return nullptr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return nullptr;
    memcpy(GlobalLock(hMem), data, size);
    GlobalUnlock(hMem);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stream))) {
        GlobalFree(hMem);
        return nullptr;
    }

    ID3D11ShaderResourceView* result = nullptr;
    {
        Gdiplus::Bitmap bmp(stream);
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
            if (w > 0 && h > 0) {
                Gdiplus::BitmapData bd{};
                Gdiplus::Rect rect(0, 0, w, h);
                bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                             PixelFormat32bppARGB, &bd);

                D3D11_TEXTURE2D_DESC desc{};
                desc.Width  = w;  desc.Height = h;
                desc.MipLevels = 1; desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage     = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA init{};
                init.pSysMem      = bd.Scan0;
                init.SysMemPitch  = bd.Stride;

                ID3D11Texture2D* tex = nullptr;
                HRESULT hr = g_Device->CreateTexture2D(&desc, &init, &tex);
                bmp.UnlockBits(&bd);

                if (SUCCEEDED(hr) && tex) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format = desc.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    g_Device->CreateShaderResourceView(tex, &srvDesc, &result);
                    tex->Release();
                }
            }
        }
    }
    stream->Release();
    return result;
}

// ─── Загрузка портретов героев из папки assets/ ──────────────────────────────
// Файлы assets/*.png именованы по heroes.name (без префикса npc_dota_hero_,
// напр. "antimage.png"), а не по localized_name — тот Valve иногда временно
// меняет. Портреты кэшируются по heroId, поэтому переименование Valve не влияет
// на отображение.

// Суффикс heroes.name → heroId. Зашито в бинарник (не читается из БД), т.к.
// heroId в Dota 2 неизменны, а таблица heroes на первом запуске приложения
// ещё не заполнена (runDataFetcherInit работает в фоновом потоке и не успевает
// до вызова loadHeroPortraits() при старте GUI). При добавлении нового героя
// сюда нужно добавить строку вместе с assets/<suffix>.png.
static const std::map<std::string, int>& heroNameSuffixToId() {
    static const std::map<std::string, int> m = {
        {"antimage", 1}, {"axe", 2}, {"bane", 3}, {"bloodseeker", 4},
        {"crystal_maiden", 5}, {"drow_ranger", 6}, {"earthshaker", 7}, {"juggernaut", 8},
        {"mirana", 9}, {"morphling", 10}, {"nevermore", 11}, {"phantom_lancer", 12},
        {"puck", 13}, {"pudge", 14}, {"razor", 15}, {"sand_king", 16},
        {"storm_spirit", 17}, {"sven", 18}, {"tiny", 19}, {"vengefulspirit", 20},
        {"windrunner", 21}, {"zuus", 22}, {"kunkka", 23}, {"lina", 25},
        {"lion", 26}, {"shadow_shaman", 27}, {"slardar", 28}, {"tidehunter", 29},
        {"witch_doctor", 30}, {"lich", 31}, {"riki", 32}, {"enigma", 33},
        {"tinker", 34}, {"sniper", 35}, {"necrolyte", 36}, {"warlock", 37},
        {"beastmaster", 38}, {"queenofpain", 39}, {"venomancer", 40}, {"faceless_void", 41},
        {"skeleton_king", 42}, {"death_prophet", 43}, {"phantom_assassin", 44}, {"pugna", 45},
        {"templar_assassin", 46}, {"viper", 47}, {"luna", 48}, {"dragon_knight", 49},
        {"dazzle", 50}, {"rattletrap", 51}, {"leshrac", 52}, {"furion", 53},
        {"life_stealer", 54}, {"dark_seer", 55}, {"clinkz", 56}, {"omniknight", 57},
        {"enchantress", 58}, {"huskar", 59}, {"night_stalker", 60}, {"broodmother", 61},
        {"bounty_hunter", 62}, {"weaver", 63}, {"jakiro", 64}, {"batrider", 65},
        {"chen", 66}, {"spectre", 67}, {"ancient_apparition", 68}, {"doom_bringer", 69},
        {"ursa", 70}, {"spirit_breaker", 71}, {"gyrocopter", 72}, {"alchemist", 73},
        {"invoker", 74}, {"silencer", 75}, {"obsidian_destroyer", 76}, {"lycan", 77},
        {"brewmaster", 78}, {"shadow_demon", 79}, {"lone_druid", 80}, {"chaos_knight", 81},
        {"meepo", 82}, {"treant", 83}, {"ogre_magi", 84}, {"undying", 85},
        {"rubick", 86}, {"disruptor", 87}, {"nyx_assassin", 88}, {"naga_siren", 89},
        {"keeper_of_the_light", 90}, {"wisp", 91}, {"visage", 92}, {"slark", 93},
        {"medusa", 94}, {"troll_warlord", 95}, {"centaur", 96}, {"magnataur", 97},
        {"shredder", 98}, {"bristleback", 99}, {"tusk", 100}, {"skywrath_mage", 101},
        {"abaddon", 102}, {"elder_titan", 103}, {"legion_commander", 104}, {"techies", 105},
        {"ember_spirit", 106}, {"earth_spirit", 107}, {"abyssal_underlord", 108}, {"terrorblade", 109},
        {"phoenix", 110}, {"oracle", 111}, {"winter_wyvern", 112}, {"arc_warden", 113},
        {"monkey_king", 114}, {"dark_willow", 119}, {"pangolier", 120}, {"grimstroke", 121},
        {"hoodwink", 123}, {"void_spirit", 126}, {"snapfire", 128}, {"mars", 129},
        {"ringmaster", 131}, {"dawnbreaker", 135}, {"marci", 136}, {"primal_beast", 137},
        {"muerta", 138}, {"kez", 145}, {"largo", 155},
    };
    return m;
}

static void loadHeroPortraits() {
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(L"assets\\*.png", &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    const auto& nameToId = heroNameSuffixToId();

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, L"assets\\%s", fd.cFileName);

        HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) continue;

        DWORD sz = GetFileSize(hFile, nullptr);
        if (sz == 0 || sz == INVALID_FILE_SIZE) { CloseHandle(hFile); continue; }

        std::vector<uint8_t> buf(sz);
        DWORD bytesRead = 0;
        ReadFile(hFile, buf.data(), sz, &bytesRead, nullptr);
        CloseHandle(hFile);
        if (bytesRead != sz) continue;

        auto* srv = createTextureFromImageData(buf.data(), buf.size());
        if (!srv) continue;

        // Имя файла (без .png) = суффикс heroes.name → находим heroId
        int len = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                      nullptr, 0, nullptr, nullptr);
        std::string name(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                            name.data(), len, nullptr, nullptr);
        if (name.size() > 4) name.resize(name.size() - 4); // strip ".png"
        for (auto& c : name) c = static_cast<char>(std::tolower((unsigned char)c));

        auto it = nameToId.find(name);
        if (it == nameToId.end()) { srv->Release(); continue; }

        g_heroPortraits[it->second] = srv;
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

// ─── OpenDota: получение имени и аватара игрока (фоновый поток) ───────────────
static void fetchOpenDotaProfile(long long accountId) {
    std::string url = "https://api.opendota.com/api/players/"
                    + std::to_string(accountId);
    std::string resp;
    try { resp = httpGet(url); } catch (...) { return; }
    if (resp.empty()) return;

    auto j = json::parse(resp, nullptr, false);
    if (j.is_discarded() || !j.contains("profile") || !j["profile"].is_object())
        return;

    auto& profile = j["profile"];
    std::string name      = sanitizeUtf8(profile.value("personaname", ""));
    std::string avatarUrl = profile.value("avatarmedium", "");

    if (!name.empty()) {
        sqlite3* db = nullptr;
        if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
            createPlayerInfoTable(db);
            savePlayerInfo(db, accountId, name.c_str());
            sqlite3_close(db);
        }
        std::lock_guard<std::mutex> lk(g_player.mtx);
        std::snprintf(g_player.name, sizeof(g_player.name), "%s", name.c_str());
    }

    if (!avatarUrl.empty()) {
        try {
            std::string imgData = httpGet(avatarUrl);
            if (!imgData.empty()) {
                std::lock_guard<std::mutex> lk(g_player.mtx);
                g_player.avatarData.assign(imgData.begin(), imgData.end());
                g_player.avatarDataReady = true;
            }
        } catch (...) {}
    }
}

// ─── Запуск фазы 1 (DataFetcher + имя игрока) ────────────────────────────────
static void startPhase1(long long accountId) {
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        g_player.phase1Running = true;
        g_player.phase1Done    = false;
        g_player.phase1Error   = false;
        std::snprintf(g_player.phase1Msg, sizeof(g_player.phase1Msg),
                      "Fetching data...");
    }

    // Сброс аватара (GUI-поток, безопасно — startPhase1 вызывается из GUI)
    if (g_avatarSRV) { g_avatarSRV->Release(); g_avatarSRV = nullptr; }

    std::thread([accountId]() {
        // Имя + аватар из OpenDota (~1с)
        fetchOpenDotaProfile(accountId);

        // Основная загрузка данных (~30-90с)
        int rc = runDataFetcher(accountId, g_stratzToken);

        std::lock_guard<std::mutex> lk(g_player.mtx);
        g_player.phase1Running = false;
        if (rc == 0) {
            g_player.phase1Done = true;
            std::snprintf(g_player.phase1Msg, sizeof(g_player.phase1Msg),
                          "Data ready");
        } else {
            g_player.phase1Error = true;
            std::snprintf(g_player.phase1Msg, sizeof(g_player.phase1Msg),
                          "Fetch error");
        }
    }).detach();
}

// ─── Фоновый оркестратор: управление portrait + picker потоками ───────────────
static void orchestratorMain() {
    long long accountId = 0;
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        accountId = g_player.accountId;
    }

    sqlite3* db = nullptr;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        LOG_ERR("Orchestrator: cannot open DB");
        return;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;",   nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    createLivePicksIfNotExists(db);
    sqlite3_exec(db, "DELETE FROM livepicks;", nullptr, nullptr, nullptr);

    // ── [D] overlay кнопка — запускается один раз, видна всегда ─────────────
    startDotaOverlay();

    // Start GSI server (always)
    std::thread([&]{ runGsiServer(g_gameInfo); }).detach();

    std::string lastMatchId;
    using Clock = std::chrono::steady_clock;
    Clock::time_point phase3EndTime;
    bool phase3EndPending = false;
    int  prevOurSlot = -1;
    int  prevOurSide = -1;

    while (g_orchestratorRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Refresh accountId (user might have entered it after start)
        {
            std::lock_guard<std::mutex> lk(g_player.mtx);
            accountId = g_player.accountId;
        }

        // Чтение состояния игры
        GamePhase   phase;
        std::string matchId;
        int         ourSide, ourSlot;
        bool        newMatch, isHeroSel;
        {
            std::lock_guard<std::mutex> lk(g_gameInfo.mtx);

            // GSI таймаут: если Dota 2 закрыта, GSI перестаёт слать данные.
            // Сброс на IDLE через 15 секунд без обновлений.
            if (g_gameInfo.phase != GamePhase::IDLE &&
                g_gameInfo.lastUpdate.time_since_epoch().count() > 0)
            {
                auto elapsed = std::chrono::steady_clock::now() - g_gameInfo.lastUpdate;
                if (elapsed > std::chrono::seconds(15)) {
                    g_gameInfo.phase           = GamePhase::IDLE;
                    g_gameInfo.isHeroSelection = false;
                    g_gameInfo.matchId.clear();
                    std::puts("[orchestrator] GSI timeout — reset to IDLE");
                }
            }

            phase     = g_gameInfo.phase;
            matchId   = g_gameInfo.matchId;
            ourSide   = g_gameInfo.ourSide;
            ourSlot   = g_gameInfo.ourSlot;
            newMatch  = g_gameInfo.newMatch;
            isHeroSel = g_gameInfo.isHeroSelection;
            g_gameInfo.newMatch = false;
        }

        // ── Слот/сторона изменились (GSI может прислать позже чем newMatch) ──
        if ((ourSlot != prevOurSlot || ourSide != prevOurSide) && !matchId.empty()) {
            const char* slotSql =
                "UPDATE livepicks SET our_slot=?, our_side=?, updated_at=? WHERE 1;";
            sqlite3_stmt* ss = nullptr;
            if (sqlite3_prepare_v2(db, slotSql, -1, &ss, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(ss, 1, ourSlot);
                sqlite3_bind_int(ss, 2, ourSide);
                sqlite3_bind_int64(ss, 3,
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                sqlite3_step(ss);
                sqlite3_finalize(ss);
            }
            prevOurSlot = ourSlot;
            prevOurSide = ourSide;
        }

        bool isDraftOrIngame = (phase == GamePhase::DRAFT ||
                                phase == GamePhase::INGAME);

        // ── ID сменился — сбросить фазу 3 и пересоздать livepicks ────────────
        bool idChanged = false;
        {
            std::lock_guard<std::mutex> lk(g_player.mtx);
            if (g_player.idChanged) {
                idChanged = true;
                g_player.idChanged = false;
                accountId = g_player.accountId;
            }
        }
        if (idChanged) {
            // Пикер зависит от accountId — перезапускаем
            if (g_pickerRunning.load()) {
                g_pickerRunning.store(false);
                if (g_pickerThread.joinable()) g_pickerThread.join();
            }
            // НЕ сбрасываем pickerState — драфт остаётся видимым

            // Обновить только accountId в livepicks (НЕ пересоздавать строку)
            if (!matchId.empty()) {
                sqlite3_exec(db,
                    ("UPDATE livepicks SET our_account_id="
                     + std::to_string(accountId) + ";").c_str(),
                    nullptr, nullptr, nullptr);
                lastMatchId = matchId;
            }

            // Запустить пикер если есть accountId и идёт драфт
            if (phase == GamePhase::DRAFT && accountId != 0 && !g_pickerRunning.load()) {
                g_pickerRunning.store(true);
                g_pickerThread = std::thread([]{
                    runPickerGui(MODEL_PATH, DB_PATH,
                                 g_pickerRunning, &g_pickerState, &g_portraitState);
                });
            }
        }

        // ── New game ──────────────────────────────────────────────────────────
        if (newMatch && !matchId.empty()) {
            // Stop everything
            if (g_pickerRunning.load()) {
                g_pickerRunning.store(false);
                if (g_pickerThread.joinable()) g_pickerThread.join();
            }
            if (g_portraitRunning.load()) {
                g_portraitRunning.store(false);
                if (g_portraitThread.joinable()) g_portraitThread.join();
                g_portraitState.clear();
            }
            g_pickerState.reset();
            phase3EndPending = false;

            try {
                long long mid = std::stoll(matchId);
                initLivePicksRow(db, mid, accountId, ourSide, ourSlot);
            } catch (...) {}
            lastMatchId = matchId;
        }

        // ── Фаза 3: портреты + пикер ─────────────────────────────────────────
        // Если игра закончилась (IDLE/POSTGAME) — немедленно остановить потоки,
        // даже если isHeroSel ещё true (stale из-за обрыва GSI).
        if (phase == GamePhase::IDLE || phase == GamePhase::POSTGAME) {
            if (g_pickerRunning.load()) {
                g_pickerRunning.store(false);
                if (g_pickerThread.joinable()) g_pickerThread.join();
            }
            if (g_portraitRunning.load()) {
                g_portraitRunning.store(false);
                if (g_portraitThread.joinable()) g_portraitThread.join();
                g_portraitState.clear();
            }
            phase3EndPending = false;
            if (g_pickerState.gameStarted) {
                g_pickerState.reset();
            }

        } else if (isHeroSel) {
            phase3EndPending = false;

            // Portrait capture — всегда при HERO_SELECTION (заполняет livepicks)
            if (!g_portraitRunning.load()) {
                g_portraitRunning.store(true);
                g_portraitState.clear();
                {
                    std::lock_guard<std::mutex> lk(g_portraitState.mtx);
                    g_portraitState.active = true;
                }
                g_portraitThread = std::thread([]{
                    runPortraitCapture(g_gameInfo, DB_PATH,
                                       g_portraitRunning, g_portraitState);
                });
            }

            // Picker — только при наличии accountId
            if (!g_pickerRunning.load() && accountId != 0) {
                g_pickerRunning.store(true);
                g_pickerThread = std::thread([]{
                    runPickerGui(MODEL_PATH, DB_PATH,
                                 g_pickerRunning, &g_pickerState, &g_portraitState);
                });
            }

        } else {
            // HERO_SELECTION кончился, но игра продолжается — отсчитываем хвост
            if (g_pickerRunning.load() || g_portraitRunning.load()) {
                if (!phase3EndPending) {
                    phase3EndPending = true;
                    phase3EndTime    = Clock::now();
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::now() - phase3EndTime).count();
                if (elapsed >= PHASE3_TAIL_SEC) {
                    g_pickerRunning.store(false);
                    if (g_pickerThread.joinable()) g_pickerThread.join();
                    g_portraitRunning.store(false);
                    if (g_portraitThread.joinable()) g_portraitThread.join();
                    g_portraitState.clear();
                    phase3EndPending = false;
                }
            }
        }

        // ── Portrait → GUI: показываем драфт без пикера ─────────────────────
        if (g_portraitRunning.load() && !g_pickerRunning.load()) {
            PortraitResult snap[10];
            {
                std::lock_guard<std::mutex> lk(g_portraitState.mtx);
                for (int i = 0; i < 10; ++i) snap[i] = g_portraitState.slots[i];
            }
            std::lock_guard<std::mutex> lk(g_pickerState.mtx);
            g_pickerState.gameStarted = true;
            g_pickerState.isRadiant   = (ourSide == 1);
            g_pickerState.ourSlot     = ourSlot;
            for (int i = 0; i < 5; ++i) {
                HeroSlotGui& s = g_pickerState.radiant[i];
                s.heroId = snap[i].heroId;
                s.filled = snap[i].valid();
                s.isYou  = (ourSide == 1 && i == ourSlot - 1);
                if (s.filled) std::snprintf(s.name, sizeof(s.name), "%s",
                    heroDisplayName(snap[i].heroId, snap[i].heroName.c_str()).c_str());
                else s.name[0] = '\0';
            }
            for (int i = 0; i < 5; ++i) {
                HeroSlotGui& s = g_pickerState.dire[i];
                s.heroId = snap[5+i].heroId;
                s.filled = snap[5+i].valid();
                s.isYou  = (ourSide == 0 && i == ourSlot - 1);
                if (s.filled) std::snprintf(s.name, sizeof(s.name), "%s",
                    heroDisplayName(snap[5+i].heroId, snap[5+i].heroName.c_str()).c_str());
                else s.name[0] = '\0';
            }
            g_pickerState.recCount     = 0;
            g_pickerState.ourHeroPicked = false;
            g_pickerState.winProb      = 0.f;
        }

        // ── One-shot: ручная смена позиции вне фазы 3 → запись в DB + инференс ──
        {
            static bool oneShotActive = false;
            static int  oneShotGen    = 0;

            if (g_posRefreshNeeded.exchange(false)
                && !g_pickerRunning.load()
                && g_pickerState.gameStarted
                && accountId != 0)
            {
                // Записать manualPos нашей команды в livepicks
                int side = 0;
                {
                    std::lock_guard<std::mutex> lk(g_gameInfo.mtx);
                    side = g_gameInfo.ourSide;
                }
                int base = (side == 1) ? 0 : 5;
                for (int i = 0; i < 5; ++i) {
                    int slot = base + i;
                    int mp = 0;
                    {
                        std::lock_guard<std::mutex> lk(g_portraitState.mtx);
                        mp = g_portraitState.manualPos[slot];
                    }
                    if (mp > 0 && mp <= 5) {
                        char col[16];
                        if (slot < 5) std::snprintf(col, sizeof(col), "r%d_pos", slot + 1);
                        else          std::snprintf(col, sizeof(col), "d%d_pos", slot - 4);
                        char sql[128];
                        std::snprintf(sql, sizeof(sql),
                            "UPDATE livepicks SET %s=%d, updated_at=%lld;",
                            col, mp, (long long)std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count());
                        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
                    }
                }

                oneShotGen = g_pickerState.inferenceGen.load(std::memory_order_acquire);
                g_pickerRunning.store(true);
                if (g_pickerThread.joinable()) g_pickerThread.join();
                g_pickerThread = std::thread([]{
                    runPickerGui(MODEL_PATH, DB_PATH,
                                 g_pickerRunning, &g_pickerState, &g_portraitState);
                });
                oneShotActive = true;
            }

            if (oneShotActive &&
                g_pickerState.inferenceGen.load(std::memory_order_acquire) > oneShotGen)
            {
                g_pickerRunning.store(false);
                if (g_pickerThread.joinable()) g_pickerThread.join();
                oneShotActive = false;
            }
        }

    } // end while (g_orchestratorRunning)

    // Остановка потоков
    g_pickerRunning.store(false);
    g_portraitRunning.store(false);
    if (g_pickerThread.joinable())   g_pickerThread.join();
    if (g_portraitThread.joinable()) g_portraitThread.join();

    sqlite3_close(db);
}

// ─── Стиль ImGui ─────────────────────────────────────────────────────────────
static void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = s.ChildRounding = s.FrameRounding  = 0.f;
    s.PopupRounding  = s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0.f;
    s.WindowBorderSize = s.ChildBorderSize = 1.f;
    s.FrameBorderSize  = 0.f;
    s.WindowPadding = {8.f, 8.f};
    s.FramePadding  = {6.f, 3.f};
    s.ItemSpacing   = {6.f, 5.f};
    s.ScrollbarSize = 10.f;
    s.IndentSpacing = 12.f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = kBg;
    c[ImGuiCol_ChildBg]              = kCard;
    c[ImGuiCol_Border]               = kBorder;
    c[ImGuiCol_Text]                 = kText;
    c[ImGuiCol_TextDisabled]         = kMuted;
    c[ImGuiCol_FrameBg]              = kCard2;
    c[ImGuiCol_FrameBgHovered]       = {0.20f,0.20f,0.20f,1.f};
    c[ImGuiCol_FrameBgActive]        = {0.25f,0.25f,0.25f,1.f};
    c[ImGuiCol_Button]               = kCard2;
    c[ImGuiCol_ButtonHovered]        = {0.22f,0.22f,0.22f,1.f};
    c[ImGuiCol_ButtonActive]         = {0.28f,0.28f,0.28f,1.f};
    c[ImGuiCol_Header]               = kCard2;
    c[ImGuiCol_HeaderHovered]        = {0.22f,0.22f,0.22f,1.f};
    c[ImGuiCol_Separator]            = kBorder;
    c[ImGuiCol_ScrollbarBg]          = kBg;
    c[ImGuiCol_ScrollbarGrab]        = kCard2;
    c[ImGuiCol_ScrollbarGrabHovered] = {0.22f,0.22f,0.22f,1.f};
    c[ImGuiCol_TitleBg]              = kCard;
    c[ImGuiCol_TitleBgActive]        = kCard;
    c[ImGuiCol_Tab]                  = kCard;
    c[ImGuiCol_TabHovered]           = kCard2;
    c[ImGuiCol_TabActive]            = kCard2;
    // Масштаб 1.25× (вместе со шрифтом 20px)
    s.ScaleAllSizes(1.25f);
}

// ─── Вспомогательные функции отрисовки ────────────────────────────────────────
static void DrawPortrait(ImDrawList* dl, ImVec2 p, float sz,
                         ImU32 fill, ImU32 border, const char* label,
                         ImTextureID tex = 0) {
    if (tex) {
        dl->AddRectFilled(p, {p.x+sz, p.y+sz}, C(kCard));
        dl->AddImage(tex, p, {p.x+sz, p.y+sz});
    } else {
        dl->AddRectFilled(p, {p.x+sz, p.y+sz}, fill);
        if (label && *label) {
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText({p.x+(sz-ts.x)*0.5f, p.y+(sz-ts.y)*0.5f}, C(kText), label);
        }
    }
    dl->AddRect(p, {p.x+sz, p.y+sz}, border, 0.f, 0, 1.f);
}
static void DrawDashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    const float D=5.f, G=4.f;
    auto hline = [&](float x0, float x1, float y) {
        for (float x=x0; x<x1; x+=D+G)
            dl->AddLine({x,y},{std::min(x+D,x1),y},col);
    };
    auto vline = [&](float y0, float y1, float x) {
        for (float y=y0; y<y1; y+=D+G)
            dl->AddLine({x,y},{x,std::min(y+D,y1)},col);
    };
    hline(a.x,b.x,a.y); hline(a.x,b.x,b.y);
    vline(a.y,b.y,a.x); vline(a.y,b.y,b.x);
}
static void DrawBar(ImDrawList* dl, ImVec2 p, float w, float h, float frac, ImU32 fill) {
    frac = std::max(0.f, std::min(frac, 1.f));
    dl->AddRectFilled(p, {p.x+w,      p.y+h}, C(kCard2));
    dl->AddRectFilled(p, {p.x+w*frac, p.y+h}, fill);
}
static ImVec4 WinColor(float w) {
    if (w >= 0.55f) return kGreen;
    if (w >= 0.50f) return kAmber;
    return kRed;
}

// ─── Отрисовка слота героя ────────────────────────────────────────────────────
// absSlot: 0-9 (0-4 Radiant, 5-9 Dire). usedPos: позиции 1-5 занятые другими слотами в команде.
static void DrawHeroSlot(float rowW, const HeroSlotGui& h,
                         bool radiantSide, bool showPos, int slotNum,
                         int absSlot, const int usedPos[5]) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      rp  = ImGui::GetCursorScreenPos();
    const float lh  = ImGui::GetTextLineHeight();
    const float H   = lh * 3.0f;
    const float PSZ = lh * 2.1f;
    const float PAD = lh * 0.35f;

    ImU32 bgCol     = h.filled ? C(kCard) : Ca(kCard2, h.isYou ? 0.45f : 0.55f);
    ImU32 borderCol = h.isYou  ? Ca(kText, 0.55f) : C(kBorder);
    float bThick    = h.isYou  ? 2.f : 1.f;

    dl->AddRectFilled(rp, {rp.x+rowW, rp.y+H}, bgCol);
    if (!h.filled && !h.isYou)
        DrawDashedRect(dl, rp, {rp.x+rowW, rp.y+H}, borderCol);
    else
        dl->AddRect(rp, {rp.x+rowW, rp.y+H}, borderCol, 0.f, 0, bThick);

    ImVec2 pp      = {rp.x+PAD, rp.y+(H-PSZ)*0.5f};
    ImU32  heroFill = radiantSide ? IM_COL32(28,72,40,220) : IM_COL32(88,32,28,220);

    if (!h.filled && h.isYou) {
        dl->AddRectFilled(pp, {pp.x+PSZ, pp.y+PSZ}, C(kCard2));
        dl->AddRect      (pp, {pp.x+PSZ, pp.y+PSZ}, Ca(kText, 0.2f));
        float cx=pp.x+PSZ/2.f, cy=pp.y+PSZ/2.f, arm=PSZ*0.22f;
        dl->AddLine({cx-arm,cy},{cx+arm,cy},C(kText),1.5f);
        dl->AddLine({cx,cy-arm},{cx,cy+arm},C(kText),1.5f);
    } else if (!h.filled) {
        dl->AddRectFilled(pp, {pp.x+PSZ, pp.y+PSZ}, C(kCard2));
        dl->AddRect      (pp, {pp.x+PSZ, pp.y+PSZ}, C(kBorder));
        ImVec2 ts = ImGui::CalcTextSize("?");
        dl->AddText({pp.x+(PSZ-ts.x)/2.f, pp.y+(PSZ-ts.y)/2.f}, C(kMuted), "?");
    } else {
        char ab[3] = {h.name[0], h.name[1] ? h.name[1] : '\0', '\0'};
        ImTextureID htex = 0;
        auto it = g_heroPortraits.find(h.heroId);
        if (it != g_heroPortraits.end()) htex = (ImTextureID)it->second;
        DrawPortrait(dl, pp, PSZ, heroFill, Ca(kText, 0.18f), ab, htex);
    }

    float tx = pp.x + PSZ + PAD;

    if (!h.filled && h.isYou) {
        dl->AddText({tx, rp.y+(H-lh)*0.5f}, C(kText), "Your Pick");
    } else if (!h.filled) {
        dl->AddText({tx, rp.y+(H-lh)*0.5f}, C(kMuted), "Unknown");
    } else {
        dl->AddText({tx, rp.y+(H-lh)*0.5f}, C(kText), h.name);
    }

    // Тег позиции — кликабельный popup для своей команды.
    if (showPos) {
        int dispPos = (h.pos > 0) ? h.pos : 0;
        char posStr[8];
        if (dispPos > 0)
            std::snprintf(posStr, sizeof(posStr), "Pos%d", dispPos);
        else
            std::snprintf(posStr, sizeof(posStr), "Pos?");

        ImVec2 ts  = ImGui::CalcTextSize(posStr);
        float  tgW = ts.x + PAD*1.5f;
        float  tgH = lh + 4.f;
        float  tgX = rp.x + rowW - tgW - PAD;
        float  tgY = rp.y + (H - tgH) * 0.5f;

        dl->AddRectFilled({tgX,tgY},{tgX+tgW,tgY+tgH}, C(kCard2));
        dl->AddText({tgX+PAD*0.75f,tgY+2.f}, C(kMuted), posStr);

        char popupId[32];
        std::snprintf(popupId, sizeof(popupId), "##pos_%d", absSlot);
        ImGui::SetCursorScreenPos({tgX, tgY});
        if (ImGui::InvisibleButton(popupId, {tgW, tgH}))
            ImGui::OpenPopup(popupId);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to set position");

        if (ImGui::BeginPopup(popupId)) {
            int myIdx = absSlot % 5;
            int teamBase = absSlot - myIdx; // 0 for radiant, 5 for dire
            for (int p = 1; p <= 5; ++p) {
                char label[16];
                std::snprintf(label, sizeof(label), "Pos %d", p);
                if (ImGui::Selectable(label, p == dispPos)) {
                    std::lock_guard<std::mutex> lk(g_portraitState.mtx);
                    // Свап: если позиция занята другим слотом, обменять
                    for (int j = 0; j < 5; ++j) {
                        if (j != myIdx && usedPos[j] == p) {
                            g_portraitState.manualPos[teamBase + j] = dispPos;
                            break;
                        }
                    }
                    g_portraitState.manualPos[absSlot] = p;
                    g_posRefreshNeeded.store(true);
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::SetCursorScreenPos(rp);
    ImGui::Dummy({rowW, H});
    ImGui::Spacing();
}

// ─── Метка секции ────────────────────────────────────────────────────────────
static void SectionLabel(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(">");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6.f);
    ImGui::TextUnformatted(label);
}

// ─── Панель драфта (слоты Radiant/Dire + winProb) ─────────────────────────────
static void DrawDraftPanel(float panelW) {
    const float PAD  = 8.f;
    const float colW = (panelW - PAD) * 0.5f;
    const float lh   = ImGui::GetTextLineHeight();

    // Снимок состояния пикера
    bool         gameStarted;
    bool         isRadiant;
    float        winProb;
    HeroSlotGui  rad[5], dir[5];
    bool         ourHeroPicked;
    GamePhase    phase;
    {
        std::lock_guard<std::mutex> lk(g_pickerState.mtx);
        gameStarted   = g_pickerState.gameStarted;
        isRadiant     = g_pickerState.isRadiant;
        winProb       = g_pickerState.winProb;
        ourHeroPicked = g_pickerState.ourHeroPicked;
        for (int i=0;i<5;i++) rad[i] = g_pickerState.radiant[i];
        for (int i=0;i<5;i++) dir[i] = g_pickerState.dire[i];
    }
    {
        std::lock_guard<std::mutex> lk(g_gameInfo.mtx);
        phase = g_gameInfo.phase;
    }

    int rCount=0, dCount=0;
    for (int i=0;i<5;i++) { if (rad[i].filled) ++rCount; if (dir[i].filled) ++dCount; }

    SectionLabel((rCount==5 && dCount==5) ? "STRATEGY PHASE" : "DRAFT PHASE");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Waiting overlay ────────────────────────────────────────────────────
    if (!gameStarted) {
        ImDrawList* dl  = ImGui::GetWindowDrawList();
        ImVec2      cp  = ImGui::GetCursorScreenPos();
        float       aH  = 300.f;
        float       aW  = panelW;

        dl->AddRectFilled(cp, {cp.x+aW, cp.y+aH}, Ca(kCard2, 0.4f));
        dl->AddRect      (cp, {cp.x+aW, cp.y+aH}, C(kBorder));

        const char* line1 = "Waiting for a game...";
        const char* line2 = (phase == GamePhase::IDLE) ? "GSI server: listening on :62326" : phaseName(phase);

        ImVec2 ts1 = ImGui::CalcTextSize(line1);
        ImVec2 ts2 = ImGui::CalcTextSize(line2);
        dl->AddText({cp.x+(aW-ts1.x)*0.5f, cp.y+aH*0.5f-lh},       C(kMuted),       line1);
        dl->AddText({cp.x+(aW-ts2.x)*0.5f, cp.y+aH*0.5f+2.f},       Ca(kMuted,0.6f), line2);

        ImGui::Dummy({aW, aH});
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        ImVec2 bp  = ImGui::GetCursorScreenPos();
        float  bW  = panelW;
        float  bH  = 52.f;
        dl2->AddRectFilled(bp,{bp.x+bW,bp.y+bH},Ca(kCard2,0.4f));
        dl2->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder));
        float cy = bp.y+(bH-lh)*0.5f;
        dl2->AddText({bp.x+10.f,cy},C(kMuted),">  Current draft win probability");
        dl2->AddText({bp.x+bW-50.f,cy},C(kMuted),"--.--%");
        ImGui::Dummy({bW,bH});
        return;
    }

    // Позиции, занятые в каждой команде (для popup без дубликатов)
    int radUsedPos[5] = {}, dirUsedPos[5] = {};
    for (int i=0;i<5;i++) { radUsedPos[i] = rad[i].pos; dirUsedPos[i] = dir[i].pos; }

    // ── Radiant column ─────────────────────────────────────────────────────
    ImGui::BeginGroup();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 hp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled({hp.x+1,hp.y+4},{hp.x+9,hp.y+12}, C(kGreen));
        ImGui::Dummy({10.f, lh});
        ImGui::SameLine(0, 4.f);
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::TextUnformatted("RADIANT");
        ImGui::PopStyleColor();
        ImGui::SameLine(colW-24.f);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::Text("%d/5", rCount);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        for (int i=0;i<5;i++) DrawHeroSlot(colW, rad[i], true,  isRadiant,  i+1, i, radUsedPos);
    }
    ImGui::EndGroup();

    ImGui::SameLine(0, PAD);

    // ── Dire column ────────────────────────────────────────────────────────
    ImGui::BeginGroup();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 hp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled({hp.x+1,hp.y+4},{hp.x+9,hp.y+12}, C(kRed));
        ImGui::Dummy({10.f, lh});
        ImGui::SameLine(0, 4.f);
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextUnformatted("DIRE");
        ImGui::PopStyleColor();
        ImGui::SameLine(colW-24.f);
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::Text("%d/5", dCount);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        for (int i=0;i<5;i++) DrawHeroSlot(colW, dir[i], false, !isRadiant, i+1, 5+i, dirUsedPos);
    }
    ImGui::EndGroup();

    // ── Win probability bar ────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bp  = ImGui::GetCursorScreenPos();
    float  bW  = panelW;
    float  bH  = 52.f;
    ImVec4 wc  = WinColor(winProb);

    dl->AddRectFilled(bp,{bp.x+bW,bp.y+bH},Ca(kCard2,0.4f));
    dl->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder),0.f,0,1.f);

    float cy = bp.y+(bH-lh)*0.5f;
    dl->AddText({bp.x+10.f,cy}, C(kMuted), ">");
    dl->AddText({bp.x+24.f,cy}, C(kMuted), "Current draft win probability");

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f%%", winProb*100.f);
    ImVec2 vts = ImGui::CalcTextSize(buf);
    dl->AddText({bp.x+bW-vts.x-10.f, cy}, ourHeroPicked ? C(wc) : C(kMuted), buf);

    DrawBar(dl, {bp.x+1.f, bp.y+bH-6.f}, bW-2.f, 4.f, winProb, C(wc));
    ImGui::Dummy({bW, bH});
}

// ─── Панель рекомендаций (top-10 / выбранный герой) ───────────────────────────
static void DrawPicksPanel(float panelW) {
    const float PAD       = 8.f;
    const float lh        = ImGui::GetTextLineHeight();
    const float WIN_COL_W = 88.f;
    const float BAR_W     = 68.f;
    const float rW_ref    = panelW;

    // Снимок состояния
    bool       gameStarted, ourHeroPicked;
    char       ourHeroName[64];
    int        ourHeroId;
    float      winProb;
    int        ourPosition, ourSlot;
    bool       isRadiant;
    PickRowGui recs[10];
    int        recCount;
    {
        std::lock_guard<std::mutex> lk(g_pickerState.mtx);
        gameStarted   = g_pickerState.gameStarted;
        ourHeroPicked = g_pickerState.ourHeroPicked;
        winProb       = g_pickerState.winProb;
        ourPosition   = g_pickerState.ourPosition;
        ourSlot       = g_pickerState.ourSlot;
        isRadiant     = g_pickerState.isRadiant;
        std::memcpy(ourHeroName, g_pickerState.ourHeroName, sizeof(ourHeroName));
        ourHeroId = g_pickerState.ourHeroId;
        recCount = g_pickerState.recCount;
        for (int i=0;i<recCount && i<10;i++) recs[i] = g_pickerState.recs[i];
    }

    SectionLabel(ourHeroPicked ? "YOUR PICK" : "RECOMMENDED PICKS");

    // Position badge — высота строки резервируется всегда (даже без бейджа),
    // чтобы разделитель совпадал по высоте с соседней панелью Meta Heroes.
    {
        float bH = lh + 6.f;
        if (ourPosition > 0) {
            char badgeText[32];
            std::snprintf(badgeText, sizeof(badgeText),
                          "[=] Position %d", ourPosition);
            ImVec2 ts = ImGui::CalcTextSize(badgeText);
            float  bW = ts.x + 14.f;
            ImGui::SameLine(panelW - bW);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 bp = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(bp,{bp.x+bW,bp.y+bH},C(kCard2));
            dl->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder));
            dl->AddText({bp.x+7.f,bp.y+3.f},C(kMuted),badgeText);
            ImGui::Dummy({bW,bH});
        } else {
            // Без бейджа — держим резервируемую высоту на той же строке, что и заголовок
            // секции (как и в ветке с бейджем), а не на новой строке ниже него.
            ImGui::SameLine(panelW - 1.f);
            ImGui::Dummy({1.f, bH});
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ── Waiting overlay ────────────────────────────────────────────────────
    if (!gameStarted) {
        ImGui::Spacing();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float  aW = rW_ref;
        float  aH = 280.f;
        dl->AddRectFilled(cp,{cp.x+aW,cp.y+aH},Ca(kCard2,0.3f));
        dl->AddRect      (cp,{cp.x+aW,cp.y+aH},C(kBorder));

        bool hasPlayer_ = false;
        { std::lock_guard<std::mutex> lk(g_player.mtx); hasPlayer_ = g_player.hasPlayer; }
        const char* msg  = hasPlayer_ ? "Waiting for a game..." : "Enter Friend ID";
        const char* msg2 = hasPlayer_ ? "" : "to start recommendations";

        ImVec2 ts  = ImGui::CalcTextSize(msg);
        ImVec2 ts2 = ImGui::CalcTextSize(msg2);
        float  cy  = cp.y + aH*0.5f - lh*(msg2[0] ? 1.0f : 0.5f);
        dl->AddText({cp.x+(aW-ts.x)*0.5f,  cy},       C(kMuted),       msg);
        if (msg2[0])
            dl->AddText({cp.x+(aW-ts2.x)*0.5f, cy+lh+2.f}, Ca(kMuted,0.6f), msg2);
        ImGui::Dummy({aW,aH});
        return;
    }

    // Заголовки колонок
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+4.f);
    ImGui::TextUnformatted("#  Hero");
    {
        float colX  = rW_ref - WIN_COL_W;
        ImVec2 ts   = ImGui::CalcTextSize("Win Chance");
        float textX = colX + (WIN_COL_W - ts.x)*0.5f;
        ImGui::SameLine(textX);
        ImGui::TextUnformatted("Win Chance");
    }
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Фиксированные позиции колонок ────────────────────────────────────
    // rankX  : +4   (ширина ~22)
    // portX  : +28  (портрет 34px)
    // nameX  : +70  (фиксирован, все имена стартуют здесь)
    // statsX : 47% от ширины строки (фиксирован, независимо от имени)
    // winX   : rW - WIN_COL_W

    // Отрисовка строки рекомендации
    auto DrawPickRow = [&](int rank, const char* name, int heroId, float win, bool highlight,
                           int gamesPlayer, float wrPlayer, int gamesImm, float wrImm)
    {
        ImDrawList* dl  = ImGui::GetWindowDrawList();
        ImVec2      rp  = ImGui::GetCursorScreenPos();
        const float lh2 = ImGui::GetTextLineHeight();
        const float RH  = lh2 * 2.9f;    // ~64px at lh=22
        const float PSZ = lh2 * 2.0f;    // ~44px
        const float rW  = rW_ref;
        ImVec4      wc  = WinColor(win);

        // Фиксированные X-позиции (lh-based)
        const float portX  = (rank > 0) ? rp.x + lh2 * 1.8f : rp.x + lh2 * 0.3f;
        const float nameX  = portX + PSZ + lh2 * 0.5f;
        const float statsX = rp.x + rW * 0.47f;
        const float winX   = rp.x + rW - WIN_COL_W;

        // Фон строки
        dl->AddRectFilled(rp,{rp.x+rW,rp.y+RH},C(kCard));
        if (highlight)
            dl->AddRect(rp,{rp.x+rW,rp.y+RH},Ca(wc,0.6f),0.f,0,1.5f);
        else
            dl->AddRect(rp,{rp.x+rW,rp.y+RH},C(kBorder));

        // Колонка 1: ранк
        if (rank > 0) {
            char rb[4]; std::snprintf(rb,sizeof(rb),"%2d",rank);
            dl->AddText({rp.x+4.f, rp.y+(RH-lh2)*0.5f}, C(kMuted), rb);
        }

        // Колонка 2: портрет
        ImVec2 pp  = {portX, rp.y+(RH-PSZ)*0.5f};
        ImU32  fill = highlight ? Ca(wc,0.20f) : C(kCard2);
        ImU32  bord = highlight ? Ca(wc,0.45f) : Ca(kText,0.12f);
        char   ab[3] = {name[0], name[1] ? name[1] : '\0', '\0'};
        ImTextureID htex = 0;
        auto hit = g_heroPortraits.find(heroId);
        if (hit != g_heroPortraits.end()) htex = (ImTextureID)hit->second;
        DrawPortrait(dl, pp, PSZ, fill, bord, ab, htex);

        // Колонка 3: имя героя (фиксированный x, клипп до statsX)
        dl->PushClipRect({nameX, rp.y}, {statsX - 6.f, rp.y+RH}, true);
        dl->AddText({nameX, rp.y+(RH-lh2)*0.5f}, C(kText), name);
        dl->PopClipRect();

        // Колонка 4: статистика (фиксированный x=statsX)
        if (gamesPlayer > 0 || gamesImm > 0) {
            char sub[64] = {};
            int  n = 0;
            if (gamesPlayer > 0)
                n += std::snprintf(sub+n, sizeof(sub)-n,
                                   "you %dg %.0f%%", gamesPlayer, wrPlayer*100.f);
            if (gamesImm > 0) {
                if (n > 0) n += std::snprintf(sub+n, sizeof(sub)-n,
                                              "  imm %.0f%%", wrImm*100.f);
                else       std::snprintf(sub+n, sizeof(sub)-n,
                                         "imm %.0f%%", wrImm*100.f);
            }
            dl->PushClipRect({statsX, rp.y}, {winX - 4.f, rp.y+RH}, true);
            dl->AddText({statsX, rp.y+(RH-lh2)*0.5f}, Ca(kMuted,0.85f), sub);
            dl->PopClipRect();
        }

        // Колонка 5: Win% + бар
        char   wb[8]; std::snprintf(wb,sizeof(wb),"%.1f%%", win*100.f);
        ImVec2 wts = ImGui::CalcTextSize(wb);
        dl->AddText({winX+(WIN_COL_W-wts.x)*0.5f, rp.y+5.f}, C(wc), wb);
        float barX = winX + (WIN_COL_W-BAR_W)*0.5f;
        DrawBar(dl, {barX, rp.y+lh2+12.f}, BAR_W, 4.f, win, C(wc));

        ImGui::Dummy({rW,RH});
        ImGui::Spacing();
    };

    if (ourHeroPicked) {
        // Наш герой + P(win)
        DrawPickRow(0, ourHeroName, ourHeroId, winProb, true, 0,0,0,0);
    } else {
        // Top-10 рекомендаций
        for (int i=0;i<recCount && i<10;i++) {
            const PickRowGui& r = recs[i];
            DrawPickRow(r.rank, r.name, r.heroId, r.winProb, r.rank==1,
                        r.gamesPlayer, r.wrPlayer, r.gamesImm, r.wrImm);
        }
        if (recCount == 0) {
            bool hasPlayer_ = false;
            { std::lock_guard<std::mutex> lk(g_player.mtx); hasPlayer_ = g_player.hasPlayer; }
            ImGui::TextColored(kMuted, hasPlayer_
                ? "  Computing recommendations..."
                : "  Enter Friend ID for recommendations");
        }
    }

    // Легенда цветов
    ImGui::Separator();
    ImGui::Spacing();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 fp  = ImGui::GetCursorScreenPos();
        const float lhL = ImGui::GetTextLineHeight();
        const float dotSz = lhL * 0.55f;
        const float dotY  = fp.y + (lhL - dotSz) * 0.5f;
        const float gap   = lhL * 0.5f;

        struct LegItem { ImVec4 col; const char* txt; };
        LegItem items[] = {
            {kGreen, ">=55%"},
            {kAmber, "50-55%"},
            {kRed,   "<50%"}
        };

        float x = fp.x + 4.f;
        for (auto& it : items) {
            dl->AddRectFilled({x, dotY}, {x+dotSz, dotY+dotSz}, C(it.col));
            x += dotSz + 4.f;
            ImVec2 ts = ImGui::CalcTextSize(it.txt);
            dl->AddText({x, fp.y}, Ca(kMuted, 0.8f), it.txt);
            x += ts.x + gap * 2.f;
        }
        ImGui::Dummy({rW_ref, lhL + 4.f});
    }
}

// ─── Панель "Meta Heroes" (живая стата по героям из STRATZ, без ML) ───────────
// Позиция всегда авто: следует за позицией игрока из драфта; если позиция не определена — агрегат по всем.
static void DrawMetaHeroesPanel(float panelW) {
    loadMetaHeroStatsIfNeeded();

    const float lh        = ImGui::GetTextLineHeight();
    const float WIN_COL_W = 88.f;
    const float BAR_W     = 68.f;
    const float rW_ref    = panelW;

    int ourPosition = 0;
    { std::lock_guard<std::mutex> lk(g_pickerState.mtx); ourPosition = g_pickerState.ourPosition; }

    int effectivePos = (ourPosition > 0) ? ourPosition : 0; // 0 = агрегат по всем позициям

    SectionLabel("META HEROES");

    // Position badge — то же оформление, что и в DrawPicksPanel
    {
        char badgeText[32];
        if (effectivePos > 0)
            std::snprintf(badgeText, sizeof(badgeText), "[=] Position %d", effectivePos);
        else
            std::snprintf(badgeText, sizeof(badgeText), "[=] All Positions");
        ImVec2 ts = ImGui::CalcTextSize(badgeText);
        float  bW = ts.x + 14.f;
        float  bH = lh + 6.f;
        ImGui::SameLine(panelW - bW);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(bp,{bp.x+bW,bp.y+bH},C(kCard2));
        dl->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder));
        dl->AddText({bp.x+7.f,bp.y+3.f},C(kMuted),badgeText);
        ImGui::Dummy({bW,bH});
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (!g_metaHeroStatsLoaded) {
        ImGui::TextColored(kMuted, "  Loading meta stats...");
        return;
    }

    auto it = g_metaHeroStats.find(effectivePos);
    if (it == g_metaHeroStats.end() || it->second.empty()) {
        ImGui::TextColored(kMuted, "  No data for this position yet");
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+4.f);
    ImGui::TextUnformatted("#  Hero");
    {
        float colX  = rW_ref - WIN_COL_W;
        ImVec2 ts   = ImGui::CalcTextSize("Win Rate");
        float textX = colX + (WIN_COL_W - ts.x)*0.5f;
        ImGui::SameLine(textX);
        ImGui::TextUnformatted("Win Rate");
    }
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    auto DrawMetaRow = [&](int rank, const MetaHeroRow& row) {
        ImDrawList* dl  = ImGui::GetWindowDrawList();
        ImVec2      rp  = ImGui::GetCursorScreenPos();
        const float lh2 = ImGui::GetTextLineHeight();
        const float RH  = lh2 * 2.9f;
        const float PSZ = lh2 * 2.0f;
        const float rW  = rW_ref;
        ImVec4      wc  = WinColor(row.winRate);

        const float portX  = rp.x + lh2 * 1.8f;
        const float nameX  = portX + PSZ + lh2 * 0.5f;
        const float statsX = rp.x + rW * 0.47f;
        const float winX   = rp.x + rW - WIN_COL_W;

        dl->AddRectFilled(rp,{rp.x+rW,rp.y+RH},C(kCard));
        dl->AddRect(rp,{rp.x+rW,rp.y+RH},C(kBorder));

        char rb[4]; std::snprintf(rb,sizeof(rb),"%2d",rank);
        dl->AddText({rp.x+4.f, rp.y+(RH-lh2)*0.5f}, C(kMuted), rb);

        ImVec2 pp  = {portX, rp.y+(RH-PSZ)*0.5f};
        ImU32  fill = C(kCard2);
        ImU32  bord = Ca(kText,0.12f);
        char   ab[3] = {row.name[0], row.name[1] ? row.name[1] : '\0', '\0'};
        ImTextureID htex = 0;
        auto hit = g_heroPortraits.find(row.heroId);
        if (hit != g_heroPortraits.end()) htex = (ImTextureID)hit->second;
        DrawPortrait(dl, pp, PSZ, fill, bord, ab, htex);

        dl->PushClipRect({nameX, rp.y}, {statsX - 6.f, rp.y+RH}, true);
        dl->AddText({nameX, rp.y+(RH-lh2)*0.5f}, C(kText), row.name);
        dl->PopClipRect();

        char sub[32]; std::snprintf(sub,sizeof(sub),"%d games", row.games);
        dl->PushClipRect({statsX, rp.y}, {winX - 4.f, rp.y+RH}, true);
        dl->AddText({statsX, rp.y+(RH-lh2)*0.5f}, Ca(kMuted,0.85f), sub);
        dl->PopClipRect();

        char   wb[8]; std::snprintf(wb,sizeof(wb),"%.1f%%", row.winRate*100.f);
        ImVec2 wts = ImGui::CalcTextSize(wb);
        dl->AddText({winX+(WIN_COL_W-wts.x)*0.5f, rp.y+5.f}, C(wc), wb);
        float barX = winX + (WIN_COL_W-BAR_W)*0.5f;
        DrawBar(dl, {barX, rp.y+lh2+12.f}, BAR_W, 4.f, row.winRate, C(wc));

        ImGui::Dummy({rW,RH});
        ImGui::Spacing();
    };

    auto& rows = it->second;
    int n = (int)std::min(rows.size(), (size_t)10);
    for (int i=0;i<n;i++) DrawMetaRow(i+1, rows[i]);

    // Легенда цветов — то же оформление, что и в DrawPicksPanel
    ImGui::Separator();
    ImGui::Spacing();
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 fp  = ImGui::GetCursorScreenPos();
        const float lhL = ImGui::GetTextLineHeight();
        const float dotSz = lhL * 0.55f;
        const float dotY  = fp.y + (lhL - dotSz) * 0.5f;
        const float gap   = lhL * 0.5f;

        struct LegItem { ImVec4 col; const char* txt; };
        LegItem items[] = {
            {kGreen, ">=55%"},
            {kAmber, "50-55%"},
            {kRed,   "<50%"}
        };

        float x = fp.x + 4.f;
        for (auto& li : items) {
            dl->AddRectFilled({x, dotY}, {x+dotSz, dotY+dotSz}, C(li.col));
            x += dotSz + 4.f;
            ImVec2 ts = ImGui::CalcTextSize(li.txt);
            dl->AddText({x, fp.y}, Ca(kMuted, 0.8f), li.txt);
            x += ts.x + gap * 2.f;
        }
        ImGui::Dummy({rW_ref, lhL + 4.f});
    }
}

// ─── Полоса статуса (Data + Game phase + match ID) ────────────────────────────
static void DrawStatusBar(float fullW) {
    // Снимок состояния player state
    bool   phase1Running, phase1Done, phase1Error;
    char   phase1Msg[256];
    bool   hasPlayer;
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        phase1Running = g_player.phase1Running;
        phase1Done    = g_player.phase1Done;
        phase1Error   = g_player.phase1Error;
        std::memcpy(phase1Msg, g_player.phase1Msg, sizeof(phase1Msg));
        hasPlayer = g_player.hasPlayer;
    }
    GamePhase phase;
    std::string matchId;
    {
        std::lock_guard<std::mutex> lk(g_gameInfo.mtx);
        phase   = g_gameInfo.phase;
        matchId = g_gameInfo.matchId;
    }

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      sp  = ImGui::GetCursorScreenPos();
    const float H   = 26.f;
    const float lh  = ImGui::GetTextLineHeight();
    const float ty  = sp.y + (H-lh)*0.5f;

    dl->AddRectFilled(sp,{sp.x+fullW,sp.y+H},Ca(kCard2,0.5f));

    const float dotR  = 4.f;
    const float dotCY = ty + lh * 0.5f;

    // Player data dot + text
    float refreshEndX = 0.f;
    float playerEndX = sp.x + 10.f;
    {
        ImVec4 dot1col = !hasPlayer ? kMuted :
            (phase1Error ? kRed : (phase1Done ? kGreen : (phase1Running ? kAmber : kMuted)));
        dl->AddCircleFilled({sp.x+10.f, dotCY}, dotR, C(dot1col));
        char p1buf[64];
        const char* p1status = !hasPlayer ? "no ID" :
            (phase1Running ? "fetching..." : (phase1Done ? "ready" : (phase1Error ? "error" : "pending")));
        std::snprintf(p1buf, sizeof(p1buf), " Player data: %s", p1status);
        dl->AddText({sp.x+18.f, ty}, C(kMuted), p1buf);
        ImVec2 p1sz = ImGui::CalcTextSize(p1buf);
        playerEndX = sp.x + 18.f + p1sz.x;
    }
    if (!phase1Running && hasPlayer) {
        float btnSz = H - 2.f;
        float btnX = playerEndX + 4.f;
        float cx = btnX + btnSz * 0.5f;
        float cy = sp.y + H * 0.5f;
        ImGui::SetCursorScreenPos({btnX, sp.y + 1.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(1,1,1,0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(1,1,1,0.2f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, btnSz * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0,0});
        bool clicked = ImGui::Button("##refresh", {btnSz, btnSz});
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        // Иконка refresh: две дуги + наконечники
        constexpr float PI = 3.14159265f;
        float r = btnSz * 0.34f;
        ImU32 col = ImGui::IsItemHovered() ? IM_COL32(220,220,220,255) : C(kMuted);

        // Дуга 1: верх (10 → 2 часа по часовой)
        dl->PathArcTo({cx,cy}, r, 200.f*PI/180.f, 340.f*PI/180.f, 12);
        dl->PathStroke(col, 0, 2.f);
        // Дуга 2: низ (4 → 8 часов по часовой)
        dl->PathArcTo({cx,cy}, r, 20.f*PI/180.f, 160.f*PI/180.f, 12);
        dl->PathStroke(col, 0, 2.f);

        // Наконечник на 2 часа (конец дуги 1)
        auto drawArrow = [&](float angleDeg) {
            float ae  = angleDeg * PI / 180.f;
            float dir = ae + PI * 0.5f;
            float tx  = cx + r * cosf(ae);
            float tty = cy + r * sinf(ae);
            float len = r * 0.5f, hw = len * 0.45f;
            float bx  = tx - len * cosf(dir), by = tty - len * sinf(dir);
            dl->AddTriangleFilled({tx, tty},
                {bx + hw*cosf(dir+PI/2.f), by + hw*sinf(dir+PI/2.f)},
                {bx + hw*cosf(dir-PI/2.f), by + hw*sinf(dir-PI/2.f)}, col);
        };
        drawArrow(340.f);
        drawArrow(160.f);

        if (clicked) {
            long long aid = 0;
            { std::lock_guard<std::mutex> lk(g_player.mtx); aid = g_player.accountId; }
            if (aid > 0) startPhase1(aid);
        }
        refreshEndX = btnX + btnSz;
    }

    // Game phase dot + text
    ImVec4 dot2col = (phase == GamePhase::DRAFT   ? kAmber  :
                      phase == GamePhase::INGAME  ? kGreen  :
                      phase == GamePhase::POSTGAME? kMuted  : kMuted);
    float x2 = refreshEndX > 0.f
        ? refreshEndX + 12.f
        : playerEndX + 12.f;
    dl->AddCircleFilled({x2+4.f, dotCY}, dotR, C(dot2col));
    const char* phaseStr = (phase == GamePhase::IDLE)     ? "Waiting for game" :
                           (phase == GamePhase::DRAFT)    ? "Draft Phase" :
                           (phase == GamePhase::INGAME)   ? "In Game" : "Post Game";
    char p2buf[64];
    std::snprintf(p2buf, sizeof(p2buf), " Game: %s", phaseStr);
    dl->AddText({x2+12.f, ty}, C(kMuted), p2buf);

    // Match ID (right side)
    if (!matchId.empty()) {
        char mbuf[64];
        std::snprintf(mbuf, sizeof(mbuf), "match %s", matchId.c_str());
        ImVec2 mts = ImGui::CalcTextSize(mbuf);
        dl->AddText({sp.x+fullW-mts.x-8.f, ty}, Ca(kMuted,0.6f), mbuf);
    }

    // Зафиксировать курсор после статус-бара (Button внутри сдвигает layout)
    ImGui::SetCursorScreenPos({sp.x, sp.y + H});
    ImGui::Dummy({fullW, 0.f});
}

// ─── Шапка: логотип [D] + заголовок + карточка игрока / ввод Friend ID ────────
static void DrawHeader(float fullW) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      hs  = ImGui::GetCursorScreenPos();
    const float H   = 60.f;
    const float lh  = ImGui::GetTextLineHeight();

    // ── [D] logo box (на всю высоту шапки) ─────────────────────────────
    const float LOGO_W = H;
    ImVec2 logoPos = {hs.x, hs.y};

    dl->AddRectFilled(logoPos, {logoPos.x+LOGO_W, logoPos.y+H}, C(kCard));
    dl->AddRect      (logoPos, {logoPos.x+LOGO_W, logoPos.y+H}, C(kBorder));
    ImVec2 sts = ImGui::CalcTextSize("[D]");
    dl->AddText({logoPos.x+(LOGO_W-sts.x)/2.f,
                 logoPos.y+(H-sts.y)/2.f - 1.f}, C(kText), "[D]");

    // Invisible button over the [D] box
    ImGui::SetCursorScreenPos(logoPos);
    if (ImGui::InvisibleButton("##logo_btn",{LOGO_W, H})) {
        if (g_Hwnd) {
            if (IsIconic(g_Hwnd)) ShowWindow(g_Hwnd, SW_RESTORE);
            SetForegroundWindow(g_Hwnd);
            BringWindowToTop(g_Hwnd);
        }
    }

    // ── Title (вертикально по центру шапки, две строки) ───────────────────
    float tx       = hs.x + LOGO_W + 10.f;
    float textH    = lh * 2.f + 2.f;
    float titleY   = hs.y + (H - textH) * 0.5f;
    dl->AddText({tx, titleY},            C(kText),  "Dota_Drafter");
    dl->AddText({tx, titleY + lh + 2.f}, C(kMuted), "Dota 2 Draft Analyzer - Live Overlay");

    // ── Player card (right side) ──────────────────────────────────────────
    const float CW = 240.f;
    float cx = hs.x + fullW - CW;

    dl->AddRectFilled({cx,hs.y},    {cx+CW,hs.y+H}, C(kCard));
    dl->AddRect      ({cx,hs.y},    {cx+CW,hs.y+H}, C(kBorder));

    // Avatar box (36×36, вертикально по центру карточки)
    const float AVS  = 36.f;
    const float avY  = hs.y + (H - AVS) * 0.5f;
    dl->AddRectFilled({cx+8, avY}, {cx+8+AVS, avY+AVS}, C(kCard2));
    dl->AddRect      ({cx+8, avY}, {cx+8+AVS, avY+AVS}, C(kBorder));

    // Снимок состояния player info
    long long  accountId;
    char       pname[128];
    bool       hasPlayer;
    bool       phase1Running;
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        accountId    = g_player.accountId;
        hasPlayer    = g_player.hasPlayer;
        phase1Running= g_player.phase1Running;
        std::memcpy(pname, g_player.name, sizeof(pname));
    }

    if (!hasPlayer || s_editMode) {
        // ── Input mode ────────────────────────────────────────────────────
        ImVec2 avts = ImGui::CalcTextSize("ID");
        dl->AddText({cx+8+(AVS-avts.x)/2.f, avY+(AVS-avts.y)/2.f},
                    C(kMuted), "ID");

        dl->AddText({cx+52.f, avY}, C(kMuted), "Enter Friend ID:");

        // InputText + Set button
        ImGui::SetCursorScreenPos({cx+52.f, avY+lh+4.f});
        ImGui::SetNextItemWidth(120.f);
        bool commit = ImGui::InputText("##steamid", s_inputBuf, sizeof(s_inputBuf),
                                       ImGuiInputTextFlags_CharsNoBlank |
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, 4.f);
        bool setBtn = ImGui::Button("Set");

        if ((commit || setBtn) && s_inputBuf[0] != '\0') {
            long long newId = 0;
            try { newId = std::stoll(s_inputBuf); } catch (...) {}
            if (newId > 0) {
                // Update player state immediately
                {
                    std::lock_guard<std::mutex> lk(g_player.mtx);
                    g_player.accountId     = newId;
                    g_player.hasPlayer     = true;
                    g_player.idChanged     = true;   // сигнал оркестратору
                    g_player.phase1Done    = false;
                    g_player.phase1Error   = false;
                    std::snprintf(g_player.name, sizeof(g_player.name),
                                  "ID %lld", newId);
                }
                s_editMode    = false;
                s_inputBuf[0] = '\0';
                startPhase1(newId);
            }
        }

        // Cancel button (only in edit mode with existing player)
        if (s_editMode && hasPlayer) {
            ImGui::SameLine(0,4.f);
            if (ImGui::SmallButton("x")) s_editMode = false;
        }

    } else {
        // ── Display mode ──────────────────────────────────────────────────
        if (g_avatarSRV) {
            dl->AddImage((ImTextureID)g_avatarSRV,
                         {cx+8.f, avY}, {cx+8.f+AVS, avY+AVS});
        } else {
            char av[3] = {pname[0] ? pname[0] : '?',
                          pname[1] ? pname[1] : '\0', '\0'};
            ImVec2 avts = ImGui::CalcTextSize(av);
            dl->AddText({cx+8+(AVS-avts.x)/2.f, avY+(AVS-avts.y)/2.f},
                        C(kMuted), av);
        }

        // Player name + ID (вертикально по центру карточки)
        float nameBlockH = lh * 2.f + 2.f;
        float nameY      = hs.y + (H - nameBlockH) * 0.5f;
        dl->AddText({cx+52.f, nameY},            C(kText), pname);
        char idStr[32];
        std::snprintf(idStr, sizeof(idStr), "ID %lld", accountId);
        dl->AddText({cx+52.f, nameY+lh+2.f}, C(kMuted), idStr);

        // Invisible button over name area to trigger edit
        ImGui::SetCursorScreenPos({cx+52.f, nameY});
        if (ImGui::InvisibleButton("##player_edit",{CW-60.f,lh*2.f+8.f})) {
            s_editMode = true;
            std::snprintf(s_inputBuf, sizeof(s_inputBuf), "%lld", accountId);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to change player ID");

        // Phase-1 spinner
        if (phase1Running) {
            static float spin = 0.f;
            spin += ImGui::GetIO().DeltaTime * 3.f;
            char sp[4] = {"|/-\\"[(int)(spin)%4]};
            dl->AddText({cx+CW-20.f, hs.y+H*0.5f-lh*0.5f},
                        C(kAmber), sp);
        }
    }

    ImGui::SetCursorScreenPos({hs.x, hs.y + H});
    ImGui::Dummy({fullW, 0.f});
}

// ─── Главный кадр ────────────────────────────────────────────────────────────
static void RenderFrame() {
    // Загрузка аватара из буфера (GUI-поток → D3D11 текстура)
    {
        bool ready = false;
        { std::lock_guard<std::mutex> lk(g_player.mtx); ready = g_player.avatarDataReady; }
        if (ready) {
            std::vector<uint8_t> data;
            {
                std::lock_guard<std::mutex> lk(g_player.mtx);
                data = std::move(g_player.avatarData);
                g_player.avatarDataReady = false;
            }
            if (g_avatarSRV) { g_avatarSRV->Release(); g_avatarSRV = nullptr; }
            g_avatarSRV = createTextureFromImageData(data.data(), data.size());
        }
    }

    auto&       io   = ImGui::GetIO();
    const float W    = io.DisplaySize.x;
    const float H    = io.DisplaySize.y;
    const float PAD  = 8.f;
    const float FULL = W - PAD*2.f;

    ImGui::SetNextWindowPos({0,0});
    ImGui::SetNextWindowSize({W,H});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBg);
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos({PAD, PAD});

    DrawHeader(FULL);

    // Единый отступ между секциями
    const float GAP = PAD;

    ImGui::SetCursorPos({PAD, PAD + 60.f + GAP});
    DrawStatusBar(FULL);
    ImGui::SetCursorPos({PAD, PAD + 60.f + GAP + 26.f + GAP});

    // ── Баннеры совместимости ────────────────────────────────────────────
    {
        bool schemaErr = false;
        char msg[256] = {};
        {
            std::lock_guard<std::mutex> lk(g_pickerState.mtx);
            schemaErr = g_pickerState.schemaError;
            if (schemaErr) std::memcpy(msg, g_pickerState.schemaMsg, sizeof(msg));
        }
        if (schemaErr) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.8f,0.f,1.f));
            ImGui::TextWrapped("%s", msg);
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        if (g_schemaBannerNeeded) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.8f,0.f,1.f));
            ImGui::TextWrapped("A newer data version is available but requires an app update.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
    }

    float usedH    = ImGui::GetCursorPosY();
    float contentH = H - usedH - PAD;
    float leftW    = FULL * 0.46f;
    float restW    = FULL - leftW - 16.f;   // 2 gaps of 8px
    float midW     = restW * 0.5f;
    float rightW   = restW - midW;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##draft",{leftW,contentH},true,ImGuiWindowFlags_NoScrollbar);
    ImGui::Spacing();
    DrawDraftPanel(ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 8.f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##picks",{midW,contentH},true,ImGuiWindowFlags_NoScrollbar);
    ImGui::Spacing();
    DrawPicksPanel(ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 8.f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##meta",{rightW,contentH},true,ImGuiWindowFlags_NoScrollbar);
    ImGui::Spacing();
    DrawMetaHeroesPanel(ImGui::GetContentRegionAvail().x);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::End();
}

static void PresentFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderFrame();
    ImGui::Render();
    const float bg[4] = {0.04f,0.04f,0.04f,1.f};
    g_Context->OMSetRenderTargets(1, &g_RTV, nullptr);
    g_Context->ClearRenderTargetView(g_RTV, bg);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_SwapChain->Present(1, 0);
}

// ─── Иконка приложения [D] (программно через GDI) ────────────────────────────
static HICON g_AppIcon = nullptr;

static HICON CreateDIcon(int sz) {
    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);
    HBITMAP hBmp = CreateCompatibleBitmap(screenDC, sz, sz);
    HBITMAP hOld = (HBITMAP)SelectObject(memDC, hBmp);

    RECT r = {0, 0, sz, sz};
    HBRUSH bg = CreateSolidBrush(RGB(10, 10, 10));
    FillRect(memDC, &r, bg);
    DeleteObject(bg);

    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(220, 220, 220));
    int fs = (std::max)(8, (int)(sz * 0.45f));
    HFONT f  = CreateFontW(fs, 0, 0, 0, FW_BOLD, 0, 0, 0,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT of = (HFONT)SelectObject(memDC, f);
    DrawTextW(memDC, L"[D]", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(memDC, of);
    DeleteObject(f);

    SelectObject(memDC, hOld);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    HBITMAP hMask = CreateBitmap(sz, sz, 1, 1, nullptr);
    ICONINFO ii   = {};
    ii.fIcon      = TRUE;
    ii.hbmColor   = hBmp;
    ii.hbmMask    = hMask;
    HICON icon    = CreateIconIndirect(&ii);
    DeleteObject(hBmp);
    DeleteObject(hMask);
    return icon;
}

// ─── Окно обновления (до D3D11/ImGui) ────────────────────────────────────────

static HWND  g_updateWnd      = nullptr;
static HWND  g_updateLabel    = nullptr;
static HWND  g_updatePercent  = nullptr;
static HWND  g_updateBtn      = nullptr;
static bool  g_retryClicked   = false;
static constexpr int IDC_RETRY_BTN = 101;

static LRESULT CALLBACK UpdateWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RETRY_BTN) g_retryClicked = true;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        HBRUSH br = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdc, &ps.rcPaint, br);
        DeleteObject(br);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(220, 220, 220));
        SetBkColor(hdc, RGB(20, 20, 20));
        static HBRUSH bg = CreateSolidBrush(RGB(20, 20, 20));
        return (LRESULT)bg;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void CreateUpdateWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc   = UpdateWndProc;
    wc.hInstance      = hInst;
    wc.lpszClassName  = L"DotaDrafterUpdate";
    wc.hbrBackground  = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 420, h = 170;

    g_updateWnd = CreateWindowExW(0,
        L"DotaDrafterUpdate", L"Dota Drafter - Update",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sw - w) / 2, (sh - h) / 2, w, h,
        nullptr, nullptr, hInst, nullptr);

    // Тёмная тема
    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_updateWnd, 20, &dark, sizeof(dark));
    COLORREF capColor  = RGB(12, 12, 12);
    COLORREF textColor = RGB(240, 240, 240);
    COLORREF borColor  = RGB(99, 199, 118);
    DwmSetWindowAttribute(g_updateWnd, 35, &capColor,  sizeof(capColor));
    DwmSetWindowAttribute(g_updateWnd, 36, &textColor, sizeof(textColor));
    DwmSetWindowAttribute(g_updateWnd, 34, &borColor,  sizeof(borColor));

    // Иконка
    HICON iconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    HICON iconBg = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
    if (iconSm) SendMessage(g_updateWnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSm);
    if (iconBg) SendMessage(g_updateWnd, WM_SETICON, ICON_BIG,   (LPARAM)iconBg);

    RECT cr;
    GetClientRect(g_updateWnd, &cr);
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;

    HFONT font = CreateFontW(30, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fontPercent = CreateFontW(35, 0, 0, 0, FW_BOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    g_updateLabel = CreateWindowW(L"STATIC",
        L"Checking for updates...",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        5, 8, cw - 10, 40, g_updateWnd, nullptr, hInst, nullptr);
    SendMessage(g_updateLabel, WM_SETFONT, (WPARAM)font, TRUE);

    g_updatePercent = CreateWindowW(L"STATIC",
        L"",
        WS_CHILD | SS_CENTER,
        5, 45, cw - 10, 50, g_updateWnd, nullptr, hInst, nullptr);
    SendMessage(g_updatePercent, WM_SETFONT, (WPARAM)fontPercent, TRUE);

    HFONT btnFont = CreateFontW(24, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_updateBtn = CreateWindowW(L"BUTTON",
        L"Try again",
        WS_CHILD | BS_PUSHBUTTON | BS_FLAT,
        (cw - 180) / 2, ch - 50, 180, 42,
        g_updateWnd, (HMENU)(INT_PTR)IDC_RETRY_BTN, hInst, nullptr);
    SendMessage(g_updateBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);

    ShowWindow(g_updateWnd, SW_SHOW);
    UpdateWindow(g_updateWnd);
}

static void PumpMessages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
}

static void SetUpdateStatus(const wchar_t* text) {
    if (g_updateLabel) SetWindowTextW(g_updateLabel, text);
    if (g_updatePercent) { SetWindowTextW(g_updatePercent, L""); ShowWindow(g_updatePercent, SW_HIDE); }
    if (g_updateWnd) RedrawWindow(g_updateWnd, nullptr, nullptr,
                                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    PumpMessages();
}

static void SetUpdateProgress(const wchar_t* label, int percent) {
    static int lastPercent = -1;
    static const wchar_t* lastLabel = nullptr;

    // Обновить заголовок только если изменился
    if (label != lastLabel) {
        lastLabel = label;
        if (g_updateLabel) SetWindowTextW(g_updateLabel, label);
    }
    if (g_updatePercent) {
        ShowWindow(g_updatePercent, SW_SHOW);
        if (percent != lastPercent) {
            lastPercent = percent;
            wchar_t buf[32];
            swprintf_s(buf, L"%d%%", percent);
            SetWindowTextW(g_updatePercent, buf);
        }
    }
    PumpMessages();
}

static void ShowRetryButton(bool show) {
    if (g_updateBtn) ShowWindow(g_updateBtn, show ? SW_SHOW : SW_HIDE);
    g_retryClicked = false;
}

static void DestroyUpdateWindow() {
    if (g_updateWnd) { DestroyWindow(g_updateWnd); g_updateWnd = nullptr; }
    g_updateLabel   = nullptr;
    g_updatePercent = nullptr;
    g_updateBtn   = nullptr;
}

static bool WaitForRetryClick() {
    g_retryClicked = false;
    MSG msg;
    while (!g_retryClicked) {
        if (GetMessage(&msg, nullptr, 0, 0) <= 0) return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT) return false;
    }
    return true;
}

// ─── Точка входа ─────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Рабочая директория = папка с моделью (exe dir или parent, если exe в build/)
    {
        wchar_t exeDir[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
            wchar_t* slash = wcsrchr(exeDir, L'\\');
            if (slash) *slash = L'\0';
            SetCurrentDirectoryW(exeDir);
            if (GetFileAttributesA("draft_helper_abstract.cbm") == INVALID_FILE_ATTRIBUTES) {
                wchar_t* parent = wcsrchr(exeDir, L'\\');
                if (parent) { *parent = L'\0'; SetCurrentDirectoryW(exeDir); }
            }
        }
    }

    // DPI-осведомлённость
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI* SPDA_t)(void*);
        if (auto fn = (SPDA_t)GetProcAddress(u32,"SetProcessDpiAwarenessContext"))
            fn((void*)(intptr_t)-4);
    }

    ensureLogsDir();
    CurlGlobal curlInit;

    // GDI+ для декодирования аватаров (JPEG/PNG)
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    // ── Update check (blocking, before any threads) ────────────────────────
    {
        checkPendingSwap();
        cleanupStaging();

        CreateUpdateWindow(hInst);

        ManifestInfo manifest;

        // Retry loop: app requires internet
        while (true) {
            SetUpdateStatus(L"Checking for updates...");
            ShowRetryButton(false);
            if (fetchManifest(manifest)) break;

            SetUpdateStatus(L"Failed to check for updates");
            ShowRetryButton(true);
            if (!WaitForRetryClick()) {
                DestroyUpdateWindow();
                Gdiplus::GdiplusShutdown(gdipToken);
                return 0;
            }
        }

        UpdateAction action = checkForUpdates(manifest);

        if (action == UpdateAction::APP_UPDATE) {
            SetUpdateStatus(L"Downloading app update...");
            CreateDirectoryA("staging", nullptr);
            std::string stagingPath = "staging\\dota_drafter_setup.exe";

            bool ok = downloadToStaging(manifest.appUrl, manifest.appSha256,
                stagingPath, [](size_t done, size_t total) {
                    if (total > 0)
                        SetUpdateProgress(L"Downloading update...", (int)(done * 100 / total));
                    else
                        SetUpdateProgress(L"Downloading update...", 0);
                });

            if (ok) {
                SetUpdateStatus(L"Installing update...");

                // Написать .bat который ждёт выхода процесса, потом запускает инсталлятор
                char fullStagingPath[MAX_PATH];
                GetFullPathNameA(stagingPath.c_str(), MAX_PATH, fullStagingPath, nullptr);

                std::string batPath = std::string(fullStagingPath) + ".bat";
                {
                    std::ofstream bat(batPath, std::ios::trunc);
                    bat << "@echo off\n";
                    bat << "ping -n 4 127.0.0.1 >nul\n";
                    bat << "\"" << fullStagingPath << "\" /SILENT /SUPPRESSMSGBOXES\n";
                    bat << "del \"%~f0\"\n";
                }

                STARTUPINFOA si = {sizeof(si)};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION pi = {};
                char cmdLine[1024];
                std::snprintf(cmdLine, sizeof(cmdLine), "cmd.exe /c \"%s\"", batPath.c_str());
                if (CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }
                DestroyUpdateWindow();
                Gdiplus::GdiplusShutdown(gdipToken);
                return 0;
            }
            LOG_WARN("App update download/verify failed — sha256 mismatch or network error");
            SetUpdateStatus(L"Update failed, starting current version...");
            Sleep(2000);
        }

        if (action == UpdateAction::DATA_UPDATE) {
            SetUpdateStatus(L"Downloading data update...");
            std::vector<std::string> stagedFiles;
            bool staged = downloadAndStageData(manifest, stagedFiles,
                [](size_t done, size_t total) {
                    if (total > 0)
                        SetUpdateProgress(L"Downloading data...", (int)(done * 100 / total));
                    else
                        SetUpdateProgress(L"Downloading data...", 0);
                });
            if (staged) {
                SetUpdateStatus(L"Applying data update...");
                if (swapDataFiles(manifest, stagedFiles))
                    LOG_INFO("Data updated to version " << manifest.dataVersion);
                else
                    LOG_ERR("Data swap failed, using existing data");
            }
        }

        if (action == UpdateAction::SCHEMA_TOO_NEW) {
            g_schemaBannerNeeded = true;
        }

        DestroyUpdateWindow();
    }

    // ── Config from environment ───────────────────────────────────────────
    if (const char* e = std::getenv("STRATZ_API_KEY")) g_stratzToken = e;
    else g_stratzToken = DEFAULT_STRATZ_TOKEN;

    // ── Load saved player from DB ─────────────────────────────────────────
    {
        sqlite3* db = nullptr;
        if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
            sqlite3_exec(db,"PRAGMA journal_mode=WAL;",nullptr,nullptr,nullptr);
            createPlayerInfoTable(db);
            char savedName[128] = {};
            long long savedId   = loadSavedPlayer(db, savedName);
            if (savedId > 0) {
                std::lock_guard<std::mutex> lk(g_player.mtx);
                g_player.accountId = savedId;
                g_player.hasPlayer = true;
                std::snprintf(g_player.name, sizeof(g_player.name), "%s",
                              savedName[0] ? savedName : "Unknown");
            }
            sqlite3_close(db);
        }
    }

    // ── Фаза 1a: таблицы + справочник героев + мета-стата (фоновый поток) ───
    std::thread([]{ runDataFetcherInit(g_stratzToken); }).detach();

    // ── Start orchestrator thread ─────────────────────────────────────────
    g_orchestratorThread = std::thread(orchestratorMain);

    // Запуск фазы 1b (данные игрока, если сохранён)
    {
        long long savedId  = 0;
        bool      hasPlayer = false;
        {
            std::lock_guard<std::mutex> lk(g_player.mtx);
            savedId   = g_player.accountId;
            hasPlayer = g_player.hasPlayer;
        }
        if (hasPlayer) startPhase1(savedId);
    }

    // ── Create window ─────────────────────────────────────────────────────
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                      hInst, nullptr, nullptr, nullptr, nullptr,
                      L"Dota_Drafter", nullptr};
    RegisterClassExW(&wc);

    // ── Начальный размер окна: 1.5× базового (1200×900), но не больше экрана ─
    int sw   = GetSystemMetrics(SM_CXSCREEN);
    int sh   = GetSystemMetrics(SM_CYSCREEN);
    int winW = (std::min)(1800, (int)(sw * 0.94f));
    int winH = (std::min)(1300, (int)(sh * 0.92f));
    int winX = (sw - winW) / 2;
    int winY = (std::max)(0, (sh - winH) / 2);

    wchar_t titleBuf[128];
    swprintf_s(titleBuf, L"Dota_Drafter v%S - Dota 2 Draft Analyzer", kAppVersion);
    HWND hwnd = CreateWindowW(L"Dota_Drafter",
        titleBuf,
        WS_OVERLAPPEDWINDOW, winX, winY, winW, winH,
        nullptr, nullptr, hInst, nullptr);
    g_Hwnd = hwnd;

    // Тёмная тема окна (DWM)
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    COLORREF capColor  = RGB(12,12,12);
    COLORREF textColor = RGB(240,240,240);
    COLORREF borColor  = RGB(99,199,118);
    DwmSetWindowAttribute(hwnd, 35, &capColor,  sizeof(capColor));
    DwmSetWindowAttribute(hwnd, 36, &textColor, sizeof(textColor));
    DwmSetWindowAttribute(hwnd, 34, &borColor,  sizeof(borColor));

    if (!InitD3D(hwnd)) { DestroyWindow(hwnd); return 1; }

    // ── Icon (16×16 для заголовка, 32×32 для панели задач) ──────────────
    int smSz = GetSystemMetrics(SM_CXSMICON);  // обычно 16
    int bgSz = GetSystemMetrics(SM_CXICON);    // обычно 32
    HICON iconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     smSz ? smSz : 16, smSz ? smSz : 16, 0);
    g_AppIcon    = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     bgSz ? bgSz : 32, bgSz ? bgSz : 32, 0);
    if (!iconSm) iconSm    = CreateDIcon(smSz ? smSz : 16);
    if (!g_AppIcon) g_AppIcon = CreateDIcon(bgSz ? bgSz : 32);
    if (iconSm)    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSm);
    if (g_AppIcon) SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)g_AppIcon);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // Шрифт: Segoe UI 24px — латиница + кириллица + спецсимволы Steam-ников
        float fontSize = 24.f;
        static const ImWchar glyphRanges[] = {
            0x0020, 0x00FF, // Basic Latin + Latin Supplement
            0x0100, 0x024F, // Latin Extended-A/B
            0x0400, 0x052F, // Кириллица + Cyrillic Supplement
            0x2000, 0x206F, // General Punctuation (—, –, …, ′, ″)
            0x2100, 0x214F, // Letterlike Symbols (℃, №, ™, ℠)
            0x2190, 0x21FF, // Arrows (→, ←, ↑, ↓)
            0x2200, 0x22FF, // Math Operators (∞, ≈, ≠, ≤, ≥)
            0x25A0, 0x25FF, // Geometric Shapes (■, □, ▲, ▼, ◆, ●)
            0x2600, 0x26FF, // Misc Symbols (★, ☆, ♥, ♠, ♦, ♣, ☺, ♪, ♫)
            0x2700, 0x27BF, // Dingbats (✓, ✗, ✦, ✧, ✪, ✰)
            0,
        };
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", fontSize, nullptr, glyphRanges))
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", fontSize, nullptr, glyphRanges);
    }
    ApplyStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_Device, g_Context);

    loadHeroPortraits();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            continue;
        }
        PresentFrame();
    }

    // ── Завершение ────────────────────────────────────────────────────────
    g_orchestratorRunning.store(false);
    if (g_orchestratorThread.joinable()) g_orchestratorThread.join();

    if (g_avatarSRV) { g_avatarSRV->Release(); g_avatarSRV = nullptr; }
    for (auto& [k, srv] : g_heroPortraits) if (srv) srv->Release();
    g_heroPortraits.clear();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(L"Dota_Drafter", hInst);
    if (g_AppIcon) DestroyIcon(g_AppIcon);
    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}