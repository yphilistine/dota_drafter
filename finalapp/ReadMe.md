# Dota 2 Draft Assistant

Автоматический помощник драфта с ImGui/D3D11 интерфейсом, распознаванием портретов и ML-рекомендациями (CatBoost).

---

## Описание

Приложение анализирует текущий матч Dota 2 в реальном времени и выдаёт рекомендации по выбору героев.

**Источники данных:**
- Статистика конкретного игрока (OpenDota / STRATZ)
- Matchup-данные (global WR, vs/with пайрвайзные, hero×position WR)
- Живая Immortal-статистика (STRATZ `heroStats`, последняя неделя) для фильтрации пула кандидатов и панели Meta Heroes

**Распознавание героев** — захват HUD и сравнение по Pearson-корреляции 8×8 матриц. **Распознавание позиций** — Windows OCR (текст плашки "Safe Lane" / "Лёгкая линия" и т.п.).

**ML-предсказания** — CatBoost модель предсказывает P(radiant_win) для каждого возможного пика (10 cat + 42 float признаков).

**GUI** — ImGui/D3D11 окно с двумя панелями: состояние драфта (Radiant/Dire) и рекомендации top-10.

**Автообновление** — при запуске проверяет manifest.json на GitHub, скачивает и устанавливает обновления приложения и данных.

---

## Возможности

- **Графический интерфейс** — ImGui/D3D11 окно
- **Автообновление** — проверка обновлений при старте, скачивание app/data, silent install + перезапуск
- **Ввод Friend ID прямо в интерфейсе** — без аргументов командной строки
- **Карточка игрока** — аватар и имя из OpenDota профиля
- **Панель драфта** — 10 слотов с портретами и кликабельными позициями
- **Панель рекомендаций** — топ-10 героев с winrate и статистикой
- **Панель Meta Heroes** — топ-10 героев по популярности (STRATZ, сумма матчей, без ML) с винрейтом как вторичной метрикой, отдельная колонка рядом с рекомендациями, то же оформление; позиция всегда авто (следует за позицией игрока, иначе агрегат по всем позициям). Когда герой уже выбран — вместо топ-10 показывает его собственную мету на этой позиции (games/winRate), аналогично панели рекомендаций
- **Win probability bar** — цветовая индикация (зелёный >=55%, жёлтый 50-55%, красный <50%)
- **Overlay-кнопка [D]** — прозрачная кнопка поверх Dota 2, переключает фокус
- **Распознавание героев и позиций** — захват портретов каждые 500мс
- **Ручной выбор позиций** — кликабельный popup с автоматическим свапом
- **Драфт без Friend ID** — портреты распознаются и отображаются без ввода ID
- **Кнопка Refresh** — перезагрузка данных игрока без перезапуска
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
draft_helper_abstract_data.db       Matchup-данные модели (SQLite, содержит meta-таблицу)

hero_hashes.dat                     База хешей портретов героев (бинарный)
assets/*.png                        PNG-портреты героев для GUI
playerandlivestats.db               Данные игрока (создаётся автоматически)

gamestate_integration_dota2.cfg     Конфигурация GSI (устанавливается инсталлятором)
```

---

## Система обновлений

### Как работает

При каждом запуске приложение проверяет `manifest.json` на GitHub (main-ветка, raw.githubusercontent.com):
1. Если нет подключения — показывает окно "Failed to check for updates" с кнопкой "Try again"
2. Если есть обновление приложения — скачивает инсталлятор, проверяет SHA-256, запускает silent install
3. Если есть обновление данных — точечно скачивает **только** те файлы из `manifest.data.files`, которые реально отличаются (отсутствуют локально или не совпадают по SHA-256); уже актуальные файлы не трогаются и не перекачиваются
4. Гейт совместимости — если schema данных не совпадает с поддерживаемой приложением, показывает баннер

`manifest.data.files` — не только `.cbm`/`_data.db`, но и `hero_hashes.dat` + каждый `assets/<hero>.png` (~130 записей). Для `.cbm`/`_data.db` `url` указывает на версионированный GitHub Release (`pack_data.bat`); для `hero_hashes.dat`/ассетов — напрямую на `raw.githubusercontent.com/.../main/finalapp/...`, т.е. приложение на каждом старте сверяет свои локальные копии с тем, что прямо сейчас лежит в main-ветке репозитория, без отдельных релизов/версий (см. `scripts/update_assets.ps1`). Несовпадение или отсутствие файла → перекачка только этого файла; PNG, отсутствующий в манифесте (герой переименован/убран), удаляется (`cleanupObsoleteAssets` в `updater.cpp`).

Прогресс-бар при обновлении данных считается по всей пачке файлов, которые реально нужно скачать, а не по одному файлу за раз: каждая запись в манифесте содержит `size` (байты), из них складывается общий объём докачки, и проценты идут от него — бар не сбрасывается на 0% при переходе к следующему файлу.

Отдельно: полный переустановщик (`installer/dota_draft_setup.iss`) тоже ставит `assets/*.png` и `hero_hashes.dat` напрямую и перед копированием полностью удаляет папку `assets` (секция `[InstallDelete]`) — то же самое "не копить устаревшие файлы", но на случай, когда обновление идёт через новый инсталлятор, а не через фоновый data-канал.

### Версионирование

| Версия | Формат | Когда меняется |
|--------|--------|----------------|
| `app_version` | `MAJOR.MINOR.PATCH` | Новый билд приложения |
| `data_version` | `YYYY.MM.DD` | Ретрейн модели или обновление данных |
| `schema` | Целое число | Изменение формата данных (таблицы, buildVector) |

### Правила релиза

- **schema++** + `app.mandatory=true`: изменена раскладка фич/таблиц или buildVector
- Только **data_version++**: чистый ретрейн на тех же фичах
- Только **app_version++**: изменения в UI/логике без изменения формата данных

### Релиз через VS Code

- **Release App**: Tasks → "Release App" → ввести версию (напр. `1.0.1`)
- **Release Data**: Tasks → "Release Data" → ввести версию данных и схему (только `.cbm`/`_data.db`)
- **Update Assets**: Tasks → "Update Assets" — пересчитать sha256 и обновить манифест после изменений в `assets/` или `hero_hashes.dat` (без версии/релиза, `scripts/update_assets.ps1`)

---

## Зависимости

| Компонент | Описание |
|-----------|----------|
| `catboostmodel.dll` | CatBoost C API runtime |
| vcpkg | libcurl, sqlite3, openssl, lz4, zlib, nlohmann-json, libpq (static) |
| ImGui | Встроена как `imgui.lib` (D3D11 backend) |
| Windows SDK | d3d11, dxgi, d3dcompiler, dwmapi, gdiplus, bcrypt |
| Windows OCR (WinRT/cppwinrt) | `pos_ocr.h` — распознавание позиций (`Windows.Media.Ocr`) |

---

## Установка

Скачать `dota_drafter_setup.exe` из [GitHub Releases](https://github.com/yphilistine/dota_drafter/releases) и запустить. Инсталлятор:
- Устанавливает в `%LOCALAPPDATA%\Dota_Drafter` (без прав админа)
- Копирует GSI-конфигурацию в папку Dota 2
- Создаёт ярлыки

Переменные окружения (опционально):
```
set STRATZ_API_KEY=eyJ...
set DOTA_PROXY=socks5://127.0.0.1:7890
```

---

## Запуск

```
Dota_Drafter.exe
```

При первом запуске ввести Friend ID в интерфейсе (верхний правый угол).
Обновления проверяются автоматически.

> **Friend ID** — числовой ID из Dota 2 профиля. Пример: `1261660135`

---

## Фазы работы

### Фаза 0 — Обновление (при старте)

Проверяет manifest.json на GitHub, скачивает обновления app/data если доступны.
Без интернета — приложение не запускается (кнопка "Try again").

### Фаза 1a — Инициализация (без ID)

Загружает справочник героев из OpenDota, создаёт таблицы SQLite, тянет живую мета-стату героев из STRATZ (`heroStats`, DIVINE_IMMORTAL, последняя завершённая неделя) → таблица `stats`.

### Фаза 1b — Данные игрока (при вводе Friend ID)

Загружает из OpenDota и STRATZ. Кнопка Refresh в статус-баре позволяет перезагрузить.

### Фаза 2 — Ожидание матча

GSI-сервер слушает порт 62326. Статус игры в статус-баре.

### Фаза 3 — HERO_SELECTION

Portrait capture + ML Picker работают параллельно. Schema gate проверяет совместимость данных.

---

## Потоковая модель

| Поток | Функция | Описание |
|-------|---------|----------|
| GUI | `WinMain` | ImGui/D3D11 рендер-цикл |
| Оркестратор | `orchestratorMain` | Управление потоками, portrait->GUI sync |
| GSI-сервер | `runGsiServer` | HTTP-сервер для GSI-данных Dota 2 |
| Portrait capture | `runPortraitCapture` | Захват и распознавание героев + позиций |
| Picker | `runPickerGui` | CatBoost инференс + рекомендации |

---

## ML-модель (CatBoost)

Одна модель: `draft_helper_abstract.cbm`
Данные: `draft_helper_abstract_data.db` (содержит meta-таблицу с schema_version и data_version)

**Вход:** 10 категориальных (имена героев) + 42 числовых (matchup + mastery + hero_pos_wr)

**Выход:** logit -> sigmoid -> P(radiant_win)

---

## Базы данных

### playerandlivestats.db — данные игрока

| Таблица | Описание |
|---------|----------|
| `heroes` | Справочник героев (фаза 1a) |
| `playerheroes` | Статистика героев игрока (все режимы) |
| `playerheroesranked` | Статистика героев (ranked) |
| `playerrecentmatches` | История матчей |
| `livepicks` | Текущий драфт: 10 hero + 10 pos слотов |
| `player_info` | Сохранённый Friend ID + имя |
| `stats` | Живая мета-стата героев (STRATZ `heroStats`, DIVINE_IMMORTAL, последняя неделя). Перезаписывается фазой 1a при каждом старте |

### draft_helper_abstract_data.db — данные модели

| Таблица | Описание |
|---------|----------|
| `meta` | Метаданные: schema_version, data_version |
| `global_wr` | Smoothed winrate героя |
| `vs_wr` | Пайрвайзный winrate hero vs hero |
| `with_wr` | Пайрвайзный winrate hero with hero |
| `modal_pos` | Модальная позиция героя |
| `hero_pos_wr` | Winrate героя на позиции |

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

**Исходные файлы:**

| Файл | Описание |
|------|----------|
| `mainGUI.cpp` | ImGui/D3D11 GUI + оркестратор + окно обновления |
| `shared_types.h` | Общие типы (GameInfo, SharedPortraitState, GuiPickerState) |
| `common.cpp / .h` | Логирование, HTTP (curl), SQLite RAII |
| `version.h` | Константы версии: kAppVersion, kSupportedSchema, kManifestUrl |
| `version_utils.cpp / .h` | Чтение meta (schema_version/data_version) из _data.db |
| `updater.cpp / .h` | Система обновлений: manifest, точечная докачка только несовпавших файлов, sha256, swap, rollback, агрегированный прогресс |
| `datafetcher.cpp` | Фаза 1a (init) + 1b (player data) |
| `clouddatafetcher.cpp / .h` | PostgreSQL → SQLite sync (proherostats/immortalherostats); dev-инструмент, не вызывается в рантайме приложения |
| `livestatsfetcher.cpp` | GSI HTTP-сервер |
| `portrait_runner.cpp / .h` | Захват портретов + позиций + overlay [D] |
| `dota_picker.cpp` | ML-пикер CatBoost + schema gate |
| `dota2_capture.cpp / .h` | Захват окна Dota 2 (PrintWindow + GDI) |
| `dhash.h` | Pearson-корреляция 8x8 для распознавания **героев** (база — `hero_hashes.dat`, грузится в рантайме) |
| `pos_ocr.h` | Распознавание **позиций** через Windows OCR (заменяет старый Pearson-подход) |
| `playerdatafetcher.cpp / .h` | OpenDota / STRATZ запросы |

---

## Производительность

### Затраты системы обновлений на старте

| Операция | Время | Когда |
|----------|-------|-------|
| Fetch manifest.json (~30KB, ~130 записей файлов) | 0.5-2с | Каждый запуск |
| SHA-256 проверка ~130 локальных файлов (cbm/db/hero_hashes.dat/assets) | <100мс | Каждый запуск (checkForUpdates хэширует все файлы манифеста, чтобы найти расхождения) |
| Скачивание app (~10MB) | 2-10с | Только при обновлении |
| Скачивание data | пропорционально объёму изменившихся файлов | Только при обновлении, и только для файлов, реально не совпавших с манифестом (`size` из манифеста → единый прогресс-бар на всю пачку) |
| File swap (MoveFileEx) | <5мс/файл | Только для реально скачанных файлов при обновлении data |
| Schema gate (2 SQL запроса) | <5мс | Один раз при старте picker |

**Итого**: +0.5-2с к запуску (online, без обновления). Без влияния на производительность во время игры.

### Затраты основного приложения

| Компонент | CPU | RAM | Диск |
|-----------|-----|-----|------|
| GUI (ImGui/D3D11, 60fps) | ~2-5% | ~30MB | - |
| Portrait capture (500мс) | ~1-3% (кратковременно) | ~5MB | - |
| CatBoost инференс (500мс) | ~5-10% (кратковременно) | ~40MB (модель) | - |
| GSI HTTP-сервер | <0.1% | <1MB | - |
| SQLite (WAL mode) | <0.5% | ~2MB cache | ~1MB DB |
| HTTP запросы (фаза 1b) | ~1% | ~5MB | ~100KB логи |
| **Итого idle** | **~3%** | **~80MB** | - |
| **Итого в драфте** | **~10-15%** | **~80MB** | - |

---

## Логи

```
logs\console.log        Весь вывод программы
logs\curl_debug.txt     HTTP заголовки и трафик (для диагностики)
```
