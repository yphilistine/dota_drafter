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
