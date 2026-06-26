# Dota 2 Draft Assistant

Автоматический помощник драфта с ImGui/D3D11 интерфейсом, распознаванием портретов и ML-рекомендациями (CatBoost).

---

## Описание

Приложение анализирует текущий матч Dota 2 в реальном времени и выдаёт рекомендации по выбору героев.

**Источники данных:**
- Статистика конкретного игрока (OpenDota / STRATZ)
- Matchup-данные (global WR, vs/with пайрвайзные, hero×position WR)
- Immortal-статистика для фильтрации пула кандидатов

**Распознавание героев и позиций** — захват HUD и сравнение по Pearson-корреляции 8×8 матриц (без OCR).

**ML-предсказания** — CatBoost модель предсказывает P(radiant_win) для каждого возможного пика (10 cat + 42 float признаков).

**GUI** — ImGui/D3D11 окно с двумя панелями: состояние драфта (Radiant/Dire) и рекомендации top-10.

---

## Возможности

- **Графический интерфейс** — ImGui/D3D11 окно
- **Ввод Friend ID прямо в интерфейсе** — без аргументов командной строки
- **Карточка игрока** — аватар и имя из OpenDota профиля
- **Панель драфта** — 10 слотов с портретами и кликабельными позициями
- **Панель рекомендаций** — топ-10 героев с winrate и статистикой
- **Win probability bar** — цветовая индикация (зелёный ≥55%, жёлтый 50–55%, красный <50%)
- **Overlay-кнопка [D]** — прозрачная кнопка поверх Dota 2, переключает фокус
- **Распознавание героев и позиций** — захват портретов каждые 500мс
- **Ручной выбор позиций** — кликабельный popup с автоматическим свапом
- **Драфт без Friend ID** — портреты распознаются и отображаются без ввода ID
- **Поддержка прокси** — SOCKS5 / HTTP для доступа к OpenDota
- **Многопоточность** — GUI, оркестратор, GSI-сервер, портреты, пикер работают параллельно
- **DPI-масштабирование** — 125%, 150%, 200% автоматически
- **GSI-таймаут** — автоматический сброс статуса при закрытии Dota 2

---

## Структура файлов

```
Dota_Drafter.exe                    Исполняемый файл (ImGui GUI)
catboostmodel.dll                   CatBoost runtime

draft_helper_abstract.cbm           ML-модель (одна, все фазы драфта)
draft_helper_abstract_data.db       Matchup-данные модели (SQLite)

hero_hashes.h                       База хешей портретов героев
pos_hashes.h                        База хешей индикаторов позиций
hero_portraits_data.h               Встроенные PNG-портреты героев
playerandlivestats.db               Данные игрока (создаётся автоматически)

gamestate_integration_dota2.cfg     Конфигурация GSI (скопировать один раз):
    → ..\dota 2 beta\game\dota\cfg\gamestate_integration\
```

---

## Зависимости

| Компонент | Описание |
|-----------|----------|
| `catboostmodel.dll` | CatBoost C API runtime |
| vcpkg | libcurl, sqlite3, openssl, lz4, zlib, nlohmann-json (static) |
| ImGui | Встроена как `imgui.lib` (D3D11 backend) |
| Windows SDK | d3d11, dxgi, d3dcompiler, dwmapi, gdiplus |

---

## Установка

1. **CatBoost** — положить `catboostmodel.dll` и `catboostmodel.lib` в `C:\catboost`

2. **GSI** — скопировать `gamestate_integration_dota2.cfg` в папку Dota 2:
   ```
   C:\Program Files (x86)\Steam\steamapps\common\dota 2 beta\
     game\dota\cfg\gamestate_integration\
   ```

3. **Модель** — положить рядом с exe:
   ```
   draft_helper_abstract.cbm
   draft_helper_abstract_data.db
   ```

4. **Переменные окружения** (опционально):
   ```
   set STRATZ_API_KEY=eyJ...
   set DOTA_PROXY=socks5://127.0.0.1:7890
   ```

---

## Запуск

```
build\Dota_Drafter.exe
```

Приложение работает сразу — драфт отображается без ввода ID.
Для рекомендаций ввести Friend ID в интерфейсе (верхний правый угол).

> **Friend ID** — числовой ID из Dota 2 профиля.
> Пример: `1261660135`

---

## Фазы работы

### Фаза 1a — Инициализация (при старте, без ID)

Загружает справочник героев из OpenDota, создаёт таблицы SQLite.
Необходима для распознавания героев portrait capture.

### Фаза 1b — Данные игрока (при вводе Friend ID)

Загружает из OpenDota и STRATZ:
- Статистика игрока по героям (все режимы + ranked)
- История матчей за 90 дней (STRATZ GraphQL батчами)

Статус отображается в GUI: спиннер → зелёная точка "Ready" / красная "Error".

### Фаза 2 — Ожидание матча

GSI-сервер слушает порт. Ждёт подключения Dota 2 и начала матча.
Статус игры: Waiting / Draft / In Game / Post Game.
Таймаут: если GSI не присылает данные 5 секунд → сброс на IDLE.

### Фаза 3 — HERO_SELECTION

**Portrait Capture** (работает без Friend ID):
- Захватывает портреты и позиции HUD каждые 500мс
- Распознаёт героев (Pearson ≥ 0.80) и позиции
- Записывает в `livepicks` и `SharedPortraitState`
- Результаты отображаются в GUI через portrait→GUI sync

**ML Picker** (требует Friend ID):
- Читает `livepicks` каждые 500мс
- Если наш слот пустой — показывает топ-10 рекомендаций
- Если наш герой выбран — показывает P(win)

Оба потока продолжают работать 5 секунд после окончания HERO_SELECTION.

---

## Интерфейс

### Шапка
- Логотип **[D]** + заголовок
- Карточка игрока: аватар, имя, Friend ID (кликабельно для редактирования)

### StatusBar
- **Data** — статус загрузки данных игрока
- **Game** — фаза матча (Waiting / Draft / In Game / Post Game)
- **Match ID** — ID текущего матча

### Панель драфта (левая, 57.5%)
- 5 слотов Radiant + 5 слотов Dire
- Портрет героя, имя, кликабельная позиция (для своей команды)
- Позиции: клик → popup 1-5, автоматический свап занятых позиций
- Win probability bar с цветовой индикацией

### Панель рекомендаций (правая, 42.5%)
- Без Friend ID: "Enter Friend ID for recommendations"
- С Friend ID: топ-10 героев с winrate и статистикой
- После выбора героя: наш герой + P(win)

---

## Overlay-кнопка [D]

Прозрачная кнопка **[D]** поверх Dota 2 HUD (WS_EX_LAYERED с per-pixel alpha).
- Позиция подстраивается под разрешение и аспект (4:3 / 16:10 / 16:9 / 21:9)
- Клик переключает фокус между Dota 2 и приложением
- Видна только когда Dota 2 или приложение на переднем плане

---

## Потоковая модель

| Поток | Функция | Описание |
|-------|---------|----------|
| GUI | `WinMain` | ImGui/D3D11 рендер-цикл |
| Оркестратор | `orchestratorMain` | Управление потоками, portrait→GUI sync, one-shot inference |
| GSI-сервер | `runGsiServer` | HTTP-сервер для GSI-данных Dota 2 |
| Portrait capture | `runPortraitCapture` | Захват и распознавание героев + позиций |
| Picker | `runPickerGui` | CatBoost инференс + рекомендации |

---

## ML-модель (CatBoost)

Одна модель: `draft_helper_abstract.cbm`
Данные: `draft_helper_abstract_data.db`

**Вход:** 10 категориальных (имена героев) + 42 числовых (matchup + mastery + hero_pos_wr)

**Выход:** logit → sigmoid → P(radiant_win)

| Признаки | Размер | Описание |
|----------|--------|----------|
| hero_name | 10 cat | Имена героев r1..r5, d1..d5 |
| matchup | 30 float | global_wr + avg_vs + avg_with (×10 слотов) |
| mastery | 2 float | WR и log1p(games) нашего игрока на кандидате |
| hero_pos_wr | 10 float | WR героя на позиции (своя = из livepicks, враг = modal) |

---

## Базы данных

### playerandlivestats.db — данные игрока

| Таблица | Описание |
|---------|----------|
| `heroes` | Справочник героев (фаза 1a) |
| `playerheroes` | Статистика героев игрока (все режимы) |
| `playerheroesranked` | Статистика героев (ranked) |
| `playerrecentmatches` | История матчей |
| `relevantplayerherobyposstats` | Агрегат героя + позиции |
| `playerherovsherobyposstats` | Hero vs hero |
| `playerherowithherobyposstats` | Hero with hero |
| `livepicks` | Текущий драфт: 10 hero + 10 pos слотов |
| `player_info` | Сохранённый Friend ID + имя |

### draft_helper_abstract_data.db — данные модели

| Таблица | Описание |
|---------|----------|
| `global_wr` | Smoothed winrate героя |
| `vs_wr` | Пайрвайзный winrate hero vs hero |
| `with_wr` | Пайрвайзный winrate hero with hero |
| `modal_pos` | Модальная позиция героя |
| `hero_pos_wr` | Winrate героя на позиции |
| `immortalherostats` | Immortal-статистика для фильтрации пула |

---

## Поддерживаемые разрешения

| Аспект | Разрешения |
|--------|-----------|
| 16:9 | 1280×720, 1920×1080, 2560×1440, 3840×2160 |
| 16:10 | 1920×1200, 2560×1600 |
| 21:9 | 2560×1080, 3440×1440 |
| 4:3 | 1024×768, 1280×960 |

DPI-масштабирование (125%, 150%, 200%) поддерживается.
При смене разрешения во время захвата — автоматический пересчёт регионов.

---

## Сборка из исходников

**Требования:**
- Visual Studio 2019/2022 или BuildTools (MSVC x64)
- vcpkg: `libcurl sqlite3 openssl lz4 zlib nlohmann-json` (x64-windows-static)
- CatBoost C API: `catboostmodel.dll` + `catboostmodel.lib` в `C:\catboost`
- ImGui: собранная `imgui.lib` (D3D11 + Win32 backend)

**Сборка:**
```bat
build_unified.bat           :: release
build_unified.bat debug     :: debug (/Od /Zi)
```

**Результат:** `build\Dota_Drafter.exe` + `catboostmodel.dll`

**Исходные файлы:**

| Файл | Описание |
|------|----------|
| `mainGUI.cpp` | ImGui/D3D11 GUI + оркестратор |
| `shared_types.h` | Общие типы (GameInfo, SharedPortraitState, GuiPickerState) |
| `common.cpp / .h` | Логирование, HTTP (curl), SQLite RAII |
| `datafetcher.cpp` | Фаза 1a (init) + 1b (player data) |
| `livestatsfetcher.cpp` | GSI HTTP-сервер |
| `portrait_runner.cpp / .h` | Захват портретов + позиций + overlay [D] |
| `dota_picker.cpp` | ML-пикер CatBoost |
| `dota2_capture.cpp / .h` | Захват окна Dota 2 (PrintWindow + GDI) |
| `dhash.h` | Pearson-корреляция 8×8 для распознавания героев и позиций |
| `hero_hashes.h` | База хешей портретов героев |
| `pos_hashes.h` | База хешей индикаторов позиций |
| `hero_portraits_data.h` | Встроенные PNG-портреты для GUI |
| `playerdatafetcher.cpp / .h` | OpenDota / STRATZ запросы |

---

## Логи

```
logs\console.log        Весь вывод программы
logs\curl_debug.txt     HTTP заголовки и трафик (для диагностики)
```
