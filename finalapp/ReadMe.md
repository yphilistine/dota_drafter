# Dota 2 Draft Assistant

Автоматический помощник драфта с ImGui/D3D11 интерфейсом, распознаванием портретов и ML-рекомендациями (CatBoost).

---

## Описание

Приложение анализирует текущий матч Dota 2 в реальном времени и выдаёт рекомендации по выбору героев.

**Источники данных:**
- Статистика конкретного игрока (OpenDota / STRATZ)
- Про-статистика героев (OpenDota ProMatches, PostgreSQL)
- Immortal-статистика (STRATZ, последние 8 дней)

**Распознавание героев** — захват HUD и сравнение портретов по Pearson-корреляции 8×8 матриц (без OCR).

**ML-предсказания** — три CatBoost модели (начало / середина / конец драфта) предсказывают P(radiant_win) для каждого возможного пика.

**GUI** — ImGui/D3D11 окно с двумя панелями: состояние драфта (Radiant/Dire) и рекомендации top-10.

---

## Возможности

- **Графический интерфейс** — полноценное ImGui/D3D11 окно вместо консоли
- **Ввод Steam ID прямо в интерфейсе** — без аргументов командной строки
- **Карточка игрока** — аватар и имя из OpenDota профиля
- **Панель драфта** — 10 слотов (5 Radiant + 5 Dire) с портретами и позициями
- **Панель рекомендаций** — топ-10 героев с winrate, статистикой игрока и immortal %
- **Win probability bar** — цветовая индикация (зелёный ≥55%, жёлтый 50–55%, красный <50%)
- **Overlay-кнопка [D]** — прозрачная кнопка поверх Dota 2, переключает фокус между игрой и приложением
- **Автоматическое распознавание героев** — захват портретов каждые 500мс
- **Три ML-модели** — early (0–4 героя), mid (5–7), late (8–10)
- **Поддержка прокси** — SOCKS5 / HTTP для доступа к OpenDota
- **Многопоточность** — GUI, оркестратор, GSI-сервер, портреты, пикер работают параллельно
- **DPI-масштабирование** — 125%, 150%, 200% поддерживаются автоматически

---

## Структура файлов

```
Dota_Drafter.exe              Главный исполняемый файл (ImGui GUI)
catboostmodel.dll             CatBoost runtime (копируется при сборке)

draft_helper_v3_early.cbm     Модель: начало драфта (0–4 героя)
draft_helper_v3_mid.cbm       Модель: середина (5–7 героев)
draft_helper_v3_late.cbm      Модель: конец (8–10 героев)

hero_hashes.h                 База хешей портретов героев
hero_portraits_data.h         Встроенные PNG-портреты героев
playerandlivestats.db         SQLite база данных (создаётся автоматически)

gamestate_integration_dota2.cfg   Конфигурация GSI (скопировать один раз):
    → C:\Program Files (x86)\Steam\steamapps\common\dota 2 beta\
      game\dota\cfg\gamestate_integration\
```

---

## Зависимости

| Компонент | Описание |
|-----------|----------|
| `catboostmodel.dll` | CatBoost C API runtime |
| vcpkg | libcurl, sqlite3, libpq, openssl, lz4, zlib, nlohmann-json (static) |
| ImGui | Встроена как `imgui.lib` (D3D11 backend) |
| Windows SDK | d3d11, dxgi, d3dcompiler, dwmapi, gdiplus |
| PostgreSQL | Опционально — для pro/immortal статистики |

---

## Установка

1. **CatBoost** — положить `catboostmodel.dll` и `catboostmodel.lib` в `C:\catboost`

2. **GSI** — скопировать `gamestate_integration_dota2.cfg` в папку Dota 2:
   ```
   C:\Program Files (x86)\Steam\steamapps\common\dota 2 beta\
     game\dota\cfg\gamestate_integration\
   ```

3. **Модели** — положить `.cbm` файлы рядом с exe:
   ```
   draft_helper_v3_early.cbm
   draft_helper_v3_mid.cbm
   draft_helper_v3_late.cbm
   ```

4. **Переменные окружения** (опционально):
   ```
   set PG_CONN_STR=postgresql://user:pass@localhost:5432/dota2
   set STRATZ_API_KEY=eyJ...
   set DOTA_PROXY=socks5://127.0.0.1:7890
   ```

---

## Запуск

```
build\Dota_Drafter.exe
```

При первом запуске ввести 32-битный Steam ID прямо в интерфейсе (верхний правый угол → карточка игрока).

> **Steam ID** — 32-битный (не SteamID64).
> Найти: https://www.steamidfinder.com → "Steam ID 3" без `[U:1:]`
> Пример: `1261660135`

---

## Фазы работы

### Фаза 1 — DataFetcher (~30–90 сек)

Загружает данные из OpenDota и STRATZ:
- Справочник героев
- Статистика игрока по героям (все режимы + ranked)
- История матчей за 90 дней (STRATZ GraphQL батчами)
- Pro-статистика и immortal-статистика (если задан `PG_CONN_STR`)

Статус отображается в GUI: спиннер → зелёная точка "Ready" / красная "Error".

### Фаза 2 — Ожидание матча

GSI-сервер слушает порт 3000. Ждёт подключения Dota 2 и начала матча.
Статус игры отображается в StatusBar: Waiting / Draft / In Game / Post Game.

### Фаза 3 — HERO_SELECTION

**Portrait Capture:**
- Захватывает портреты HUD каждые 500мс
- Распознаёт героев (Pearson-корреляция ≥ 0.80 = уверенное совпадение)
- Записывает распознанных героев в таблицу `livepicks`

**ML Picker:**
- Читает `livepicks` каждые 500мс
- Если наш слот пустой — показывает топ-10 рекомендаций с winrate
- Если наш герой выбран — показывает P(radiant_win)
- Выбирает модель по количеству распознанных героев (early/mid/late)

Портреты и рекомендации продолжают обновляться 5 секунд после окончания HERO_SELECTION.

---

## Интерфейс

### Шапка
- Логотип **[D]** + заголовок
- Карточка игрока: аватар, имя, Steam ID (кликабельно для редактирования)
- Статус загрузки данных (спиннер / Ready / Error)

### StatusBar
- **Data** — статус загрузки данных игрока
- **Game** — фаза матча (Waiting / Draft / In Game / Post Game)
- **Match ID** — ID текущего матча

### Панель драфта (левая, 57.5%)
- 5 слотов Radiant (зелёный) + 5 слотов Dire (красный)
- Каждый слот: портрет героя, имя, позиция (для своей команды), маркер "Your Pick"
- Пустые слоты отображаются пунктирной рамкой
- Win probability bar с процентом и цветовой индикацией

### Панель рекомендаций (правая, 42.5%)
- Топ-10 рекомендованных героев (или выбранный герой)
- Для каждого героя: портрет, имя, статистика игрока (`you Xg Y%`), immortal winrate (`imm Z%`), общий winrate
- Цветовые полосы winrate

---

## Overlay-кнопка [D]

Прозрачная кнопка **[D]** поверх Dota 2 HUD (WS_EX_LAYERED с per-pixel alpha).
- Позиция автоматически подстраивается под разрешение и аспект (4:3 / 16:10 / 16:9 / 21:9)
- Клик переключает фокус между Dota 2 и окном приложения
- Видна только когда Dota 2 или приложение на переднем плане
- Обновляет позицию каждые 500мс при перемещении окна игры

---

## Потоковая модель

| Поток | Функция | Описание |
|-------|---------|----------|
| GUI | `WinMain` | ImGui/D3D11 рендер-цикл |
| Оркестратор | `orchestratorMain` | Управление фазами 1–3, запуск потоков |
| GSI-сервер | `runGsiServer` | HTTP-сервер на порту 3000 |
| Portrait capture | `runPortraitCapture` | Захват и распознавание портретов |
| Picker | `runPickerGui` | CatBoost инференс + рекомендации |

---

## ML-модели (CatBoost)

| Модель | Фаза | Известных героев |
|--------|------|-----------------|
| `draft_helper_v3_early.cbm` | Начало драфта | 0–4 |
| `draft_helper_v3_mid.cbm` | Середина | 5–7 |
| `draft_helper_v3_late.cbm` | Конец | 8–10 |

**Вход:** 20 категориальных (имена героев + позиции) + 70 числовых (winrate, games, bans)

**Выход:** logit → sigmoid → P(radiant_win)

---

## Поддерживаемые разрешения

HUD-раскладки откалиброваны для:

| Аспект | Разрешения |
|--------|-----------|
| 16:9 | 1280×720, 1920×1080, 2560×1440, 3840×2160 |
| 16:10 | 1920×1200, 2560×1600 |
| 21:9 | 2560×1080, 3440×1440 |
| 4:3 | 1024×768, 1280×960 |

DPI-масштабирование (125%, 150%, 200%) поддерживается автоматически.

---

## Таблицы SQLite

| Таблица | Описание |
|---------|----------|
| `heroes` | Справочник героев (id, name, localized_name) |
| `playerheroes` | Статистика героев игрока (все режимы) |
| `playerheroesranked` | Статистика героев (только ranked) |
| `playerrecentmatches` | История матчей с пиками и позициями |
| `relevantplayerherobyposstats` | Агрегат героя + позиции из матчей |
| `playerherovsherobyposstats` | Статистика hero vs hero |
| `playerherowithherobyposstats` | Статистика hero with hero |
| `proherostats` | Про-статистика (из PostgreSQL) |
| `immortalherostats` | Immortal-статистика (STRATZ) |
| `livepicks` | Текущий драфт: 10 hero-слотов + метаданные |
| `player_info` | Сохранённый Steam ID + имя |

---

## Сборка из исходников

**Требования:**
- Visual Studio 2019/2022 или BuildTools (MSVC x64)
- vcpkg: `libcurl sqlite3 libpq openssl lz4 zlib nlohmann-json` (x64-windows-static)
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
| `mainGUI.cpp` | ImGui/D3D11 GUI + оркестратор фаз |
| `shared_types.h` | Общие типы (GameInfo, SharedPortraitState, GuiPickerState) |
| `common.cpp / .h` | Логирование, HTTP (curl), SQLite RAII |
| `datafetcher.cpp` | Оркестратор фазы 1 (runDataFetcher) |
| `livestatsfetcher.cpp` | GSI HTTP-сервер (runGsiServer) |
| `portrait_runner.cpp / .h` | Захват портретов + overlay [D] (runPortraitCapture) |
| `dota_picker.cpp` | ML-пикер CatBoost (runPickerGui) |
| `dota2_capture.cpp / .h` | Захват окна Dota 2 (PrintWindow + GDI) |
| `dhash.h` | Pearson-корреляция 8×8 для распознавания |
| `hero_hashes.h` | База хешей портретов |
| `hero_portraits_data.h` | Встроенные PNG-портреты для GUI |
| `playerdatafetcher.cpp / .h` | OpenDota / STRATZ запросы |
| `clouddatafetcher.cpp / .h` | PostgreSQL → SQLite синхронизация |

---

## Генерация базы хешей портретов

1. Запустить Dota 2, начать тренировочную игру
2. Запустить `build_hero_db.exe` — захватит все портреты из HUD
3. Переименовать PNG по `localized_name` героев
4. Запустить `build_hero_db.exe --build` — создаст `hero_hashes.h`
5. Пересобрать проект (`build_unified.bat`)

В `hero_hashes.h` добавить запись `"NULL"` для пустых слотов — при score > 0.7 для NULL слот не записывается в `livepicks`.

---

## Предупреждение при запуске

```
"There are invalid params and some of them will be ignored.
 Parameter {feature_weights...} is ignored, because it cannot be parsed."
```

Это **не ошибка**. Модели обучены с `feature_weights`, но версия `catboostmodel.dll` не поддерживает этот параметр. На качество предсказаний не влияет. Появляется 3 раза (по одному для каждой модели).

---

## Логи

```
logs\console.log        Весь вывод программы
logs\curl_debug.txt     HTTP заголовки и трафик (для диагностики)
```
