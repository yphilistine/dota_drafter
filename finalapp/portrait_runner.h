#pragma once
/*
 * portrait_runner.h
 *
 * Функция захвата портретов героев без сохранения PNG.
 * Каждые 500 мс: PrintWindow → crop → hash → recognize.
 * Если score >= 0.5: записывает hero_id в livepicks и обновляет SharedPortraitState.
 */

#include "shared_types.h"
#include <string>
#include <atomic>

// Запускает цикл захвата. Блокирует поток пока running == true.
void runPortraitCapture(GameInfo&           gameInfo,
                        const std::string&  dbPath,
                        std::atomic<bool>&  running,
                        SharedPortraitState& out);
