# Dota Drafter — Контекст проекта

## Архитектура

Приложение — Dota 2 Draft Assistant с ImGui/D3D11 GUI.
Потоковая модель: GUI-поток, оркестратор, GSI-сервер, DataFetcher, Portrait capture, Picker.

Три фазы:
1. **DataFetcher** — загрузка данных игрока (OpenDota, STRATZ, PostgreSQL → SQLite)
2. **GSI-сервер** — приём состояния игры от Dota 2 через Game State Integration (порт 62326)
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
GSI HTTP-сервер.

| Функция | Описание |
|---------|----------|
| `runGsiServer(gameInfo)` | Запуск HTTP-сервера на порту 62326, приём GSI-данных |
| `parsePhaseStr(s)` | Строка GSI game_state → GamePhase |
| `handle_request(raw)` | Обработка HTTP: POST / → парсинг GSI, GET /phase → JSON статус |

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
| `paintLayeredButton(hwnd)` | Per-pixel alpha отрисовка [D] серым цветом (как шестерёнка HUD) через UpdateLayeredWindow |
| `selectOverlayPos(w, h)` | Выбор позиции кнопки по аспекту: 4:3 / 16:10 / 16:9 / 21:9 |
| `bringAppToFront()` | Вывод главного окна приложения на передний план |
| `bringDotaToFront(cap)` | Вывод окна Dota 2 на передний план |

---

### dota_picker.cpp
ML-пикер: CatBoost инференс + рекомендации.

| Функция / Класс | Описание |
|------------------|----------|
| `FeatureVector` | 10 категориальных + 72 числовых признака для CatBoost |
| `buildVector(v, lp, ...)` | Заполнение вектора признаков из LivePick + matchup-данных + статистики игрока |
| `DB` | RAII: sqlite3_open_v2 (readonly) |
| `Stmt` | RAII: sqlite3_prepare_v2 / finalize + хелперы |
| `loadHeroes(db)` | SQLite → map\<id, name\> |
| `loadMatchupData(db)` | SQLite → MatchupData (global_wr, vs_wr, with_wr, modal_pos, hero_pos_wr, pick_rates) |
| `loadImmortalHeroStats(db, pos)` | SQLite → map\<hero_id, ImmortalHeroStats\> (games >= 1000) |
| `loadPlayerStats(db, account_id)` | SQLite → map\<hero_id, PlayerStats\> |
| `loadLatestLivePick(db, lp)` | Последняя строка livepicks → LivePick |
| `runBatch(model, batch)` | Батч-инференс CatBoost → sigmoid → P(radiant_win) |
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
| `DrawPortrait(dl, p, sz, ...)` | Отрисовка квадрата портрета: PNG-текстура из assets/ (если есть) или инициалы |
| `loadHeroPortraits()` | Загрузка PNG из assets/ → кэш `g_heroPortraits` (localized_name → D3D11 текстура) |
| `DrawBar(dl, p, w, h, frac, fill)` | Горизонтальный прогресс-бар |
| `WinColor(w)` | Цвет по win probability (green/amber/red) |
| `RenderFrame()` | Главный кадр: root window → Header → StatusBar → Draft + Picks |
| `orchestratorMain()` | Фоновый цикл: GSI → portrait → picker (управление потоками) |
| `startPhase1(accountId)` | Запуск DataFetcher + STRATZ имя в фоновом потоке |
| `fetchOpenDotaProfile(accountId)` | OpenDota /api/players/{id} → имя + аватар (avatarmedium) |
| `createTextureFromImageData(data, size)` | JPEG/PNG байты → D3D11 текстура через GDI+ |
| `WinMain(hInst, ...)` | Точка входа: D3D11, ImGui, окно, оркестратор, message loop |

Палитра: `kBg`, `kCard`, `kText`, `kMuted`, `kGreen`, `kRed`, `kAmber`, `kBlue`.

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
| STRATZ GraphQL | Батч-запрос деталей матчей |
| OpenDota `/api/players/{id}` | Профиль игрока (personaname, avatarmedium) |
| Dota 2 GSI (порт 62326) | Состояние игры в реальном времени |

---

## ML-модель (CatBoost)

Одна модель: `draft_helper_abstract.cbm`.

Вход: **10 категориальных** (имена героев) + **72 числовых** признака.
Выход: logit → sigmoid → P(radiant_win).

### Вектор признаков (buildVector)

| Индексы | Группа | Размер | Описание |
|---------|--------|--------|----------|
| cat 0-9 | hero_name | 10 cat | Имена героев (localized_name): r1..r5, d1..d5. `"unknown"` для пустых слотов |
| flt 0-9 | global_wr | 10 | Глобальный винрейт героя (из таблицы `global_wr`). По умолчанию 0.5 |
| flt 10-19 | vs_adv | 10 | Среднее преимущество героя против вражеской команды: avg(vs_wr − global_wr) |
| flt 20-29 | with_adv | 10 | Средняя синергия героя с союзниками: avg(with_wr − global_wr) |
| flt 30 | mastery_wr | 1 | Винрейт нашего игрока на герое-кандидате (Bayesian smoothed, prior=30) |
| flt 31 | mastery_games | 1 | log1p(количество игр нашего игрока на герое-кандидате) |
| flt 32-41 | hero_pos_wr | 10 | Винрейт героя на конкретной позиции (из `hero_pos_wr`). По умолчанию 0.5 |
| flt 42-51 | best_vs | 10 | Лучший матчап героя: max(vs_wr − global_wr) среди врагов |
| flt 52-61 | worst_vs | 10 | Худший матчап героя: min(vs_wr − global_wr) среди врагов |
| flt 62-71 | pick_rate | 10 | Pick rate героя (из таблицы `pick_rates`). По умолчанию 0 |

**Итого**: 10 categorical + 72 float = 82 признака.

### Источники данных

| Таблица (matchup DB) | → Признак |
|-----------------------|-----------|
| `global_wr` (hero_id, wr) | global_wr, базовая линия для vs_adv/with_adv |
| `vs_wr` (hero_id, opp_hero_id, wr) | vs_adv, best_vs, worst_vs |
| `with_wr` (hero_id, ally_hero_id, wr) | with_adv |
| `modal_pos` (hero_id, pos) | Позиция врагов при отсутствии OCR |
| `hero_pos_wr` (hero_id, pos, wr) | hero_pos_wr |
| `pick_rates` (hero_id, rate) | pick_rate |
| `playerheroes` (player DB) | mastery_wr, mastery_games |
| `immortalherostats` (player DB) | Фильтрация пула кандидатов (games ≥ 1000 на позиции) |
