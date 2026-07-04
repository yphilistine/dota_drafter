#pragma once
/*
 * portrait_runner.h — захват портретов + позиций.
 */

#include "shared_types.h"
#include "overlay_button.h"
#include <string>
#include <atomic>

// Цикл захвата портретов + позиций каждые 500мс → распознавание → запись в livepicks.
// Работает без accountId. Блокирует поток до running == false.
void runPortraitCapture(GameInfo&           gameInfo,
                        const std::string&  dbPath,
                        std::atomic<bool>&  running,
                        SharedPortraitState& out);