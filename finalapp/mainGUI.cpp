/*
 * mainGUI.cpp — ImGui D3D11 GUI: WinMain, D3D11, WndProc, RenderFrame.
 *
 * Остальное живёт в отдельных файлах по зонам ответственности:
 *   app_state.{h,cpp}     — общее состояние (GameInfo/PortraitState/PickerState/
 *                           PlayerState/AppNotice), расшаренное между этим файлом,
 *                           orchestrator.cpp и gui_draw.cpp
 *   update_window.{h,cpp} — нативное Win32-окно апдейтера (до D3D11/ImGui)
 *   orchestrator.{h,cpp}  — фоновый оркестратор (GSI/portrait/picker потоки, livepicks)
 *   gui_draw.{h,cpp}      — панели ImGui (Draft/Picks/Meta Heroes/Header/StatusBar)
 *
 * Потоковая модель:
 *   GUI-поток      — WinMain / message loop / ImGui render
 *   Оркестратор    — управление потоками, portrait→GUI sync, one-shot inference
 *   GSI-поток      — runGsiServer (постоянно)
 *   Фаза 1a        — runDataFetcherInit (при старте, без accountId)
 *   Фаза 1b        — runDataFetcher (при вводе Friend ID)
 *   Portrait       — runPortraitCapture (HERO_SELECTION + 5с хвост, без accountId)
 *   Picker         — runPickerGui (HERO_SELECTION/DRAFT + 5с хвост, требует accountId)
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
#include "version.h"
#include "updater.h"
#include "app_state.h"
#include "gui_draw.h"
#include "orchestrator.h"
#include "update_window.h"

#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

// --- D3D11 инициализация ------------------------------------------------------
// g_Device и g_Hwnd — в app_state.h (нужны и gui_draw.cpp).
static ID3D11DeviceContext*    g_Context   = nullptr;
static IDXGISwapChain*         g_SwapChain = nullptr;
static ID3D11RenderTargetView* g_RTV       = nullptr;

static void RenderFrame();   // определена ниже; нужна раньше для WM_SIZE
static void PresentFrame();  // определена ниже; форс-рендер во время live-resize

// Минимальный размер клиентской области: раскладка (шапка с карточкой игрока
// фиксированной ширины + 3 колонки драфта/рекомендаций/меты) считается в
// абсолютных пикселях и не масштабируется — при более узком/низком окне
// элементы наезжают друг на друга и на заголовок окна.
static const LONG kMinClientW = 1300;
static const LONG kMinClientH = 480;

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
    case WM_GETMINMAXINFO: {
        RECT r = {0, 0, kMinClientW, kMinClientH};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
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

// --- Стиль ImGui -------------------------------------------------------------
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

    float headerH = DrawHeader(FULL);

    // Единый отступ между секциями
    const float GAP = PAD;

    ImGui::SetCursorPos({PAD, PAD + headerH + GAP});
    DrawStatusBar(FULL);
    ImGui::SetCursorPos({PAD, PAD + headerH + GAP + 26.f + GAP});

    // -- Баннеры совместимости / общие уведомления -----------------------
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

        // g_appNotice — общий канал уведомлений уровня приложения. schemaError
        // выше — отдельный, самодостаточный механизм пикера; оба баннера могут
        // показываться одновременно, не мешая друг другу.
        bool noticeActive = false;
        char noticeMsg[256] = {};
        {
            std::lock_guard<std::mutex> lk(g_appNotice.mtx);
            noticeActive = g_appNotice.active;
            if (noticeActive) std::memcpy(noticeMsg, g_appNotice.text, sizeof(noticeMsg));
        }
        if (noticeActive) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.8f,0.f,1.f));
            ImGui::TextWrapped("%s", noticeMsg);
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

// Sync interval для Present(): рендер на каждый 2-й vblank. Present()
// блокируется до нужного vblank'а, поэтому частота полной пересборки ImGui
// draw-list (RenderFrame/Render) в RunMessageLoop масштабируется вместе с Гц
// монитора, а не привязана к фиксированному FPS.
static constexpr UINT kPresentSyncInterval = 2;

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
    g_SwapChain->Present(kPresentSyncInterval, 0);
}

// --- Иконка приложения [D] (программно через GDI) ----------------------------
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

// --- Точка входа: подфункции WinMain ------------------------------------------

static void PlatformStartupFixups() {
    // Рабочая директория = папка с моделью (exe dir или parent, если exe в build/)
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

    // DPI-осведомлённость
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL(WINAPI* SPDA_t)(void*);
        if (auto fn = (SPDA_t)GetProcAddress(u32,"SetProcessDpiAwarenessContext"))
            fn((void*)(intptr_t)-4);
    }

    initConsole();
    // Сетка безопасности на уровне процесса: без неё необработанное исключение
    // вне точечных try/catch (или SEH-исключение из GDI+/D3D11/WinRT) тихо
    // валит процесс без единой строки в логах.
    installCrashHandlers();
}

// Блокирующая проверка обновлений (до создания D3D11-окна) — окно апдейтера,
// ретрай-цикл при отсутствии сети, скачивание/установка app- или data-обновлений.
// Возвращает false, если приложение должно завершиться сейчас (пользователь
// отказался от ретрая, либо запущен релонч инсталлятора).
static bool RunStartupUpdateCheck(HINSTANCE hInst) {
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
            return false;
        }
    }

    UpdateAction action = checkForUpdates(manifest);

    // GSI-конфиг живёт вне {app} (в папке Dota 2) и не завязан на схему
    // данных/версию приложения, поэтому синхронизируется отдельно от
    // action — и на APP_UPDATE/DATA_UPDATE, и на NONE (обычный запуск).
    syncGsiConfig(manifest);

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

            // Написать .bat который ждёт выхода процесса, потом запускает инсталлятор.
            // kPingCount пингов ping -n дают ~(kPingCount-1) секунд задержки — время,
            // за которое наш процесс должен полностью завершиться и отпустить файл exe,
            // прежде чем инсталлятор начнёт его перезаписывать.
            const int kPingCount = 4;

            char fullStagingPath[MAX_PATH];
            GetFullPathNameA(stagingPath.c_str(), MAX_PATH, fullStagingPath, nullptr);

            std::string batPath = std::string(fullStagingPath) + ".bat";
            {
                std::ofstream bat(batPath, std::ios::trunc);
                bat << "@echo off\n";
                bat << "ping -n " << kPingCount << " 127.0.0.1 >nul\n";
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
            } else {
                LOG_ERR("Failed to launch installer relaunch script: " << batPath);
            }

            // Не закрывать окно сразу — держать его открытым (с накачкой сообщений,
            // чтобы не словить "Not Responding") примерно на то же время, что и .bat
            // выше ждёт перед запуском инсталлятора, плюс небольшой запас на его
            // собственный старт — иначе между закрытием апдейтера и появлением окна
            // инсталлятора пользователь видел бы несколько секунд пустого экрана.
            SetUpdateStatus(L"Starting installer...");
            DWORD waitMs    = (DWORD)(kPingCount - 1) * 1000 + 500;
            DWORD waitStart = GetTickCount();
            while (GetTickCount() - waitStart < waitMs) {
                PumpMessages();
                Sleep(15);
            }

            DestroyUpdateWindow();
            return false;
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
        } else {
            LOG_WARN("Data update download failed, using existing data");
        }
    }

    if (action == UpdateAction::SCHEMA_TOO_NEW) {
        g_appNotice.set(NoticeLevel::Warn,
            "A newer data version is available but requires an app update.");
    }

    DestroyUpdateWindow();
    return true;
}

// Конфиг из env, загрузка сохранённого игрока из БД, запуск фазы 1a, оркестратора
// и (если есть сохранённый Friend ID) фазы 1b.
static void InitPlayerStateAndBackgroundThreads() {
    if (const char* e = std::getenv("STRATZ_API_KEY")) g_stratzToken = e;
    else g_stratzToken = DEFAULT_STRATZ_TOKEN;

    try {
        SqliteDB db(DB_PATH);
        createPlayerInfoTable(db.get());
        char savedName[128] = {};
        long long savedId   = loadSavedPlayer(db.get(), savedName);
        if (savedId > 0) {
            std::lock_guard<std::mutex> lk(g_player.mtx);
            g_player.accountId = savedId;
            g_player.hasPlayer = true;
            std::snprintf(g_player.name, sizeof(g_player.name), "%s",
                          savedName[0] ? savedName : "Unknown");
        }
    } catch (const std::exception& e) {
        LOG_WARN("Could not load saved player: " << e.what());
    }

    // Фаза 1a: таблицы + справочник героев + мета-стата (фоновый поток)
    std::thread([]{ runDataFetcherInit(g_stratzToken); }).detach();

    startOrchestrator();

    long long savedId  = 0;
    bool      hasPlayer = false;
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        savedId   = g_player.accountId;
        hasPlayer = g_player.hasPlayer;
    }
    if (hasPlayer) startPhase1(savedId);
}

static HWND CreateMainWindow(HINSTANCE hInst) {
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0, 0,
                      hInst, nullptr, nullptr, nullptr, nullptr,
                      L"Dota_Drafter", nullptr};
    RegisterClassExW(&wc);

    // -- Начальный размер окна: 1.5× базового (1200×900), но не больше экрана -
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

    // -- Icon (16×16 для заголовка, 32×32 для панели задач) --------------
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

    return hwnd;
}

// D3D11 + ImGui + шрифты + стиль + кэш портретов. false при провале InitD3D
// (единственная существующая явная fatal-ветка WinMain).
static bool InitGuiAndAssets(HWND hwnd) {
    if (!InitD3D(hwnd)) return false;

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
    return true;
}

// Safety-net для события redrawEventHandle(): курсор InputText, hover-тултипы
// и подобная ImGui-анимация не сигналят requestRedraw() сами по себе, поэтому
// луп всё равно просыпается сам не реже этого интервала, даже без сообщений
// и без сигналов от фоновых потоков.
static constexpr DWORD kRedrawTimeoutMs = 300;

static void RunMessageLoop() {
    MSG msg{};
    HANDLE redrawEvt = redrawEventHandle();
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg);
            continue;
        }
        // Окно свёрнуто — рендерить нечего (DWM не показывает бэкбуфер).
        // WaitMessage() блокирует поток до следующего сообщения (например
        // WM_SYSCOMMAND restore).
        if (IsIconic(g_Hwnd)) {
            WaitMessage();
            continue;
        }
        // PresentFrame() — main-поток WinMain, необработанное исключение
        // из отрисовки убило бы весь процесс, поэтому ловим здесь.
        try {
            PresentFrame();
        } catch (const std::exception& e) {
            LOG_ERR("[gui] PresentFrame exception: " << e.what());
        } catch (...) {
            LOG_ERR("[gui] PresentFrame unknown exception");
        }

        // Ожидание идёт после Present: кадр должен отражать сообщения,
        // обработанные в этой итерации (например, символ, введённый в
        // InputText), а не оставлять их до следующего пробуждения.
        // Просыпаемся на: новое оконное сообщение (QS_ALLINPUT), сигнал
        // requestRedraw() от фонового потока, либо таймаут.
        MsgWaitForMultipleObjects(1, &redrawEvt, FALSE, kRedrawTimeoutMs, QS_ALLINPUT);
        ResetEvent(redrawEvt);
    }
}

static void ShutdownApp(HINSTANCE hInst, HWND hwnd, ULONG_PTR gdipToken) {
    stopOrchestrator();

    if (g_avatarSRV) { g_avatarSRV->Release(); g_avatarSRV = nullptr; }
    unloadHeroPortraits();
    unloadPoweredByIcons();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(L"Dota_Drafter", hInst);
    if (g_AppIcon) DestroyIcon(g_AppIcon);
    Gdiplus::GdiplusShutdown(gdipToken);
}

// --- Точка входа -------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    PlatformStartupFixups();

    CurlGlobal curlInit;

    // GDI+ для декодирования аватаров/портретов (JPEG/PNG)
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

    if (!RunStartupUpdateCheck(hInst)) {
        Gdiplus::GdiplusShutdown(gdipToken);
        return 0;
    }

    InitPlayerStateAndBackgroundThreads();

    HWND hwnd = CreateMainWindow(hInst);
    if (!InitGuiAndAssets(hwnd)) { DestroyWindow(hwnd); return 1; }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    RunMessageLoop();

    ShutdownApp(hInst, hwnd, gdipToken);
    return 0;
}
