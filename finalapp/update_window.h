#pragma once
/*
 * update_window.h - нативное Win32-окно "Checking for updates..." (до
 * инициализации D3D11/ImGui). Самодостаточный блок, не зависит от
 * остального состояния приложения (app_state.h/shared_types.h).
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

void CreateUpdateWindow(HINSTANCE hInst);
void DestroyUpdateWindow();
void SetUpdateStatus(const wchar_t* text);
void SetUpdateProgress(const wchar_t* label, int percent);
void ShowRetryButton(bool show);
bool WaitForRetryClick();

// true, если пользователь закрыл окно апдейтера крестиком. Стартовая проверка
// обновлений (WinMain) обязана прерваться и завершить приложение: GUI ещё не
// создан, и без окна процесс жил бы дальше невидимым.
bool UpdateWindowAborted();

// Прокачка очереди сообщений без блокировки - используется между шагами
// обновления (см. WinMain) и внутри SetUpdateStatus/SetUpdateProgress, чтобы
// окно оставалось отзывчивым во время скачивания/установки.
void PumpMessages();
