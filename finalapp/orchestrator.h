#pragma once
/*
 * orchestrator.h — фоновый оркестратор: управление GSI/portrait/picker
 * потоками, livepicks/player_info, запуск фазы 1b. Тред-менеджмент
 * (g_pickerRunning/g_portraitRunning/... ) инкапсулирован внутри
 * orchestrator.cpp — наружу только функции ниже.
 */

#include <sqlite3.h>

// Запускает фоновый поток orchestratorMain (GSI-сервер, overlay [D],
// управление жизненным циклом portrait/picker потоков). Вызывать один раз
// из WinMain.
void startOrchestrator();

// Останавливает и join'ит поток оркестратора (и, транзитивно, picker/portrait
// потоки, которые он поднял). Вызывать при завершении приложения.
void stopOrchestrator();

// Фаза 1b: запускает загрузку данных игрока (OpenDota имя/аватар + runDataFetcher)
// в фоновом потоке. Вызывается из WinMain (сохранённый игрок) и из gui_draw.cpp
// (ввод/смена Friend ID, кнопка Refresh).
void startPhase1(long long accountId);

// Сигнал оркестратору: пользователь вручную сменил позицию в GUI — записать
// manualPos в livepicks и запустить one-shot переинференс. Единственная точка
// входа для gui_draw.cpp (SetOurPosition): флаг-атомик оркестратора приватен
// (static внутри orchestrator.cpp).
void requestPositionRefresh();

// --- SQLite: таблица player_info (используется также из WinMain при старте) ---
void createPlayerInfoTable(sqlite3* db);
long long loadSavedPlayer(sqlite3* db, char nameOut[128]);
