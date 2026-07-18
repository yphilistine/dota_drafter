#include "app_state.h"

PlayerState g_player;

GameInfo            g_gameInfo;
SharedPortraitState g_portraitState;
GuiPickerState      g_pickerState;

std::string g_stratzToken;

ID3D11ShaderResourceView* g_avatarSRV = nullptr;
ID3D11Device*             g_Device    = nullptr;
HWND                      g_Hwnd      = nullptr;

AppNotice g_appNotice;

// Manual-reset, изначально сигнальный — первый кадр рисуется сразу при
// старте, не дожидаясь первого фонового обновления состояния.
static HANDLE g_redrawEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);

void requestRedraw() {
    if (g_redrawEvent) SetEvent(g_redrawEvent);
}

HANDLE redrawEventHandle() { return g_redrawEvent; }
