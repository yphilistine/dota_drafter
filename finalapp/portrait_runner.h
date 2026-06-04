#pragma once
/*
 * portrait_runner.h
 */

#include "shared_types.h"
#include <string>
#include <atomic>

// Запускает прозрачную кнопку [D] поверх Dota 2.
// Вызывать один раз при старте приложения (из orchestratorMain).
// Кнопка видна всегда, пока окно Dota 2 открыто.
void startDotaOverlay();

// Запускает цикл захвата портретов. Блокирует поток пока running == true.
void runPortraitCapture(GameInfo&           gameInfo,
                        const std::string&  dbPath,
                        std::atomic<bool>&  running,
                        SharedPortraitState& out);