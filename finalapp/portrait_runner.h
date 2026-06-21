#pragma once
/*
 * portrait_runner.h — захват портретов + overlay-кнопка [D].
 */

#include "shared_types.h"
#include <string>
#include <atomic>

// Запуск прозрачной кнопки [D] поверх Dota 2.
// Вызывать один раз при старте (из orchestratorMain). Видна пока открыто окно Dota 2.
void startDotaOverlay();

// Цикл захвата портретов каждые 500мс → распознавание → запись в livepicks.
// Блокирует поток до running == false.
void runPortraitCapture(GameInfo&           gameInfo,
                        const std::string&  dbPath,
                        std::atomic<bool>&  running,
                        SharedPortraitState& out);