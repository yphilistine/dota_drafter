# Dota Drafter — Контекст проекта

## Архитектура

Приложение — Dota 2 Draft Assistant с ImGui/D3D11 GUI.
Потоковая модель: GUI-поток, оркестратор, GSI-сервер, DataFetcher, Portrait capture, Picker.

Три фазы:
1. **DataFetcher** — загрузка данных игрока (OpenDota, STRATZ, PostgreSQL → SQLite)
2. **GSI-сервер** — приём состояния игры от Dota 2 через Game State Integration (порт 3000)
3. **Portrait + Picker** — захват портретов HUD + ML-рекомендации (CatBoost)

---

## Файлы и функции

### shared_types.h
Общие типы данных для межпоточного взаимодействия.

| Тип | Описание |
|-----|----------|
| `GamePhase` | Enum: IDLE, DRAFT, INGAME, POSTGAME |
| `phaseName(GamePhase)` | Возвращает строковое имя фазы |
| `GameInfo` | Состояние матча от GSI (mutex-protected) |
| `PortraitResult` | Результат распознавания одного портрета (имя, id, score) |
| `SharedPortraitState` | 10 слотов портретов + флаг active (mutex-protected) |
| `HeroSlotGui` | Данные одного слота для отрисовки в GUI |
| `PickRowGui` | Строка рекомендации: герой, winProb, статистика |
| `GuiPickerState` | Полное состояние пикера для GUI (radiant/dire, рекомендации, winProb) |

Декларации: `runDataFetcher`, `runGsiServer`, `runPortraitCapture`, `runPickerGui`.

---

### common.h / common.cpp
Общие утилиты: логирование, HTTP, RAII-обёртки.

| Функция / Класс | Описание |
|------------------|----------|
| `CurlGlobal` | RAII: curl_global_init / cleanup |
| `CurlHeaders` | RAII: curl_slist |
| `CurlHandle` | RAII: curl_easy_init / cleanup |
| `SqliteDB` | RAII: sqlite3_open / close + PRAGMA |
| `SqliteTransaction` | RAII: BEGIN / COMMIT / ROLLBACK |
| `initConsole()` | Инициализация логов, ANSI, curl debug файла |
| `logConsole(level, msg)` | Логирование с таймстампом и цветами |
| `ensureLogsDir()` | Создание папки logs/ |
| `httpGet(url)` | HTTP GET с ретраями (3 попытки, 10 сек пауза) |
| `httpPost(url, body, token)` | HTTP POST с ретраями и авторизацией |
| `sanitizeUtf8(input)` | Очистка невалидных UTF-8 последовательностей |
| `LOG_INFO/WARN/ERR(msg)` | Макросы логирования через ostringstream |

Глобалы: `g_logMutex`, `g_dbWriteMutex`, `g_logFile`, `g_curlDebugFile`.

---

### clouddatafetcher.h / clouddatafetcher.cpp
Синхронизация PostgreSQL → SQLite.

| Функция / Класс | Описание |
|------------------|----------|
| `PgConnection` | RAII: PQconnectdb / PQfinish |
| `PgResult` | RAII: PGresult / PQclear |
| `ProHeroStats` | Структура: hero_id, pos, games, wins, bans |
| `ImmortalHeroStats` | Структура: hero_id, pos, games, wins, bans |
| `fetchAndStoreProHeroStats(db, connStr)` | PG → SQLite таблица `proherostats` |
| `fetchAndStoreImmortalHeroStats(db, connStr)` | PG `recentimmortalmatches` → SQLite `immortalherostats` (агрегация по hero_id+pos) |

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
| `parseAndStoreBatchMatches(db, accountId, response)` | Парсинг STRATZ ответа → SQLite таблицы |
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

Структуры: `MatchDraft` (matchId, picks, positions, won), `HeroStats`, `HeroInfo`.

---

### datafetcher.cpp
Оркестратор фазы 1.

| Функция | Описание |
|---------|----------|
| `runDataFetcher(accountId, stratzToken)` | Загрузка всех данных: герои, статистика, матчи, PG-синхронизация. Возвращает 0 при успехе |

---

### dota2_capture.h / dota2_capture.cpp
Захват окна Dota 2 через Windows GDI (PrintWindow).

| Класс / Функция | Описание |
|------------------|----------|
| `Resolution` | Ширина и высота окна |
| `Bitmap` | BGRA пиксели + размеры |
| `PortraitRegion` | Слот 0-9 + RECT координаты |
| `HudLayout` | Относительные координаты портретов (дроби 0..1) |
| `selectStrategyLayout(w, h)` | Выбор HUD-раскладки по соотношению сторон (4:3, 16:10, 16:9, 21:9) |
| `GdiplusSession` | RAII: GDI+ Startup / Shutdown |
| `Dota2Capture` | Основной класс захвата |
| `.findGameWindow()` | Поиск окна "Dota 2" (SDL_app / Valve001), определение разрешения и раскладки |
| `.capturePortraits()` | Захват всех портретов из одного кадра (PrintWindow → BitBlt по регионам) |
| `.captureFullWindow()` | Захват всего окна (для отладки) |
| `.saveBitmapAsPng(bmp, path)` | Сохранение Bitmap как PNG через GDI+ |
| `.savePortraits(dir)` | Сохранение всех портретов как PNG |
| `.runLoop(interval_ms)` | Цикл захвата с заданным интервалом |
| `ListAllWindows()` | Диагностика: вывод всех видимых окон |

Раскладки: `STRATEGY_LAYOUT_16_9`, `_16_10`, `_21_9`, `_4_3`.

---

### dhash.h
Распознавание героев по Pearson-корреляции 8x8 серых матриц.

| Тип / Функция | Описание |
|---------------|----------|
| `Matrix8` | 64 float: серая 8x8 матрица (zero-mean, unit-variance) |
| `HeroHashEntry` | Пара: имя героя + Matrix8 |
| `HeroMatch` | Результат: имя, score. `confident()` при score >= 0.80 |
| `pearson(a, b)` | Корреляция Пирсона двух Matrix8 (dot / 63) |
| `computeMatrix(bgra, w, h)` | BGRA-пиксели → Matrix8 (greyscale → bilinear 8x8 → нормализация) |
| `HeroRecognizer` | Поиск ближайшего героя по базе хешей |
| `.recognize(bgra/bmp)` | Распознавание: computeMatrix → findNearest → HeroMatch |

Порог уверенности: score >= 0.80 (same hero ~0.99, different ~0.0).

---

### livestatsfetcher.cpp
GSI HTTP-сервер + Steam API поллер.

| Функция | Описание |
|---------|----------|
| `runGsiServer(gameInfo, steamApiKey)` | Запуск HTTP-сервера на порту 3000, приём GSI-данных |
| `parsePhaseStr(s)` | Строка GSI game_state → GamePhase |
| `handle_request(raw)` | Обработка HTTP: POST / → парсинг GSI, GET /phase → JSON статус |
| `poll_loop()` | Фоновый поллинг Steam API (GetMatchDetails) — только логирование |
| `load_heroes()` | Загрузка справочника героев из SQLite |

---

### portrait_runner.h / portrait_runner.cpp
Захват и распознавание портретов + overlay-кнопка [D].

| Функция | Описание |
|---------|----------|
| `startDotaOverlay()` | Запуск прозрачной кнопки [D] поверх Dota 2 (один раз при старте) |
| `runPortraitCapture(gameInfo, dbPath, running, out)` | Цикл захвата портретов каждые 500мс → распознавание → запись в livepicks |
| `updateSlot(db, slot, heroId)` | Обновление одного слота в таблице livepicks |
| `clearHeroSlots(db)` | Обнуление всех hero-слотов в livepicks |
| `clearHeroSlot(db, slot)` | Обнуление одного слота |
| `overlayProc(hwnd, msg, wp, lp)` | WndProc overlay: таймер позиционирования + клик-переключение |
| `paintLayeredButton(hwnd)` | Per-pixel alpha отрисовка [D] через UpdateLayeredWindow |
| `bringAppToFront()` | Вывод главного окна приложения на передний план |
| `bringDotaToFront(cap)` | Вывод окна Dota 2 на передний план |

---

### dota_picker.cpp
ML-пикер: CatBoost модели + рекомендации.

| Функция / Класс | Описание |
|------------------|----------|
| `FeatureVector` | 20 категориальных + 70 числовых признаков для CatBoost |
| `buildVector(v, lp, ...)` | Заполнение вектора признаков из LivePick + статистики |
| `DB` | RAII: sqlite3_open_v2 (readonly) |
| `Stmt` | RAII: sqlite3_prepare_v2 / finalize + хелперы |
| `loadHeroes(db)` | SQLite → map\<id, name\> |
| `loadProStats(db)` | SQLite → map\<(hero_id,pos), ProStats\> |
| `loadImmortalHeroStats(db, pos)` | SQLite → map\<hero_id, ImmortalHeroStats\> (games >= 1000) |
| `loadPlayerStats(db, account_id)` | SQLite → map\<hero_id, PlayerStats\> |
| `loadLatestLivePick(db, lp)` | Последняя строка livepicks → LivePick |
| `StageModels` | RAII: три CatBoost модели (early/mid/late) |
| `selectModel(models, lp)` | Выбор модели по количеству известных героев (0-4 / 5-7 / 8+) |
| `runBatch(model, batch)` | Батч-инференс CatBoost → sigmoid → P(win) |
| `renderToGui(state, lp, ...)` | Запись результатов в GuiPickerState (слоты + winProb + top-10) |
| `runPickerGui(modelPath, dbPath, running, guiState, portraitState)` | Главный цикл пикера: poll livepicks → renderToGui каждые 500мс |

---

### mainGUI.cpp
GUI: ImGui/D3D11 + оркестратор.

| Функция | Описание |
|---------|----------|
| `InitD3D(hwnd)` | Инициализация D3D11 device + swap chain |
| `CleanupD3D()` | Освобождение D3D11 ресурсов |
| `ApplyStyle()` | Тёмная тема ImGui с масштабированием 1.25x |
| `DrawHeader(fullW)` | Шапка: логотип [D], заголовок, карточка игрока / ввод Steam ID |
| `DrawStatusBar(fullW)` | Полоса статуса: Data (fetching/ready/error), Game (phase), match ID |
| `DrawDraftPanel(panelW)` | Левая панель: слоты Radiant/Dire + полоса winProb |
| `DrawPicksPanel(panelW)` | Правая панель: рекомендации top-10 / выбранный герой |
| `DrawHeroSlot(rowW, h, ...)` | Отрисовка одного слота героя (портрет, имя, позиция) |
| `DrawPortrait(dl, p, sz, ...)` | Отрисовка квадрата портрета с инициалами |
| `DrawBar(dl, p, w, h, frac, fill)` | Горизонтальный прогресс-бар |
| `WinColor(w)` | Цвет по win probability (green/amber/red) |
| `RenderFrame()` | Главный кадр: root window → Header → StatusBar → Draft + Picks |
| `orchestratorMain()` | Фоновый цикл: GSI → portrait → picker (управление потоками) |
| `startPhase1(accountId)` | Запуск DataFetcher + STRATZ имя в фоновом потоке |
| `fetchStratzName(accountId, token)` | GraphQL запрос имени игрока из STRATZ |
| `WinMain(hInst, ...)` | Точка входа: D3D11, ImGui, окно, оркестратор, message loop |

Палитра: `kBg`, `kCard`, `kText`, `kMuted`, `kGreen`, `kRed`, `kAmber`, `kBlue`.

---

### main_unified.cpp
Консольный оркестратор (альтернативная точка входа без GUI).

| Функция | Описание |
|---------|----------|
| `createLivePicksIfNotExists(db)` | Создание таблицы livepicks |
| `clearLivePicks(db)` | Очистка таблицы livepicks |
| `initLivePicksRow(db, matchId, accountId, ourSide, ourSlot)` | Инициализация строки для нового матча |
| `runDataFetcherPhase(accountId, stratzToken)` | Обёртка фазы 1 с замером времени |
| `main(argc, argv)` | Консольная точка входа: параметры → фаза 1 → GSI + Portrait + Picker цикл |

---

### build_unified.bat
Скрипт сборки: cl.exe (MSVC) + vcpkg + CatBoost.

Ключевые пути:
- vcpkg: `C:\vcpkg\installed\x64-windows-static`
- CatBoost: `C:\catboost`
- Результат: `build\Dota_Drafter.exe`

---

## Таблицы SQLite (playerandlivestats.db)

| Таблица | Ключ | Описание |
|---------|------|----------|
| `heroes` | id | Справочник героев (name, localized_name) |
| `playerheroes` | (account_id, hero_id) | Статистика героев игрока (все режимы) |
| `playerheroesranked` | (account_id, hero_id) | Статистика героев (только ranked) |
| `playerrecentmatches` | (match_id, account_id) | История матчей с пиками/позициями |
| `relevantplayerherobyposstats` | (account_id, heroId, position) | Агрегат героя+позиции из матчей |
| `playerherovsherobyposstats` | (account_id, hero_id, pos, vs_hero_id, vs_pos) | Статистика hero vs hero |
| `playerherowithherobyposstats` | (account_id, hero_id, pos, with_hero_id, with_pos) | Статистика hero with hero |
| `proherostats` | (hero_id, pos) | Про-статистика (из PostgreSQL) |
| `immortalherostats` | (hero_id, pos) | Immortal-статистика (агрегат recentimmortalmatches) |
| `livepicks` | single row | Текущий драфт: 10 hero-слотов + метаданные матча |
| `player_info` | account_id | Сохранённый Steam ID + имя |

---

## Внешние API

| API | Использование |
|-----|---------------|
| OpenDota `/api/heroes` | Справочник героев |
| OpenDota `/api/players/{id}/heroes` | Статистика героев игрока |
| OpenDota `/api/players/{id}/matches` | Список match_id (90 дней) |
| STRATZ GraphQL | Батч-запрос деталей матчей, имя игрока |
| Steam Web API `GetMatchDetails` | Поллинг (только логирование) |
| Dota 2 GSI (порт 3000) | Состояние игры в реальном времени |

---

## ML-модели (CatBoost)

| Модель | Фаза | Известных героев |
|--------|------|-----------------|
| `draft_helper_v3_early.cbm` | Начало драфта | 0-4 |
| `draft_helper_v3_mid.cbm` | Середина | 5-7 |
| `draft_helper_v3_late.cbm` | Конец | 8-10 |

Вход: 20 категориальных (имена героев + позиции) + 70 числовых (winrate, games, bans).
Выход: logit → sigmoid → P(radiant_win).
