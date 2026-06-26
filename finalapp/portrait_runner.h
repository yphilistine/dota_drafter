#pragma once
/*
 * portrait_runner.h — захват портретов + позиций + overlay-кнопка [D].
 */

#include "shared_types.h"
#include <string>
#include <atomic>

// Запуск прозрачной кнопки [D] поверх Dota 2.
// Вызывать один раз при старте (из orchestratorMain). Видна пока открыто окно Dota 2.
void startDotaOverlay();

// Цикл захвата портретов + позиций каждые 500мс → распознавание → запись в livepicks.
// Работает без accountId. Блокирует поток до running == false.
void runPortraitCapture(GameInfo&           gameInfo,
                        const std::string&  dbPath,
                        std::atomic<bool>&  running,
                        SharedPortraitState& out);