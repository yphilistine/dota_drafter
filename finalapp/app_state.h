#pragma once
/*
 * app_state.h — состояние, реально расшаренное между mainGUI.cpp,
 * orchestrator.cpp и gui_draw.cpp. Глобал живёт здесь только если он
 * читается/пишется из ≥2 из этих файлов (проверено по каждому use-site
 * при разбивке mainGUI.cpp) — иначе он инкапсулирован за функциями
 * своего файла (см. orchestrator.h/gui_draw.h).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include "shared_types.h"

#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstdio>

// ─── Данные игрока: пишутся фоновыми потоками (orchestrator.cpp), читаются GUI ─
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
extern PlayerState g_player;

// ─── Общее состояние потоков (GSI / portrait / picker) ────────────────────────
extern GameInfo            g_gameInfo;
extern SharedPortraitState g_portraitState;
extern GuiPickerState      g_pickerState;

// ─── Конфигурация (из переменных окружения / умолчаний, затем read-only) ──────
extern std::string g_stratzToken;

inline constexpr const char* DB_PATH    = "playerandlivestats.db";
inline constexpr const char* MODEL_PATH = "draft_helper_abstract";

// Портреты + пикер: активны HERO_SELECTION + 5с. GUI показывает результат до конца игры.
inline constexpr int PHASE3_TAIL_SEC = 5;

// Текстура аватара — владелец D3D11-устройства (mainGUI.cpp), рендерится в
// gui_draw.cpp (DrawHeader), пересоздаётся в mainGUI.cpp (RenderFrame),
// освобождается перед рефетчем в orchestrator.cpp (startPhase1).
extern ID3D11ShaderResourceView* g_avatarSRV;

// D3D11-устройство — владелец mainGUI.cpp (InitD3D/CleanupD3D), но нужно и
// gui_draw.cpp (createTextureFromImageData создаёт текстуры портретов/аватара).
extern ID3D11Device* g_Device;

// Хендл главного окна — владелец mainGUI.cpp (CreateMainWindow), но нужен и
// gui_draw.cpp (DrawHeader: клик по логотипу [D] возвращает фокус на окно).
extern HWND g_Hwnd;

// ─── Единый канал уведомлений для GUI ──────────────────────────────────────────
// Заменяет прежний файл-статик g_schemaBannerNeeded (был только для одного
// частного случая — SCHEMA_TOO_NEW). GuiPickerState::schemaError остаётся
// отдельно и независимо — это уже рабочий, самодостаточный механизм для
// другого частного случая (несовместимость схемы данных пикера).
enum class NoticeLevel { Info, Warn, Error };
struct AppNotice {
    std::mutex  mtx;
    bool        active = false;
    NoticeLevel level  = NoticeLevel::Warn;
    char        text[256] = {};
    void set(NoticeLevel lvl, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mtx);
        active = true;
        level  = lvl;
        std::snprintf(text, sizeof(text), "%s", msg.c_str());
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mtx);
        active = false;
    }
};
extern AppNotice g_appNotice;
