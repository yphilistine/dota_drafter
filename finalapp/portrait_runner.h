#pragma once
/*
 * portrait_runner.h - захват портретов + позиций.
 */

#include "shared_types.h"
#include "overlay_button.h"
#include <string>
#include <atomic>

// Цикл захвата портретов + позиций каждые 250мс → распознавание → запись в livepicks.
// Работает без accountId. Блокирует поток до running == false.
void runPortraitCapture(GameInfo&           gameInfo,
                        const std::string&  dbPath,
                        std::atomic<bool>&  running,
                        SharedPortraitState& out);

// Одноразовый диагностический снимок для кнопки [screenshot] в GUI: захват
// текущего кадра HUD, сохранение 10 регионов героев + 10 регионов позиций как
// PNG (radiant/dire_hero_0..4, radiant/dire_pos_0..4) + fullscreen_regions.png
// (полный кадр HUD с наложенными рамками всех 20 регионов) + report.txt с
// именем и score распознанного героя на каждый слот и с сырым OCR-текстом/
// позицией/score на каждый позиционный слот. dbPath - playerandlivestats.db
// (справочник heroes для имени). Возвращает false, если окно Dota 2 не
// найдено или кадр не захвачен (dir тогда не создаётся).
bool captureDebugScreenshotWithReport(const std::string& dbPath,
                                      const std::string& dir = "screenshots");