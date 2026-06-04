/*
 * mainGUI.cpp
 *
 * Unified: ImGui D3D11 GUI + backend orchestrator (phases 1-3).
 * Replaces: mainGUI.cpp (standalone mockup) + main_unified.cpp (console).
 *
 * Thread model:
 *   GUI thread      — WinMain / message loop / ImGui render
 *   Orchestrator    — watches GameInfo, manages portrait + picker threads
 *   GSI thread      — runGsiServer (always running after player set)
 *   Phase-1 thread  — runDataFetcher (once per player ID)
 *   Portrait thread — runPortraitCapture (HERO_SELECTION + 30s tail)
 *   Picker thread   — runPickerGui (DRAFT / INGAME)
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

#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

// =============================================================================
//  D3D11 boilerplate
// =============================================================================
static ID3D11Device*           g_Device    = nullptr;
static ID3D11DeviceContext*    g_Context   = nullptr;
static IDXGISwapChain*         g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV       = nullptr;
static HWND                    g_Hwnd      = nullptr;

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
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

// =============================================================================
//  Palette
// =============================================================================
static const ImVec4 kBg     = {0.04f, 0.04f, 0.04f, 1.f};
static const ImVec4 kCard   = {0.09f, 0.09f, 0.09f, 1.f};
static const ImVec4 kCard2  = {0.15f, 0.15f, 0.15f, 1.f};
static const ImVec4 kText   = {0.98f, 0.98f, 0.98f, 1.f};
static const ImVec4 kMuted  = {0.63f, 0.63f, 0.63f, 1.f};
static const ImVec4 kBorder = {1.f,   1.f,   1.f,   0.10f};
static const ImVec4 kGreen  = {0.39f, 0.78f, 0.47f, 1.f};
static const ImVec4 kRed    = {0.88f, 0.45f, 0.35f, 1.f};
static const ImVec4 kAmber  = {0.88f, 0.73f, 0.35f, 1.f};
static const ImVec4 kBlue   = {0.35f, 0.60f, 0.90f, 1.f};

inline ImU32 C(ImVec4 v)           { return ImGui::ColorConvertFloat4ToU32(v); }
inline ImU32 Ca(ImVec4 v, float a) { v.w = a; return ImGui::ColorConvertFloat4ToU32(v); }

// =============================================================================
//  Shared cross-thread state
// =============================================================================
static GameInfo            g_gameInfo;
static SharedPortraitState g_portraitState;
static GuiPickerState      g_pickerState;

// Player info — written by background threads, read by GUI
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
};
static PlayerState g_player;

// Config (set once from env/defaults, then read-only)
static std::string g_stratzToken;
static std::string g_steamKey;
static const char* DB_PATH    = "playerandlivestats.db";
static const char* MODEL_PATH = "draft_helper_v3";
// Фаза 3 (портреты + пикер) активна во время HERO_SELECTION + хвост 5 сек.
// После — пикер остановлен, GUI показывает последний результат до конца игры.
static constexpr int PHASE3_TAIL_SEC = 5;

// Thread management
static std::atomic<bool> g_pickerRunning{false};
static std::atomic<bool> g_portraitRunning{false};
static std::atomic<bool> g_orchestratorRunning{true};
static std::thread       g_pickerThread;
static std::thread       g_portraitThread;
static std::thread       g_orchestratorThread;

// GUI-only state (touched only from GUI thread)
static bool  s_editMode     = false;
static char  s_inputBuf[32] = {};
static bool  s_nameFetching = false;
static char  s_fetchedName[128] = {};

// =============================================================================
//  SQLite helpers — player_info table
// =============================================================================
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

// =============================================================================
//  livepicks table
// =============================================================================
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

// =============================================================================
//  STRATZ — fetch player name (runs in a short-lived background thread)
// =============================================================================
static std::string fetchStratzName(long long accountId, const std::string& token) {
    // GraphQL query for player display name
    char qbuf[256];
    std::snprintf(qbuf, sizeof(qbuf),
        "{\"query\":\"{ player(steamAccountId: %lld) "
        "{ steamAccount { name } } }\"}",
        accountId);

    std::string resp = httpPost(
        "https://api.stratz.com/graphql", qbuf, token);
    if (resp.empty()) return "";

    // Simple parse: find "name":"..."
    auto pos = resp.find("\"name\":");
    if (pos == std::string::npos) return "";
    pos += 7;
    while (pos < resp.size() && resp[pos] != '"') ++pos;
    if (pos >= resp.size()) return "";
    ++pos; // skip opening "
    std::string name;
    while (pos < resp.size() && resp[pos] != '"') {
        if (resp[pos] == '\\') { ++pos; }
        if (pos < resp.size()) name += resp[pos++];
    }
    return name;
}

// =============================================================================
//  Phase 1 launcher
// =============================================================================
static void startPhase1(long long accountId) {
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        g_player.phase1Running = true;
        g_player.phase1Done    = false;
        g_player.phase1Error   = false;
        std::snprintf(g_player.phase1Msg, sizeof(g_player.phase1Msg),
                      "Fetching data...");
    }

    std::thread([accountId]() {
        // Fetch STRATZ display name first (fast, ~1s)
        std::string fetchedName = fetchStratzName(accountId, g_stratzToken);
        if (!fetchedName.empty()) {
            // Save to DB and update player state
            sqlite3* db = nullptr;
            if (sqlite3_open(DB_PATH, &db) == SQLITE_OK) {
                createPlayerInfoTable(db);
                savePlayerInfo(db, accountId, fetchedName.c_str());
                sqlite3_close(db);
            }
            std::lock_guard<std::mutex> lk(g_player.mtx);
            std::snprintf(g_player.name, sizeof(g_player.name),
                          "%s", fetchedName.c_str());
        }

        // Main data fetch (~30-90s)
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

// =============================================================================
//  Background orchestrator — mirrors main_unified.cpp loop
// =============================================================================
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
    std::thread([&]{ runGsiServer(g_gameInfo, g_steamKey); }).detach();

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

        // Read GameInfo
        GamePhase   phase;
        std::string matchId;
        int         ourSide, ourSlot;
        bool        newMatch, isHeroSel;
        {
            std::lock_guard<std::mutex> lk(g_gameInfo.mtx);
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
                "UPDATE livepicks SET our_slot=?, our_side=? WHERE 1;";
            sqlite3_stmt* ss = nullptr;
            if (sqlite3_prepare_v2(db, slotSql, -1, &ss, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(ss, 1, ourSlot);
                sqlite3_bind_int(ss, 2, ourSide);
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
            // Обновляем accountId в livepicks если матч уже идёт
            if (!matchId.empty()) {
                try {
                    long long mid = std::stoll(matchId);
                    initLivePicksRow(db, mid, accountId, ourSide, ourSlot);
                } catch (...) {}
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

        // ── Фаза 3: портреты + пикер запускаются на HERO_SELECTION ───────────
        // Останавливаются через PHASE3_TAIL_SEC после окончания HERO_SELECTION.
        // g_pickerState НЕ сбрасывается — GUI показывает последний результат
        // пока фаза STRATEGY / INGAME (до следующего newMatch).

        if (isHeroSel) {
            phase3EndPending = false;

            // Запустить пикер сразу при HERO_SELECTION
            if (!g_pickerRunning.load() && accountId != 0) {
                g_pickerRunning.store(true);
                g_pickerThread = std::thread([]{
                    runPickerGui(MODEL_PATH, DB_PATH,
                                 g_pickerRunning, &g_pickerState, &g_portraitState);
                });
            }
            // Запустить портреты
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

        } else {
            // HERO_SELECTION кончился — отсчитываем хвост
            if (g_pickerRunning.load() || g_portraitRunning.load()) {
                if (!phase3EndPending) {
                    phase3EndPending = true;
                    phase3EndTime    = Clock::now();
                }
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::now() - phase3EndTime).count();
                if (elapsed >= PHASE3_TAIL_SEC) {
                    // Остановить оба потока
                    g_pickerRunning.store(false);
                    if (g_pickerThread.joinable()) g_pickerThread.join();
                    g_portraitRunning.store(false);
                    if (g_portraitThread.joinable()) g_portraitThread.join();
                    g_portraitState.clear();
                    // g_pickerState НЕ сбрасываем — данные остаются для отображения
                    phase3EndPending = false;
                }
            }

            // Если игра закончилась / IDLE — сбросить состояние пикера
            if (phase == GamePhase::IDLE || phase == GamePhase::POSTGAME) {
                if (g_pickerState.gameStarted) {
                    g_pickerState.reset();
                }
            }
        }

    } // end while (g_orchestratorRunning)

    // Clean shutdown
    g_pickerRunning.store(false);
    g_portraitRunning.store(false);
    if (g_pickerThread.joinable())   g_pickerThread.join();
    if (g_portraitThread.joinable()) g_portraitThread.join();

    sqlite3_close(db);
}

// =============================================================================
//  Style
// =============================================================================
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
    // Масштабируем все отступы/рамки/скроллбары на 1.25× (вместе с шрифтом 20px)
    s.ScaleAllSizes(1.25f);
}

// =============================================================================
//  Draw helpers
// =============================================================================
static void DrawPortrait(ImDrawList* dl, ImVec2 p, float sz,
                         ImU32 fill, ImU32 border, const char* label) {
    dl->AddRectFilled(p, {p.x+sz, p.y+sz}, fill);
    dl->AddRect      (p, {p.x+sz, p.y+sz}, border, 0.f, 0, 1.f);
    if (label && *label) {
        ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText({p.x+(sz-ts.x)*0.5f, p.y+(sz-ts.y)*0.5f}, C(kText), label);
    }
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

// =============================================================================
//  DrawHeroSlot
//  showPos  — показывать позицию (только своя команда)
//  slotNum  — номер слота 1-5, используется если h.pos == 0
// =============================================================================
static void DrawHeroSlot(float rowW, const HeroSlotGui& h,
                         bool radiantSide, bool showPos, int slotNum) {
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
        DrawPortrait(dl, pp, PSZ, heroFill, Ca(kText, 0.18f), ab);
    }

    float tx = pp.x + PSZ + PAD;

    if (!h.filled && h.isYou) {
        dl->AddText({tx, rp.y + H*0.3f},        C(kText),       "Your Pick");
        dl->AddText({tx, rp.y + H*0.3f+lh+2.f}, Ca(kText,0.6f), "Pending...");
    } else if (!h.filled) {
        dl->AddText({tx, rp.y+(H-lh)*0.5f}, C(kMuted), "Unknown");
    } else {
        dl->AddText({tx, rp.y+(H-lh)*0.5f}, C(kText), h.name);
    }

    // Тег позиции — только для своей команды.
    // Используем h.pos если известна, иначе номер слота (1-5).
    if (showPos) {
        int dispPos = (h.pos > 0) ? h.pos : slotNum;
        if (dispPos > 0) {
            char posStr[8]; std::snprintf(posStr, sizeof(posStr), "Pos%d", dispPos);
            ImVec2 ts  = ImGui::CalcTextSize(posStr);
            float  tgW = ts.x + PAD*1.5f;
            float  tgH = lh + 4.f;
            float  tgX = rp.x + rowW - tgW - PAD;
            float  tgY = rp.y + (H - tgH) * 0.5f;
            dl->AddRectFilled({tgX,tgY},{tgX+tgW,tgY+tgH}, C(kCard2));
            dl->AddText({tgX+PAD*0.75f,tgY+2.f}, C(kMuted), posStr);
        }
    }

    ImGui::Dummy({rowW, H});
    ImGui::Spacing();
}

// =============================================================================
//  SectionLabel
// =============================================================================
static void SectionLabel(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(">");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6.f);
    ImGui::TextUnformatted(label);
}

// =============================================================================
//  DrawDraftPanel  — live data from g_pickerState, or "Waiting" overlay
// =============================================================================
static void DrawDraftPanel(float panelW) {
    const float PAD  = 8.f;
    const float colW = (panelW - PAD*3.f) * 0.5f;
    const float lh   = ImGui::GetTextLineHeight();

    // Snapshot picker state
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
        float       aW  = panelW - PAD*2.f;

        dl->AddRectFilled(cp, {cp.x+aW, cp.y+aH}, Ca(kCard2, 0.4f));
        dl->AddRect      (cp, {cp.x+aW, cp.y+aH}, C(kBorder));

        bool hasPlayer_ = false;
        { std::lock_guard<std::mutex> lk(g_player.mtx); hasPlayer_ = g_player.hasPlayer; }

        const char* line1 = hasPlayer_ ? "Waiting for a game..." : "Enter Steam ID";
        const char* line2 = hasPlayer_
            ? (phase == GamePhase::IDLE ? "GSI server: listening on :3000" : phaseName(phase))
            : "Enter your 32-bit Steam ID in the top-right corner";

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
        float  bW  = panelW - PAD*2.f;
        float  bH  = 52.f;
        dl2->AddRectFilled(bp,{bp.x+bW,bp.y+bH},Ca(kCard2,0.4f));
        dl2->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder));
        float cy = bp.y+(bH-lh)*0.5f;
        dl2->AddText({bp.x+10.f,cy},C(kMuted),">  Current draft win probability");
        dl2->AddText({bp.x+bW-50.f,cy},C(kMuted),"--.--%");
        ImGui::Dummy({bW,bH});
        return;
    }

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
        for (int i=0;i<5;i++) DrawHeroSlot(colW, rad[i], true,  isRadiant,  i+1);
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
        for (int i=0;i<5;i++) DrawHeroSlot(colW, dir[i], false, !isRadiant, i+1);
    }
    ImGui::EndGroup();

    // ── Win probability bar ────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bp  = ImGui::GetCursorScreenPos();
    float  bW  = panelW - PAD*2.f;
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

// =============================================================================
//  DrawPicksPanel  — live data from g_pickerState, or "Waiting" overlay
// =============================================================================
static void DrawPicksPanel(float panelW) {
    const float PAD       = 8.f;
    const float lh        = ImGui::GetTextLineHeight();
    const float WIN_COL_W = 88.f;
    const float BAR_W     = 68.f;
    const float rW_ref    = panelW - PAD*2.f;

    // Snapshot
    bool       gameStarted, ourHeroPicked;
    char       ourHeroName[64];
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
        recCount = g_pickerState.recCount;
        for (int i=0;i<recCount && i<10;i++) recs[i] = g_pickerState.recs[i];
    }

    SectionLabel(ourHeroPicked ? "YOUR PICK" : "RECOMMENDED PICKS");

    // Position badge
    if (ourPosition > 0) {
        char badgeText[32];
        std::snprintf(badgeText, sizeof(badgeText),
                      "[=] Position %d", ourPosition);
        ImVec2 ts = ImGui::CalcTextSize(badgeText);
        float  bW = ts.x + 14.f;
        float  bH = lh + 6.f;
        ImGui::SameLine(panelW - PAD*2.f - bW);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(bp,{bp.x+bW,bp.y+bH},C(kCard2));
        dl->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder));
        dl->AddText({bp.x+7.f,bp.y+3.f},C(kMuted),badgeText);
        ImGui::Dummy({bW,bH});
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
        const char* msg  = hasPlayer_ ? "Waiting for a game..." : "Enter Steam ID";
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

    // Column headers
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

    // Row lambda
    auto DrawPickRow = [&](int rank, const char* name, float win, bool highlight,
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
        const float portX  = rp.x + lh2 * 1.8f;   // после ранка
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
        DrawPortrait(dl, pp, PSZ, fill, bord, ab);

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
        // Show our hero + win prob
        DrawPickRow(0, ourHeroName, winProb, true, 0,0,0,0);
    } else {
        // Show top-10
        for (int i=0;i<recCount && i<10;i++) {
            const PickRowGui& r = recs[i];
            DrawPickRow(r.rank, r.name, r.winProb, r.rank==1,
                        r.gamesPlayer, r.wrPlayer, r.gamesImm, r.wrImm);
        }
        if (recCount == 0) {
            ImGui::TextColored(kMuted, "  Computing recommendations...");
        }
    }

    // Legend — динамическое выравнивание через CalcTextSize
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

// =============================================================================
//  DrawStatusBar  — phase indicators between header and content
// =============================================================================
static void DrawStatusBar(float fullW) {
    // Snapshot player state
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

    if (!hasPlayer) return;

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      sp  = ImGui::GetCursorScreenPos();
    const float H   = 22.f;
    const float lh  = ImGui::GetTextLineHeight();
    const float ty  = sp.y + (H-lh)*0.5f;

    dl->AddRectFilled(sp,{sp.x+fullW,sp.y+H},Ca(kCard2,0.5f));

    // Phase 1 dot + text
    ImVec4 dot1col = phase1Error ? kRed : (phase1Done ? kGreen : (phase1Running ? kAmber : kMuted));
    dl->AddCircleFilled({sp.x+10.f,sp.y+H*0.5f}, 4.f, C(dot1col));
    char p1buf[64];
    std::snprintf(p1buf, sizeof(p1buf), " Data: %s",
                  phase1Running ? "fetching..." : (phase1Done ? "ready" : (phase1Error ? "error" : "pending")));
    dl->AddText({sp.x+18.f, ty}, C(kMuted), p1buf);

    // Phase 2 dot + text (game state)
    ImVec4 dot2col = (phase == GamePhase::DRAFT   ? kAmber  :
                      phase == GamePhase::INGAME  ? kGreen  :
                      phase == GamePhase::POSTGAME? kMuted  : kMuted);
    float x2 = sp.x + 160.f;
    dl->AddCircleFilled({x2+4.f,sp.y+H*0.5f}, 4.f, C(dot2col));
    const char* phaseStr = (phase == GamePhase::IDLE)     ? "Waiting for game" :
                           (phase == GamePhase::DRAFT)    ? "Draft / Hero Select" :
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

    ImGui::Dummy({fullW, H});
}

// =============================================================================
//  DrawHeader  — logo [D] (clickable) + title + player card / ID input
// =============================================================================
static void DrawHeader(float fullW) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      hs  = ImGui::GetCursorScreenPos();
    const float H   = 60.f;
    const float lh  = ImGui::GetTextLineHeight();

    // ── [D] logo box — 44×44 в верхнем левом углу шапки ─────────────────
    const float LOGO_SZ = 44.f;
    ImVec2 logoPos = {hs.x, hs.y};

    dl->AddRectFilled(logoPos, {logoPos.x+LOGO_SZ, logoPos.y+LOGO_SZ}, C(kCard));
    dl->AddRect      (logoPos, {logoPos.x+LOGO_SZ, logoPos.y+LOGO_SZ}, C(kBorder));
    ImVec2 sts = ImGui::CalcTextSize("[D]");
    dl->AddText({logoPos.x+(LOGO_SZ-sts.x)/2.f,
                 logoPos.y+(LOGO_SZ-sts.y)/2.f}, C(kText), "[D]");

    // Invisible button over the [D] box
    ImGui::SetCursorScreenPos(logoPos);
    if (ImGui::InvisibleButton("##logo_btn",{LOGO_SZ, LOGO_SZ})) {
        if (g_Hwnd) {
            if (IsIconic(g_Hwnd)) ShowWindow(g_Hwnd, SW_RESTORE);
            SetForegroundWindow(g_Hwnd);
            BringWindowToTop(g_Hwnd);
        }
    }

    // ── Title ─────────────────────────────────────────────────────────────
    float tx = hs.x+54.f;
    dl->AddText({tx, hs.y+4.f},       C(kText),  "Dota_Drafter");
    dl->AddText({tx, hs.y+4.f+lh+2},  C(kMuted), "Dota 2 Draft Analyzer - Live Overlay");

    // ── Player card (right side) ──────────────────────────────────────────
    const float CW = 240.f;
    float cx = hs.x + fullW - CW;

    dl->AddRectFilled({cx,hs.y},    {cx+CW,hs.y+H}, C(kCard));
    dl->AddRect      ({cx,hs.y},    {cx+CW,hs.y+H}, C(kBorder));

    // Avatar box
    dl->AddRectFilled({cx+8,hs.y+8},{cx+44,hs.y+44},C(kCard2));
    dl->AddRect      ({cx+8,hs.y+8},{cx+44,hs.y+44},C(kBorder));

    // Snapshot player info
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
        // Prompt text in avatar box
        ImVec2 avts = ImGui::CalcTextSize("ID");
        dl->AddText({cx+8+(36-avts.x)/2.f, hs.y+8+(36-avts.y)/2.f},
                    C(kMuted), "ID");

        dl->AddText({cx+52.f, hs.y+8.f}, C(kMuted), "Enter Steam ID (32-bit):");

        // InputText + Set button
        ImGui::SetCursorScreenPos({cx+52.f, hs.y+8.f+lh+4.f});
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
        // Initials in avatar box
        char av[3] = {pname[0] ? pname[0] : '?',
                      pname[1] ? pname[1] : '\0', '\0'};
        ImVec2 avts = ImGui::CalcTextSize(av);
        dl->AddText({cx+8+(36-avts.x)/2.f, hs.y+8+(36-avts.y)/2.f},
                    C(kMuted), av);

        // Player name (clickable to edit)
        dl->AddText({cx+52.f, hs.y+8.f},  C(kText), pname);
        char idStr[32];
        std::snprintf(idStr, sizeof(idStr), "ID %lld", accountId);
        dl->AddText({cx+52.f, hs.y+8.f+lh+4.f}, C(kMuted), idStr);

        // Invisible button over name area to trigger edit
        ImGui::SetCursorScreenPos({cx+52.f, hs.y+8.f});
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

// =============================================================================
//  RenderFrame
// =============================================================================
static void RenderFrame() {
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

    float headerH = 0.f;
    {
        ImVec2 before = ImGui::GetCursorScreenPos();
        DrawHeader(FULL);
        ImVec2 after  = ImGui::GetCursorScreenPos();
        headerH = after.y - before.y;
    }

    ImGui::Spacing();
    DrawStatusBar(FULL);
    ImGui::Spacing();

    float usedH    = ImGui::GetCursorPosY();
    float contentH = H - usedH - PAD;
    float leftW    = FULL * 0.575f;
    float rightW   = FULL - leftW - 8.f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##draft",{leftW,contentH},true,ImGuiWindowFlags_NoScrollbar);
    ImGui::Spacing();
    DrawDraftPanel(leftW-20.f);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 8.f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##picks",{rightW,contentH},true);
    ImGui::Spacing();
    DrawPicksPanel(rightW-12.f);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ── No-player overlay (full screen) ───────────────────────────────────
    bool showEnterID = false;
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        showEnterID = !g_player.hasPlayer && !s_editMode;
    }
    // The header already handles the input; no separate overlay needed.
    // Just show a hint in the content area when panels are empty.
    (void)showEnterID;

    ImGui::End();
}

// =============================================================================
//  Application icon — рисуем [D] программно через GDI
// =============================================================================
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
    int fs = (int)(sz * 0.55f);
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

// =============================================================================
//  WinMain
// =============================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // DPI awareness
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI* SPDA_t)(void*);
        if (auto fn = (SPDA_t)GetProcAddress(u32,"SetProcessDpiAwarenessContext"))
            fn((void*)(intptr_t)-4);
    }

    ensureLogsDir();
    CurlGlobal curlInit;

    // ── Config from environment ───────────────────────────────────────────
    g_steamKey   = "F54FDF3461131271522AA4679FB980D7";
    if (const char* e = std::getenv("STEAM_API_KEY"))  g_steamKey   = e;
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

    // ── Start orchestrator thread ─────────────────────────────────────────
    g_orchestratorThread = std::thread(orchestratorMain);

    // If we already have a player, kick off Phase 1 (OUTSIDE mutex to avoid deadlock)
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

    HWND hwnd = CreateWindowW(L"Dota_Drafter",
        L"Dota_Drafter - Dota 2 Draft Analyzer",
        WS_OVERLAPPEDWINDOW, winX, winY, winW, winH,
        nullptr, nullptr, hInst, nullptr);
    g_Hwnd = hwnd;

    // DWM theming
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

    // ── Icon ──────────────────────────────────────────────────────────────
    g_AppIcon = CreateDIcon(32);
    if (g_AppIcon) {
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_AppIcon);
        SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)g_AppIcon);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        // Шрифт: Segoe UI 20px (Windows), fallback — Arial 20px
        float fontSize = 20.f;
        if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", fontSize))
            io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", fontSize);
        // Если ни один не найден — ImGui использует дефолтный шрифт
    }
    ApplyStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_Device, g_Context);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg{};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            continue;
        }
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

    // ── Shutdown ──────────────────────────────────────────────────────────
    g_orchestratorRunning.store(false);
    if (g_orchestratorThread.joinable()) g_orchestratorThread.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(L"Dota_Drafter", hInst);
    if (g_AppIcon) DestroyIcon(g_AppIcon);
    return 0;
}