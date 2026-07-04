# Dota Drafter — Контекст проекта

## Архитектура

Приложение — Dota 2 Draft Assistant с ImGui/D3D11 GUI.
Потоковая модель: GUI-поток, оркестратор, GSI-сервер, DataFetcher, Portrait capture, Picker.

Фазы запуска:
1. **DataFetcherInit** (фаза 1a) — создание таблиц + справочник героев (без accountId)
2. **DataFetcher** (фаза 1b) — загрузка данных игрока (OpenDota, STRATZ → SQLite), требует accountId
3. **GSI-сервер** — приём состояния игры от Dota 2 через Game State Integration
4. **Portrait capture** — захват портретов HUD + распознавание героев и позиций (без accountId)
5. **Picker** — ML-рекомендации CatBoost (требует accountId для mastery)

Ключевой принцип: portrait capture и отображение драфта работают **без accountId**.
Picker (модель + рекомендации) запускается только при наличии accountId.
GSI-таймаут: если нет обновлений > 15с — сброс на IDLE (Dota 2 закрыта).

**mainGUI.cpp разбит на 4 файла по зонам ответственности** (было 2720 строк в одном файле):
`app_state.h/.cpp` (общее состояние), `update_window.h/.cpp` (окно апдейтера),
`orchestrator.h/.cpp` (фоновый оркестратор), `gui_draw.h/.cpp` (панели ImGui).
`mainGUI.cpp` теперь содержит только `WinMain` (разбит на 7 подфункций), D3D11, `WndProc`, `RenderFrame`.
Подробности — в соответствующих секциях ниже.

---

## Файлы и функции

### shared_types.h
Общие типы данных для межпоточного взаимодействия.

| Тип | Описание |
|-----|----------|
| `GamePhase` | Enum: IDLE, DRAFT, INGAME, POSTGAME |
| `phaseName(GamePhase)` | Возвращает строковое имя фазы |
| `GameInfo` | Состояние матча от GSI (mutex-protected, lastUpdate для таймаута) |
| `PortraitResult` | Результат распознавания одного портрета (имя, id, score) |
| `SharedPortraitState` | 10 слотов портретов + manualPos[10] для GUI-позиций + флаг active |
| `HeroSlotGui` | Данные одного слота для отрисовки в GUI |
| `PickRowGui` | Строка рекомендации: герой, winProb, статистика |
| `GuiPickerState` | Полное состояние пикера для GUI + inferenceGen (атомарный счётчик) |

Декларации: `runDataFetcherInit`, `runDataFetcher`, `runGsiServer`, `runPortraitCapture`, `runPickerGui`.

---

### common.h / common.cpp
Общие утилиты: логирование, HTTP, RAII-обёртки.

| Функция / Класс | Описание |
|------------------|----------|
| `CurlGlobal` | RAII: curl_global_init / cleanup |
| `CurlHeaders` | RAII: curl_slist |
| `CurlHandle` | RAII: curl_easy_init / cleanup |
| `SqliteDB` | RAII: sqlite3_open / close + PRAGMA. `SqliteDB(path, readOnly=false)` — при `readOnly=true` открывает `SQLITE_OPEN_READONLY` и пропускает write-PRAGMA. Всегда выставляет `sqlite3_busy_timeout(db, 5000)` — единая точка защиты от `SQLITE_BUSY` под конкурентными писателями (раньше нигде не выставлялся) |
| `SqliteStmt` | RAII: sqlite3_prepare_v2/finalize + типизированные bind/column-хелперы (`row()`, `bind_int()`, `col_int()`, `col_int64()`, `col_text()`, `col_null()`, `col_double()`). Промоутирован из локального класса `Stmt` в dota_picker.cpp — раньше это был второй, независимый SQLite-wrapper в проекте |
| `SqliteTransaction` | RAII: BEGIN / COMMIT / ROLLBACK |
| `initConsole()` | Инициализация логов, ANSI, curl debug файла |
| `logConsole(level, msg)` | Логирование с таймстампом и цветами |
| `ensureLogsDir()` | Создание папки logs/ |
| `httpGet(url)` | HTTP GET с ретраями (3 попытки, 10 сек пауза) |
| `httpPost(url, body, token)` | HTTP POST с ретраями и авторизацией |
| `sanitizeUtf8(input)` | Очистка невалидных UTF-8 последовательностей |
| `safeStoi(s, fallback=0)` / `safeStoll(s, fallback=-1)` | Разбор int/long long без исключений (try/catch + fallback) — для непроверенных внешних данных (GSI-payload от Dota 2, STRATZ-ответы). Используется в `livestatsfetcher.cpp` (team_slot), `playerdatafetcher.cpp` (match id), `orchestrator.cpp` (matchId) вместо голого `std::stoi`/`std::stoll`, чьё исключение в отсоединённом потоке раньше валило весь процесс |
| `installCrashHandlers()` | Устанавливает `std::set_terminate` + `SetUnhandledExceptionFilter` — сетка безопасности на уровне процесса: логирует необработанное исключение/SEH в `logs/console.log` перед завершением вместо тихого краха без следа. Вызывается один раз в `WinMain` (`PlatformStartupFixups()`), сразу после `initConsole()` |
| `LOG_INFO/WARN/ERR(msg)` | Макросы логирования через ostringstream |

Глобалы: `g_logMutex`, `g_dbWriteMutex`, `g_logFile`, `g_curlDebugFile`.

---

### playerdatafetcher.h / playerdatafetcher.cpp
Загрузка данных игрока из OpenDota и STRATZ.

| Функция | Описание |
|---------|----------|
| `fetchHeroesList()` | GET /api/heroes → JSON строка |
| `parseHeroesList(json)` | JSON → vector\<HeroInfo\> |
| `fetchPlayerHeroesStats(accountId)` | GET /players/{id}/heroes → JSON |
| `fetchPlayerHeroesRankedStats(accountId)` | GET /players/{id}/heroes?lobby_type=7 → JSON |
| `parseHeroesStats(json)` | JSON → vector\<HeroStats\> |
| `fetchRecentMatchIds(accountId)` | GET /players/{id}/matches → vector\<match_id\> (90 дней, ranked) |
| `buildMatchesBatchQuery(matchIds)` | Формирование GraphQL запроса для STRATZ |
| `sendStratzMatchesBatch(token, matchIds, batchNum)` | POST STRATZ GraphQL |
| `parseAndStoreBatchMatches(db, accountId, response)` | Парсинг STRATZ ответа → SQLite таблицы. Match id разбирается через `safeStoll` (не голый `std::stoll`) — некорректная запись пропускается через `continue`, а не обрывает разбор всех ещё не обработанных батчей во внешнем catch |
| `fetchAndStorePlayerRecentData(db, token, accountId)` | Главная функция: матчи → батчи → парсинг → SQLite |
| `createHeroTableIfNotExists(db)` | Создание таблицы `heroes` |
| `createPlayerHeroTableIfNotExists(db, name)` | Создание `playerheroes` / `playerheroesranked` |
| `createPlayerRecentMatchesTableIfNotExists(db)` | Создание `playerrecentmatches` |
| `createRelevantPlayerByPosTableIfNotExists(db)` | Создание `relevantplayerherobyposstats` |
| `createPlayerHeroVsHeroByPosTableIfNotExists(db)` | Создание `playerherovsherobyposstats` |
| `createPlayerHeroWithHeroByPosTableIfNotExists(db)` | Создание `playerherowithherobyposstats` |
| `createIndexesIfNotExist(db)` | Создание индексов для всех таблиц игрока |
| `storeHeroTable(db, heroes)` | INSERT OR IGNORE героев |
| `storePlayerHeroStatsTable(db, accountId, heroes, table)` | INSERT OR REPLACE статистика героев |
| `storePlayerRecentMatches(db, accountId, matches)` | INSERT OR REPLACE история матчей |
| `storeRelevantPlayerByPos(db, accountId, rows)` | INSERT OR REPLACE статистика по позициям |
| `storePlayerHeroVsHeroByPos(db, accountId, rows)` | INSERT OR REPLACE статистика vs |
| `storePlayerHeroWithHeroByPos(db, accountId, rows)` | INSERT OR REPLACE статистика with |
| `lastCompletedWeekTimestamp()` | Понедельник 00:00 UTC последней полностью завершённой недели |
| `buildHeroStatsQuery(week)` | Формирование GraphQL запроса STRATZ `heroStats.stats` (bracket DIVINE_IMMORTAL, groupByPosition/Bracket) |
| `sendStratzHeroStats(token, week)` | POST STRATZ GraphQL для heroStats |
| `parseHeroStatsResponse(response)` | Парсинг ответа → vector\<HeroWeekStat\> (heroId, pos, matchCount, winCount) |
| `createHeroStatsTableIfNotExists(db)` | Создание таблицы `stats` |
| `storeHeroStatsTable(db, rows)` | Полная перезапись `stats` (DELETE + INSERT OR REPLACE) |
| `fetchAndStoreHeroStats(db, token)` | Главная функция: последняя неделя → STRATZ heroStats → SQLite `stats`. Не бросает исключений наружу (мягкий сбой при недоступности STRATZ) |

Структуры: `MatchDraft` (matchId, picks, positions, won), `HeroStats`, `HeroInfo`, `HeroWeekStat`.

---

### datafetcher.cpp
Оркестратор фазы 1 (разделён на две подфазы).

| Функция | Описание |
|---------|----------|
| `runDataFetcherInit(stratzToken)` | Фаза 1a: справочник героев + создание таблиц + живая мета-стата героев (`fetchAndStoreHeroStats`, STRATZ `heroStats`, последняя завершённая неделя). Не требует accountId, но требует STRATZ-токен для мета-статы (иначе — мягкий пропуск). Запускается при старте приложения |
| `runDataFetcher(accountId, stratzToken)` | Фаза 1b: загрузка данных игрока (статистика героев, матчи). Требует accountId |

---

### clouddatafetcher.h / clouddatafetcher.cpp
Синхронизация PostgreSQL → SQLite: `proherostats` и `immortalherostats` (агрегат `recentimmortalmatches`).

| Функция | Описание |
|---------|----------|
| `fetchAndStoreProHeroStats(db, connStr)` | PG `proherostats` → SQLite `proherostats`, полная перезапись |
| `fetchAndStoreImmortalHeroStats(db, connStr)` | PG `recentimmortalmatches` → агрегация (hero_id, pos) → SQLite `immortalherostats` |

Компилируется в основной exe (см. `build_unified.bat`), линкуется с libpq, но **не вызывается ни из одного рантайм-пути** (нет вызовов в `datafetcher.cpp`/`mainGUI.cpp`). Похоже на офлайн/dev-инструмент разработчика для регенерации `proherostats`/`immortalherostats` перед `scripts/pack_data.bat`, а не часть логики приложения у пользователя.

**Immortal-стата больше не читается из `immortalherostats`**: `dota_picker.cpp` теперь берёт её из живой таблицы `stats` в `playerandlivestats.db` (см. `playerdatafetcher.cpp`/`datafetcher.cpp` выше). Таблица `immortalherostats` удалена из текущего `finalapp/draft_helper_abstract_data.db` (`DROP TABLE`, содержала 635 устаревших строк без автоматического обновления). Код этого файла оставлен нетронутым как есть — при ручном запуске он всё ещё пересоздаст `immortalherostats`/`proherostats` в `data.db`, но ничто в рантайме их больше не прочитает.

---

### dota2_capture.h / dota2_capture.cpp
Захват окна Dota 2 через Windows GDI (PrintWindow).

| Класс / Функция | Описание |
|------------------|----------|
| `Resolution` | Ширина и высота окна |
| `Bitmap` | BGRA пиксели + размеры |
| `PortraitRegion` | Слот 0-9 + RECT координаты |
| `HudLayout` | Относительные координаты портретов и позиций (доли 0..1) |
| `selectStrategyLayout(w, h)` | Выбор HUD-раскладки по соотношению сторон (4:3, 16:10, 16:9, 21:9) |
| `GdiplusSession` | RAII: GDI+ Startup / Shutdown |
| `Dota2Capture` | Основной класс захвата |
| `.findGameWindow()` | Поиск окна "Dota 2" (SDL_app / Valve001), определение разрешения и раскладки |
| `.refreshResolution()` | Проверка смены разрешения → пересчёт регионов (вызывается перед каждым capturePortraits) |
| `.capturePortraits()` | Захват портретов + позиций из одного кадра (PrintWindow → BitBlt по регионам) |
| `.portraits()` | Последние захваченные портреты героев (10 Bitmap) |
| `.posPortraits()` | Последние захваченные индикаторы позиций (10 Bitmap) |
| `.captureFullWindow()` | Захват всего окна (для отладки) |
| `.saveBitmapAsPng(bmp, path)` | Сохранение Bitmap как PNG через GDI+ |
| `.savePortraits(dir)` | Сохранение всех портретов как PNG |
| `.runLoop(interval_ms)` | Цикл захвата с заданным интервалом |
| `ListAllWindows()` | Диагностика: вывод всех видимых окон |

HudLayout содержит координаты портретов (radiant_x_start, portrait_w, ...) и позиций (pos_x_start, pos_w, ...).
Раскладки: `STRATEGY_LAYOUT_16_9`, `_16_10`, `_21_9`, `_4_3`.
Диагностика (смена разрешения/раскладки, список окон) — через `LOG_INFO` (common.h), раньше `std::printf`/`std::puts`.

---

### dhash.h
Распознавание **героев** по Pearson-корреляции 8x8 серых матриц. Позиции больше не распознаются этим методом — см. `pos_ocr.h`.

| Тип / Функция | Описание |
|---------------|----------|
| `Matrix8` | 64 float: серая 8x8 матрица (zero-mean, unit-variance) |
| `HeroHashEntry` | Пара: имя героя + Matrix8 |
| `HeroMatch` | Результат: имя, score. `confident()` при score >= 0.80 |
| `pearson(a, b)` | Корреляция Пирсона двух Matrix8 (dot / 63) |
| `computeMatrix(bgra, w, h)` | BGRA-пиксели → Matrix8 (greyscale → bilinear 8x8 → нормализация) |
| `HeroRecognizer` | Поиск ближайшего героя по базе хешей |
| `PosMatch` | Результат OCR-распознавания позиции: pos (0-5), score. `confident()` при score >= 0.50 |

Порог уверенности героя: score >= 0.80 (same hero ~0.99, different ~0.0).
`PosHashEntry`/`PosRecognizer` (Pearson-подход к позициям) удалены из этого файла — заменены OCR (`pos_ocr.h`).

---

### hero_hashes.dat (runtime, не хедер)
База хешей героев для `HeroRecognizer` — **не компилируется в бинарник** (в отличие от того, что было раньше). Бинарный файл (uint32 count, затем на запись: uint16 nameLen + имя + 64 float32) лежит рядом с exe и грузится в рантайме через `loadHeroHashes("hero_hashes.dat")` (`portrait_runner.cpp`). Генерируется офлайн-инструментом `screencapture/build_hero_db.cpp` (вне `finalapp/`), устанавливается инсталлятором (см. `dota_draft_setup.iss`). Файла `hero_hashes.h` в проекте больше нет.

### pos_ocr.h
Распознавание **позиции** (1-5) через Windows OCR API (WinRT), заменяет старый Pearson-подход (`PosRecognizer`/`pos_hashes.h`).

| Тип / Функция | Описание |
|---------------|----------|
| `PosOcrRecognizer` | Создаёт `OcrEngine` для en-US и ru (fallback — язык профиля пользователя) |
| `.isAvailable()` | true, если хотя бы один движок OCR создан |
| `.recognize(bmp)` | Апскейл BGRA x4 → SoftwareBitmap → OCR (en, затем ru) → `textToPosition` |
| `textToPosition(text)` | Ключевые слова EN ("safe/mid/off/soft/hard/supp/lane") и RU ("центр/мид/сложн/полн.../жёстк/лёгк/мягк/поддерж") → позиция 1-5 |

OCR либо уверенно распознаёт (score=1.0), либо возвращает pos=0 (score=0) — бинарный результат, порог `confident()` из `dhash.h` (>=0.50) здесь не критичен.
Инклюдит `common.h` (для `LOG_INFO`) — диагностика количества доступных OCR-движков теперь через `LOG_INFO`, раньше через `std::printf` (невидимо без консоли).

**Файла `pos_hashes.h` в проекте больше нет** — был мёртвым кодом (`g_pos_db[]` использовал тип `PosHashEntry`, не определённый нигде в репозитории; ничего его не инклюдило), удалён.

---

### livestatsfetcher.cpp
GSI HTTP-сервер.

| Функция | Описание |
|---------|----------|
| `runGsiServer(gameInfo)` | Запуск HTTP-сервера, приём GSI-данных |
| `parsePhaseStr(s)` | Строка GSI game_state → GamePhase |
| `handle_request(raw)` | Обработка HTTP: POST / → парсинг GSI + обновление lastUpdate, GET /phase → JSON статус. `team_slot` разбирается через `safeStoi` + клэмп 0..4 (не голый `std::stoi`) — раньше исключение на нечисловом значении из непроверенного GSI-payload'а убивало весь процесс (детач-поток без сетки) |
| `client_thread(client)` | Обработчик соединения, запускается через `std::thread(...).detach()`. Всё тело обёрнуто в try/catch — раньше не было ни одной сетки на этом пути |

---

### portrait_runner.h / portrait_runner.cpp
Захват и распознавание портретов + позиций + overlay-кнопка [D].

| Функция | Описание |
|---------|----------|
| `startDotaOverlay()` | Запуск прозрачной кнопки [D] поверх Dota 2 (один раз при старте) |
| `loadHeroHashes(path)` | Загрузка `hero_hashes.dat` (бинарный формат) в vector<HeroHashEntry> для `HeroRecognizer` |
| `runPortraitCapture(gameInfo, dbPath, running, out)` | Цикл захвата каждые 500мс → `HeroRecognizer` (Pearson) для героев + `PosOcrRecognizer` (Windows OCR) для позиций → запись в livepicks. Тело цикла обёрнуто в try/catch на итерацию (логирует и продолжает, не убивая поток — раньше во всей функции не было ни одного try/catch). На ветке ошибки открытия БД теперь вызывается `winrt::uninit_apartment()` (раньше пропускался, в отличие от нормального пути через `cleanup:`). Диагностика — через `LOG_INFO/WARN/ERR` (раньше `printf`/`puts`, невидимые в GUI-приложении без консоли) |
| `updateSlot(db, slot, heroId)` | Обновление hero в слоте livepicks |
| `updateSlotPos(db, slot, pos)` | Обновление позиции в слоте livepicks |
| `clearHeroSlots(db)` | Обнуление всех hero + pos слотов в livepicks |
| `clearHeroSlot(db, slot)` | Обнуление одного hero-слота |
| `overlayProc(hwnd, msg, wp, lp)` | WndProc overlay: таймер позиционирования + клик-переключение |
| `paintLayeredButton(hwnd)` | Per-pixel alpha отрисовка [D] серым цветом через UpdateLayeredWindow |
| `selectOverlayPos(w, h)` | Выбор позиции кнопки по аспекту |
| `bringAppToFront()` | Вывод главного окна приложения на передний план |
| `bringDotaToFront(cap)` | Вывод окна Dota 2 на передний план |

Распознавание позиций: только для своей команды (manualPos override > screen capture > 0). Вражеские позиции = 0.

---

### dota_picker.cpp
ML-пикер: CatBoost инференс + рекомендации героев.

| Функция / Класс | Описание |
|------------------|----------|
| `FeatureVector` | 10 категориальных + 42 числовых признака для CatBoost |
| `buildVector(v, lp, ...)` | Заполнение вектора признаков из LivePick + matchup-данных + mastery |
| `ModelHandle` | RAII для `ModelCalcerHandle*` (CatBoost C API) — раньше модель освобождалась вручную только на пути нормального завершения цикла, при исключении посреди инференса память утекала |
| `loadHeroes(db)` | SQLite → map\<id, name\> (через общие `SqliteDB`/`SqliteStmt` из common.h) |
| `loadMatchupData(dataDb)` | Data DB → MatchupData (global_wr, vs_wr, with_wr, modal_pos, hero_pos_wr) |
| `loadImmortalHeroStats(db, pos)` | Player DB, таблица `stats` (живой STRATZ, DIVINE_IMMORTAL) → map\<hero_id, PickerHeroStat\>. Порог отсечения — динамический: 1% от суммарных игр на позиции за неделю (не фиксированное число) |
| `loadPlayerStats(db, account_id)` | Player DB → map\<hero_id, PlayerStats\> |
| `loadLatestLivePick(db, lp)` | Последняя строка livepicks → LivePick |
| `runBatch(model, batch)` | Батч-инференс CatBoost → sigmoid → P(radiant_win) |
| `renderToGui(state, lp, ...)` | Запись результатов в GuiPickerState (слоты + winProb + top-10) + inferenceGen++ |
| `runPickerGui(modelPath, dbPath, running, guiState, portraitState)` | Главный цикл пикера: poll livepicks → renderToGui каждые 500мс |

Matchup-данные читаются из `{modelPath}_data.db` (отдельная БД). Immortal/мета-стата — из `playerandlivestats.db` (`stats`), не из data.db.
Schema gate: перед загрузкой модели проверяет `meta.schema_version == kSupportedSchema` (мета-стата в схему не входит — `buildVector` её не использует).

---

### version.h
Константы версии приложения.

| Константа | Описание |
|-----------|----------|
| `kAppVersion` | Версия приложения (напр. "0.1.2") |
| `kSupportedSchema` | Поддерживаемая версия схемы данных |
| `kManifestUrl` | URL manifest.json на raw.githubusercontent.com |

---

### version_utils.h / version_utils.cpp
Чтение/запись локального состояния версий.

| Функция / Тип | Описание |
|----------------|----------|
| `DataMeta` | Структура: schema, dataVersion (из meta-таблицы _data.db) |
| `readDataDbMeta(path)` | Читает schema_version и data_version из meta-таблицы _data.db |

Файл сейчас содержит только это — `version.json`/`LocalVersionInfo`/`loadLocalVersion`/`saveLocalVersion` **больше не существуют**. Локальное состояние версий отдельно нигде не хранится: `checkForUpdates` каждый раз сравнивает манифест напрямую с содержимым файловой системы (SHA-256 каждого файла), а не с сохранённым снимком версии.

---

### updater.h / updater.cpp
Система авто-обновлений. `ManifestInfo.dataFiles` — общий map<ключ, {url, sha256}>; логика ниже одинаково работает для любых файлов, перечисленных в манифесте (не только .cbm/.db).

| Функция / Тип | Описание |
|----------------|----------|
| `UpdateAction` | Enum: NONE, APP_UPDATE, DATA_UPDATE, SCHEMA_TOO_NEW |
| `ManifestInfo` | Структура: версии app/data, URL, SHA-256, mandatory, dataFiles (map) |
| `fetchManifest(out)` | GET manifest.json с SSL verify, connect 5с, total 10с |
| `checkForUpdates(manifest)` | App: kAppVersion vs manifest. Data: SHA-256 **каждого** файла из dataFiles на диске vs манифест → UpdateAction |
| `downloadToStaging(url, sha256, path, progress)` | Скачивание в .part + SHA-256 через BCrypt + rename. Создаёт вложенные директории назначения (`ensureParentDir`). Логирует `LOG_ERR`/`LOG_WARN` на всех путях отказа (`curl_easy_init`, открытие .part, сетевая ошибка/не-200) — раньше эти ветки были беззвучными |
| `fileSha256(path)` | SHA-256 файла через BCrypt API (64KB чанки) |
| `downloadAndStageData(manifest, stagedFiles, progress)` | **Выборочно**: для каждого файла манифеста сначала проверяет `localFileUpToDate` (существует + SHA-256 совпадает) — совпавшие пропускает без скачивания; скачивает только отсутствующие/изменившиеся, их ключи возвращает через `stagedFiles` (out-параметр) |
| `swapDataFiles(manifest, stagedFiles)` | Backup (.bak) + MoveFileEx только для файлов из `stagedFiles` (не всего манифеста) + `cleanupObsoleteAssets` + удаление .bak/lock. Пустой `stagedFiles` → no-op |
| `cleanupObsoleteAssets(manifest)` | Удаляет `assets\*.png`, которых нет среди ключей `assets/*` в манифесте (герой переименован/убран) — иначе маска `assets\*.png` только добавляет/перезаписывает и никогда не подчищает лишнее |
| `rollbackDataFiles()` | Без ManifestInfo (вызывается до fetchManifest): восстанавливает `*.bak` и `assets\*.bak` по маске, а не по жёстко заданному списку файлов |
| `checkPendingSwap()` | Проверка swap.lock при старте → rollback если найден |
| `cleanupStaging()` | Удаление orphan `.part` в staging/ и staging/assets/ |
| `compareVersions(a, b)` | Посегментное числовое сравнение версий |

SSL verification включена (CURLSSLOPT_NATIVE_CA). Не использует общий applyCurlNetworkOpts.

**Ассеты и hero_hashes.dat в манифесте** (добавлено, см. `scripts/update_assets.ps1`): в отличие от `.cbm`/`_data.db` (версионированные GitHub Releases, `scripts/pack_data.bat`), `hero_hashes.dat` и каждый `assets/<name>.png` попадают в `manifest.json → data.files` с `url` на `raw.githubusercontent.com/.../main/finalapp/...` — т.е. приложение сверяется напрямую с содержимым main-ветки репозитория, без отдельных релизов/версий для ассетов. При каждом старте `checkForUpdates` хэширует и эти файлы; расхождение (или лишний PNG, которого больше нет в манифесте) чинится тем же DATA_UPDATE-путём.

---

### mainGUI.cpp
GUI: D3D11-инициализация, `WndProc`, `RenderFrame`, точка входа. Остальное (оркестратор, панели, окно апдейтера, общее состояние) вынесено в отдельные файлы — см. секции ниже. Файл сократился с 2720 до ~690 строк.

| Функция | Описание |
|---------|----------|
| `InitD3D(hwnd)` | Инициализация D3D11 device + swap chain |
| `CleanupD3D()` | Освобождение D3D11 ресурсов |
| `ApplyStyle()` | Тёмная тема ImGui с масштабированием 1.25x, палитра — из gui_draw.h |
| `WndProc(hWnd, msg, ...)` | WM_GETMINMAXINFO (мин. размер окна), WM_SIZE (ресайз swap chain + живой рендер во время интерактивного resize), WM_DESTROY |
| `RenderFrame()` | Главный кадр: аватар-текстура из буфера → root window → Header → StatusBar → баннеры (`schemaError` пикера + единый `g_appNotice`) → Draft + Picks + Meta Heroes (3 колонки, все панели — из gui_draw.h) |
| `PresentFrame()` | ImGui NewFrame → RenderFrame → Render → D3D11 Present |
| `CreateDIcon(sz)` | Программная отрисовка иконки [D] через GDI (фоллбэк, если ресурс иконки не загрузился) |
| `PlatformStartupFixups()` | CWD/exe-dir фикс, DPI awareness, `initConsole()`, `installCrashHandlers()` |
| `RunStartupUpdateCheck(hInst)` | Блокирующая проверка обновлений (до D3D11/ImGui), делегирует в update_window.h. `false` → приложение должно завершиться сейчас |
| `InitPlayerStateAndBackgroundThreads()` | Конфиг из env, загрузка сохранённого игрока (`SqliteDB`), запуск фазы 1a, `startOrchestrator()`, условный `startPhase1()` |
| `CreateMainWindow(hInst)` | Класс окна, размеры, `CreateWindowW`, тёмная тема DWM, иконка |
| `InitGuiAndAssets(hwnd)` | `InitD3D` + ImGui/шрифты + `ApplyStyle` + `loadHeroPortraits()`. `false` при провале `InitD3D` — единственная явная fatal-ветка |
| `RunMessageLoop()` | `PeekMessage`/`DispatchMessage`/`PresentFrame`, с try/catch вокруг `PresentFrame()` (раньше не было ни одной сетки на GUI-потоке) |
| `ShutdownApp(hInst, hwnd, gdipToken)` | `stopOrchestrator()`, освобождение текстур (`unloadHeroPortraits()`), шатдаун ImGui/D3D11/GDI+ |
| `WinMain(hInst, ...)` | Точка входа: последовательный вызов подфункций выше (~25 строк вместо прежних ~300) |

---

### app_state.h / app_state.cpp
Состояние, реально расшаренное между mainGUI.cpp, orchestrator.cpp и gui_draw.cpp (глобал живёт здесь, только если читается/пишется из ≥2 файлов — иначе инкапсулирован за функциями своего файла).

| Тип / Переменная | Описание |
|------------------|----------|
| `PlayerState` / `g_player` | Данные игрока: accountId, name, phase1Running/Done/Error/Msg, avatarData — пишут фоновые потоки (orchestrator.cpp), читает GUI |
| `g_gameInfo`, `g_portraitState`, `g_pickerState` | Экземпляры общих типов из shared_types.h |
| `g_stratzToken` | STRATZ API токен (из env или умолчания) |
| `DB_PATH`, `MODEL_PATH`, `PHASE3_TAIL_SEC` | `inline constexpr`-константы |
| `g_avatarSRV` | Текстура аватара — пересоздаётся в mainGUI.cpp (RenderFrame), рендерится в gui_draw.cpp (DrawHeader), освобождается перед рефетчем в orchestrator.cpp (startPhase1) |
| `g_Device` | D3D11-устройство — владелец mainGUI.cpp (InitD3D/CleanupD3D), нужен и gui_draw.cpp (createTextureFromImageData) |
| `g_Hwnd` | Хендл главного окна — владелец mainGUI.cpp (CreateMainWindow), нужен gui_draw.cpp (клик по логотипу [D] в DrawHeader) |
| `AppNotice` / `g_appNotice` | Единый канал уведомлений для GUI (мьютекс + `active` + `level` + `text[256]`). Заменяет прежний файл-статик `g_schemaBannerNeeded` (покрывал только SCHEMA_TOO_NEW). Пишут: `RunStartupUpdateCheck`, `orchestrator.cpp`/`portrait_runner.cpp` при провале открытия БД. Читает: `RenderFrame`. `GuiPickerState::schemaError` — отдельный, самодостаточный механизм пикера, оба баннера сосуществуют независимо |

---

### update_window.h / update_window.cpp
Нативное Win32-окно "Checking for updates..." (до инициализации D3D11/ImGui). Самодостаточный блок без зависимостей от остального состояния приложения, механически перенесён из mainGUI.cpp.

| Функция | Описание |
|---------|----------|
| `CreateUpdateWindow(hInst)` | Окно обновления: статус + процент + кнопка retry |
| `DestroyUpdateWindow()` | Уничтожение окна |
| `SetUpdateStatus(text)` | Обновление текста статуса |
| `SetUpdateProgress(label, percent)` | Обновление только процента (без мерцания) |
| `ShowRetryButton(show)` | Показ/скрытие кнопки "Try again" |
| `WaitForRetryClick()` | Message loop до нажатия "Try again" |
| `PumpMessages()` | Прокачка очереди сообщений без блокировки (между шагами обновления и внутри Set*) |

---

### orchestrator.h / orchestrator.cpp
Фоновый оркестратор: управление GSI/portrait/picker потоками, livepicks/player_info, запуск фазы 1b. Вынесен из mainGUI.cpp (была `orchestratorMain`, ~315 строк — самая длинная функция в проекте, без единого top-level try/catch). Тред-менеджмент (`g_pickerRunning`/`g_portraitRunning`/потоки/`g_posRefreshNeeded`/`g_orchestratorRunning`) — `static` внутри файла, наружу только функции.

| Функция | Описание |
|---------|----------|
| `startOrchestrator()` / `stopOrchestrator()` | Запуск/остановка фонового потока оркестратора (из WinMain) |
| `startPhase1(accountId)` | Запуск DataFetcher (1b) + имя/аватар игрока (OpenDota) в фоновом потоке |
| `requestPositionRefresh()` | Сигнал оркестратору: пользователь вручную сменил позицию в GUI (вызывается из gui_draw.cpp вместо прямой записи в приватный атомик) |
| `createPlayerInfoTable(db)` / `loadSavedPlayer(db, nameOut)` | Таблица `player_info` (экспортированы — используются и из WinMain при старте) |
| `orchestratorMain()` | Фоновый цикл (300мс), разбит на 8 именованных шагов с общей `OrchestratorLoopState` (вместо function-local `static`): `refreshAccountId`, `readGameStateWithTimeoutWatchdog` (GSI + 15с watchdog), `syncSlotSideToDb`, `handleAccountIdChanged`, `handleNewMatch` (safeStoll), `runPhaseStateMachine` (IDLE/POSTGAME/HERO_SELECTION/tail), `syncPortraitOnlyToGui`, `runOneShotRefresh`. Открытие БД — в try/catch (fatal при провале, пишет в `g_appNotice`); каждая итерация цикла — во внутреннем try/catch (логирует и продолжает, не убивая поток) |

Оркестратор:
- Portrait capture стартует при HERO_SELECTION **без accountId**
- Picker стартует при HERO_SELECTION/DRAFT **с accountId**
- Portrait→GUI sync: когда portrait работает но picker нет — показывает драфт из g_portraitState
- GSI таймаут: 15с без обновлений → IDLE
- One-shot: при смене позиции в GUI вне фазы 3 → запись в DB + запуск пикера до первого inferenceGen
- При idChanged: обновляется только accountId в livepicks, portrait capture не останавливается

---

### gui_draw.h / gui_draw.cpp
Панели ImGui (Header/StatusBar/Draft/Picks/Meta Heroes), кэш портретов героев, кэш меты, кэш отображаемых имён. Палитра (`kBg`...`kAmber`) и цветовые хелперы `C()`/`Ca()` объявлены здесь же — нужны и панелям, и mainGUI.cpp (ApplyStyle, RenderFrame).

| Функция | Описание |
|---------|----------|
| `DrawHeader(fullW)` | Шапка: логотип [D], заголовок, карточка игрока / ввод Friend ID |
| `DrawStatusBar(fullW)` | Полоса статуса: Player data (no ID/pending/fetching/ready/error) + Refresh + Game phase + match ID. При `phase1Error` теперь показывает реальный `phase1Msg`, а не захардкоженную заглушку "error" |
| `DrawDraftPanel(panelW)` | Левая панель: слоты Radiant/Dire + полоса winProb |
| `DrawPicksPanel(panelW)` | Средняя панель: рекомендации top-10 / выбранный герой (ML) |
| `DrawMetaHeroesPanel(panelW)` | Правая панель: топ-10 героев по популярности (STRATZ, сумма матчей, без ML), винрейт — вторичная цветная метрика. Оформление как у DrawPicksPanel. Позиция бейджа следует за НАШЕЙ настоящей позицией в драфте (клик вызывает `SetOurPosition`, унифицировано с Draft/Picks). Когда наш герой выбран — вместо топ-10 показывает единственную выделенную строку "YOUR HERO" из нефильтрованного `g_metaHeroLookup` (виден даже без прохождения 1%-порога) |
| `DrawHeroStatRow(rowW, params)` | Общая строка героя со статистикой (портрет/имя/вторичная стата/win%+бар). Раньше — две почти идентичные лямбды `DrawPickRow`/`DrawMetaRow`, продублированные в DrawPicksPanel/DrawMetaHeroesPanel; теперь общая функция + `HeroStatRowParams` (secondaryStats — предформатированная строка, пустая = без колонки статы) |
| `DrawWinRateLegend(rowW)` | Легенда цветов win-rate (`>=55%`/`50-55%`/`<50%`) — раньше дублировалась в обеих панелях |
| `SectionLabelWithPosBadge(label, panelW, position, onPositionPick=nullptr)` | Заголовок секции + position-бейдж (сокращается/переносится на узкой панели). `onPositionPick` — `std::function<void(int)>`; если задан, бейдж кликабельный: popup All/Position 1-5, выбор вызывает `onPositionPick(p)` (0 = All). DrawPicksPanel/DrawMetaHeroesPanel передают `SetOurPosition` |
| `SetOurPosition(newPos)` | Меняет НАШУ настоящую позицию в текущем драфте (своп с тиммейтом при конфликте, запись в `g_portraitState.manualPos` + `requestPositionRefresh()`) — единая точка изменения позиции для бейджей Picks/Meta и слотов Draft |
| `DrawHeroSlot(rowW, h, ...)` | Слот героя с кликабельным popup позиции (1-5, свап); popup вызывает `requestPositionRefresh()` напрямую |
| `DrawPortrait(dl, p, sz, ...)` | Квадрат портрета: PNG-текстура или инициалы |
| `loadHeroPortraits()` / `unloadHeroPortraits()` | Загрузка PNG из assets/ → кэш `g_heroPortraits` / освобождение при завершении (вызывается из `ShutdownApp`) |
| `createTextureFromImageData(data, size)` | D3D11-текстура из байт JPEG/PNG — используется и здесь (портреты), и в mainGUI.cpp (RenderFrame — текстура аватара) |
| `heroDisplayName(heroId, fallback)` | heroId → localized_name для отображения, ленивая загрузка через `SqliteDB(readOnly=true)` |
| `loadMetaHeroStatsIfNeeded()` | Ленивая загрузка `stats` из `playerandlivestats.db` в `g_metaHeroStats` (по позиции 1-5 + агрегат по всем под ключом 0), с порогом 1% от суммарных игр, сортировка по сумме матчей (games) убыв. Повторяет попытку каждый кадр, пока таблица пуста — тот же паттерн, что `heroDisplayName()`. Читает БД через `SqliteDB(readOnly=true)` |

---

### build_unified.bat
Скрипт сборки: cl.exe (MSVC) + vcpkg + CatBoost. Список исходников дополнен новыми файлами разбивки mainGUI.cpp: `app_state.cpp`, `update_window.cpp`, `orchestrator.cpp`, `gui_draw.cpp` (компилируются в общий `Dota_Drafter.exe`, без изменения структуры сборки — по-прежнему один `cl.exe`-инвок, без CMake).

Ключевые пути:
- vcpkg: `C:\vcpkg\installed\x64-windows-static`
- CatBoost: `C:\catboost`
- Результат: `build\Dota_Drafter.exe`

---

### installer/dota_draft_setup.iss (../installer/, вне finalapp/)
Inno Setup скрипт полной установки/апдейта приложения (канал `APP_UPDATE` — не `updater.cpp`, тот меняет только `.cbm`/`_data.db`).

Ставит: exe, `catboostmodel.dll`, `draft_helper_abstract.cbm`/`_data.db`, `hero_hashes.dat`, `assets\*.png`, GSI-конфиг.

`[InstallDelete] Type: filesandordirs; Name: "{app}\assets"` — папка `assets` полностью удаляется **перед** копированием новых файлов. Нужно, т.к. `[Files]` с маской `assets\*.png` в Inno Setup только добавляет/перезаписывает файлы, совпадающие с источником, но никогда не удаляет из `{app}\assets` файлы, пропавшие из источника (переименованный/убранный герой) — без этого шага устаревшие PNG копились бы в установке пользователя при каждом обновлении приложения.

---

## Базы данных

### playerandlivestats.db — данные игрока и runtime

| Таблица | Ключ | Описание |
|---------|------|----------|
| `heroes` | id | Справочник героев (name, localized_name). Заполняется фазой 1a |
| `playerheroes` | (account_id, hero_id) | Статистика героев игрока (все режимы) |
| `playerheroesranked` | (account_id, hero_id) | Статистика героев (только ranked) |
| `playerrecentmatches` | (match_id, account_id) | История матчей с пиками/позициями |
| `relevantplayerherobyposstats` | (account_id, heroId, position) | Агрегат героя+позиции из матчей |
| `playerherovsherobyposstats` | (account_id, hero_id, pos, vs_hero_id, vs_pos) | Статистика hero vs hero |
| `playerherowithherobyposstats` | (account_id, hero_id, pos, with_hero_id, with_pos) | Статистика hero with hero |
| `livepicks` | single row | Текущий драфт: 10 hero-слотов + 10 pos-слотов + метаданные матча |
| `player_info` | account_id | Сохранённый Friend ID + имя |
| `stats` | (hero_id, pos) | Живая мета-стата героев (STRATZ `heroStats`, bracket DIVINE_IMMORTAL, последняя завершённая неделя). Полная перезапись при каждом старте фазой 1a. Используется `dota_picker.cpp` (фильтрация пула кандидатов + "imm %") и GUI-панелью Meta Heroes |

### draft_helper_abstract_data.db — данные модели

| Таблица | Ключ | Описание |
|---------|------|----------|
| `meta` | key | Метаданные: `schema_version`, `data_version` |
| `global_wr` | hero_id | Smoothed winrate героя (prior=100) |
| `vs_wr` | (hero_id, opp_hero_id) | Пайрвайзный winrate hero vs hero (prior=20) |
| `with_wr` | (hero_id, ally_hero_id) | Пайрвайзный winrate hero with hero (prior=20) |
| `modal_pos` | hero_id | Модальная позиция героя (1-5) |
| `hero_pos_wr` | (hero_id, pos) | Winrate героя на позиции (prior=50) |

`immortalherostats` больше не входит в эту БД — удалена (`DROP TABLE`, была статичной, без автообновления). Immortal/мета-стата теперь живая и лежит в `playerandlivestats.db` (`stats`).

---

## Система обновлений

### Поток запуска (WinMain)

```
CWD → curl_global_init → GDI+
  → checkPendingSwap() + cleanupStaging()
  → CreateUpdateWindow()
  → retry loop: fetchManifest() или "Failed to check" + "Try again"
  → checkForUpdates():   // SHA-256 каждого файла из manifest.dataFiles, без сохранённого снимка версии
      APP_UPDATE  → download → sha256 → bat(3s delay + installer /SILENT) → exit
      DATA_UPDATE → downloadAndStageData() [только несовпавшие файлы] → swapDataFiles() [только скачанные] (+ cleanupObsoleteAssets) → continue
      SCHEMA_TOO_NEW → banner flag
  → DestroyUpdateWindow()
  → config → DB → Phase 1a → orchestrator → Phase 1b → D3D11 → ImGui → loop
```

Нет `version.json`/сохранённого локального снимка версии — на каждом старте `checkForUpdates` заново хэширует все файлы из `manifest.dataFiles` и сравнивает с манифестом, вместо сравнения версий.

### Файлы версионирования

| Файл | Расположение | Описание |
|------|-------------|----------|
| `manifest.json` | GitHub repo root (raw.githubusercontent, main) | Источник правды: `app` (версия/URL/sha256 инсталлятора), `data.files` — map ключ→{url, sha256} |
| `version.h` | Исходный код | Компилируемые константы: kAppVersion, kSupportedSchema |
| `version.rc` | Исходный код | RC-ресурс с параметризованной версией (VER_MAJOR/MINOR/PATCH) |
| `meta` таблица | _data.db | schema_version + data_version внутри пакета данных |

`manifest.data.files` смешивает два разных канала доставки по ключу:
- `draft_helper_abstract.cbm` / `draft_helper_abstract_data.db` — версионированные GitHub Releases (`data-<DVER>`), управляются `pack_data.bat`
- `hero_hashes.dat` и `assets/<hero>.png` — **без релизов/версий**, url = `raw.githubusercontent.com/.../main/finalapp/...`; приложение сверяется напрямую с текущим состоянием main-ветки, управляются `update_assets.ps1`

### Скрипты релиза

| Скрипт | Описание |
|--------|----------|
| `scripts/release_app.bat VERSION` | Обновить version.h → build → installer → gh release → manifest.json (app) |
| `scripts/pack_data.bat DVER SCHEMA` | meta в _data.db → gh release → manifest.json (cbm/db) |
| `scripts/update_assets.ps1` (+ `.bat` обёртка) | Пересчитать sha256 hero_hashes.dat + assets/*.png → manifest.json (raw main URL, без релиза) → commit + push |

VS Code Tasks: "Release App", "Release Data", "Update Assets".

---

## Внешние API

| API | Использование |
|-----|---------------|
| OpenDota `/api/heroes` | Справочник героев (фаза 1a, без accountId) |
| OpenDota `/api/players/{id}/heroes` | Статистика героев игрока (фаза 1b) |
| OpenDota `/api/players/{id}/matches` | Список match_id (90 дней, ranked) (фаза 1b) |
| STRATZ GraphQL | Батч-запрос деталей матчей (фаза 1b) |
| STRATZ GraphQL `heroStats.stats` | Живая мета-стата героев по позициям, bracket DIVINE_IMMORTAL, последняя завершённая неделя (фаза 1a, без accountId) |
| OpenDota `/api/players/{id}` | Профиль игрока: personaname, avatarmedium (фаза 1b) |
| Dota 2 GSI | Состояние игры в реальном времени |

---

## ML-модель (CatBoost)

Одна модель: `draft_helper_abstract.cbm`.
Данные модели: `draft_helper_abstract_data.db`.

Вход: **10 категориальных** (имена героев) + **42 числовых** признака.
Выход: logit → sigmoid → P(radiant_win).

### Вектор признаков (buildVector)

| Индексы | Группа | Размер | Описание |
|---------|--------|--------|----------|
| cat 0-4 | r_hero | 5 cat | Имена героев Radiant. `"unknown"` для пустых |
| cat 5-9 | d_hero | 5 cat | Имена героев Dire |
| flt 0-29 | matchup | 30 | По 3 на слот: global_wr, avg_vs, avg_with |
| flt 30 | mastery_wr | 1 | Smoothed WR игрока на герое-кандидате (prior=30) |
| flt 31 | mastery_games | 1 | log1p(games) игрока на кандидате |
| flt 32-41 | hero_pos_wr | 10 | WR героя на позиции. Своя команда = из livepicks, враг = modal_pos |

**Итого**: 10 categorical + 42 float = 52 признака.

### Matchup-фичи (per slot)

- `global_wr`: smoothed winrate героя из `global_wr` таблицы
- `avg_vs`: среднее vs_wr против раскрытых врагов (из `vs_wr` таблицы)
- `avg_with`: среднее with_wr с раскрытыми союзниками (из `with_wr` таблицы)
- Для пустых слотов: все значения = 0.5

### Позиции

- Своя команда: из livepicks (screen capture или GUI manual override)
- Вражеская команда: из `modal_pos` таблицы (модальная позиция героя)
