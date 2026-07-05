#pragma once
/*
 * gui_draw.h — панели ImGui (Header/StatusBar/Draft/Picks/Meta Heroes),
 * кэш портретов героев, кэш меты, кэш отображаемых имён.
 *
 * Палитра и цветовые хелперы (C()/Ca()) объявлены здесь же, а не в
 * app_state.h: это не runtime-состояние, а константы стиля, нужные и всем
 * Draw*-функциям этого файла, и mainGUI.cpp (ApplyStyle, RenderFrame).
 */

#include "app_state.h"
#include "imgui.h"

#include <string>
#include <cstdint>

// ─── Палитра ─────────────────────────────────────────────────────────────────
inline const ImVec4 kBg     = {0.04f, 0.04f, 0.04f, 1.f};
inline const ImVec4 kCard   = {0.09f, 0.09f, 0.09f, 1.f};
inline const ImVec4 kCard2  = {0.15f, 0.15f, 0.15f, 1.f};
inline const ImVec4 kText   = {0.98f, 0.98f, 0.98f, 1.f};
inline const ImVec4 kMuted  = {0.63f, 0.63f, 0.63f, 1.f};
inline const ImVec4 kBorder = {1.f,   1.f,   1.f,   0.10f};
inline const ImVec4 kGreen  = {0.39f, 0.78f, 0.47f, 1.f};
inline const ImVec4 kRed    = {0.88f, 0.45f, 0.35f, 1.f};
inline const ImVec4 kAmber  = {0.88f, 0.73f, 0.35f, 1.f};

inline ImU32 C(ImVec4 v)           { return ImGui::ColorConvertFloat4ToU32(v); }
inline ImU32 Ca(ImVec4 v, float a) { v.w = a; return ImGui::ColorConvertFloat4ToU32(v); }

// ─── Кэш портретов героев (assets/*.png → D3D11 текстуры) ─────────────────────
void loadHeroPortraits();
void unloadHeroPortraits();

// ─── "Powered by" — иконки STRATZ/OpenDota/D2PT (тянутся по HTTP, см. gui_draw.cpp) ─
void unloadPoweredByIcons();

// Используется и здесь (портреты героев), и в mainGUI.cpp (RenderFrame —
// текстура аватара из буферизованных байт).
ID3D11ShaderResourceView* createTextureFromImageData(const uint8_t* data, size_t size);

// heroId → localized_name для отображения (не для сопоставления портретов —
// см. portrait_runner.cpp).
std::string heroDisplayName(int heroId, const char* fallback);

// ─── Панели ──────────────────────────────────────────────────────────────────
float DrawHeader(float fullW);
void  DrawStatusBar(float fullW);
void  DrawDraftPanel(float panelW);
void  DrawPicksPanel(float panelW);
void  DrawMetaHeroesPanel(float panelW);
