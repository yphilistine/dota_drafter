# Dota 2 Draft Helper

Value-сеть для рекомендации героев в драфте. Предсказывает P(radiant win) для
частичного состояния драфта, ранжирует кандидатов по приросту вероятности победы.

## Быстрый старт

```bash
# Обучение (нужна PostgreSQL с данными матчей)
python train.py --conn "postgresql://user:pass@host:port/dota2db"

# Интерактивный драфт (БД не нужна)
python picker.py --player 475863708

# Одиночная рекомендация (БД не нужна)
python predict.py --radiant "1,14,,," --dire ",,,," --slot r3 --recommend --target-pos 1
```

## Файлы

| Файл | Назначение |
|------|-----------|
| `draft_features.py` | Построение фич, общий для train и serve |
| `train.py` | Обучение модели, нужна PostgreSQL |
| `predict.py` | Одиночная рекомендация, работает офлайн |
| `picker.py` | Интерактивный пошаговый драфт, работает офлайн |
| `evaluate.py` | Метрики качества по test predictions |

## Артефакты обучения

`train.py` создаёт три файла:

| Файл | Размер | Содержимое |
|------|--------|-----------|
| `draft_helper_abstract.cbm` | ~5 MB | CatBoost модель |
| `draft_helper_abstract_data.pkl` | ~62 MB | Все данные для serve (pickle) |
| `draft_helper_abstract_test_preds.csv` | ~4 MB | Предсказания на тесте |

`_data.pkl` содержит:

```python
{
    "hero_map":          {1: "Anti-Mage", 2: "Axe", ...},            # 127 героев
    "matchup_stats":     (global_wr, vs_wr, with_wr),                # OOF матчапы
    "modal_pos":         {1: 1, 74: 2, ...},                         # модальная позиция героя
    "hero_pos_wr":       {(hero_id, pos): [wins, games], ...},       # WR hero x position
    "player_hero_stats": {(account_id, hero_id): (wins, games), ...} # mastery всех игроков
}
```

---

## Архитектура модели

### Подход

**Критерий 1 — максимизация winrate.** Модель предсказывает P(radiant win | частичный
драфт). Кандидат подставляется в целевой слот, все кандидаты ранжируются по вероятности
победы. На частичном состоянии это амортизированный expectimax по эмпирической политике
оппонента — явный поиск по дереву не нужен.

### Волновая маскировка (action-boards)

Драфт идёт волнами. Внутри волны стороны пикают вслепую. Модель обучается на 11
состояниях per match — по одному на каждую точку принятия решения + пустой драфт:

```
Wave 1:  r1(1,0)  r2(2,0)  d1(0,1)  d2(0,2)     — blind picks
Wave 2:  r3(3,2)  r4(4,2)  d3(2,3)  d4(2,4)     — cross-side visible
Wave 3:  r5(5,4)  d5(4,5)                        — near-full draft
Anchor:  (0,0)                                    — empty baseline
```

Числа `(nr, nd)` = сколько героев radiant/dire раскрыто в этом состоянии.

### Фичи (52 total)

**10 категорных** — имя героя в каждом слоте (one-hot, 127 уровней):
```
r1_hero, r2_hero, ..., d5_hero
```

**30 matchup** — числовые, out-of-fold (5-fold OOF чтобы исключить self-inclusion leak):
```
{s}_global_wr   — smoothed winrate героя (GLOBAL_PRIOR=100)
{s}_avg_vs      — средний WR против раскрытых врагов (PAIR_PRIOR=20)
{s}_avg_with    — средний WR с раскрытыми союзниками (PAIR_PRIOR=20)
```

**2 mastery** — персональная статистика игрока (single-target, LOO):
```
target_hero_wr    — smoothed WR игрока на этом герое (MASTERY_PRIOR=30)
target_hero_games — log1p(games) игрока на этом герое
```
LOO: из all-time статистики `playerheroes` вычитается текущий матч, убирая прямой лик лейбла.
Mastery доступен только для target-слота — на serve известен только текущий игрок.

**10 hero_pos_wr** — winrate героя на конкретной позиции:
```
{s}_hero_pos_wr — smoothed WR hero×position (POS_PRIOR=50)
```
Своя сторона = true position из матча, враг = модальная позиция.
При serve: своя сторона = user input, враг = modal.

### Консистентность train/serve

Единый `build_state_vector()` в `draft_features.py` гарантирует одинаковое
построение фич при обучении и inference. Асимметрии решены:

| Фича | Train | Serve |
|------|-------|-------|
| Hero identity | one-hot name | one-hot name (тот же) |
| Matchup stats | OOF (no self-inclusion) | full-train stats |
| Mastery | LOO (вычтен текущий матч) | raw stats (матч ещё не сыгран) |
| Position own | true из данных | user input |
| Position enemy | modal | modal |

### CatBoost параметры

```python
learning_rate    = 0.01
depth            = 6
l2_leaf_reg      = 5.0
one_hot_max_size = 130     # one-hot вместо target encoding для героев
random_strength  = 2       # CPU only
early_stopping   = 300
eval_metric      = "AUC"   # CPU; Logloss на GPU
```

`one_hot_max_size=130` — критически важно. Target encoding 127-уровневых категорий
переобучается за 10 итераций. One-hot даёт бинарные фичи "герой X в слоте Y",
позволяя модели учить hero-hero взаимодействия через глубину дерева.

---

## train.py

```
python train.py --conn "postgresql://user:pass@host:port/db" [OPTIONS]
```

| Аргумент | Default | Описание |
|----------|---------|----------|
| `--conn` | localhost | PostgreSQL connection string |
| `--output` | `draft_helper_abstract.cbm` | Путь к модели |
| `--iterations` | 10000 | Макс. итераций CatBoost |
| `--test-size` | 0.1 | Доля тестовой выборки |
| `--seed` | 42 | Random seed |
| `--limit` | None | Ограничение матчей (для отладки) |
| `--gpu` | off | Обучение на GPU |
| `--folds` | 5 | Количество OOF-фолдов |

### Пайплайн

1. Загрузка матчей из `recentimmortalmatches` (heroes, players, positions)
2. Загрузка `playerheroes` для mastery (all-time stats)
3. Train/test split по match_id
4. Modal positions из train
5. **OOF loop** (5 folds):
   - Для каждого фолда: matchup stats + hero_pos_wr из остальных фолдов
   - Аугментация: 11 состояний per match с mastery (LOO)
6. Full-train stats для test set
7. CatBoost fit с early stopping
8. Сохранение: `.cbm` + `_data.pkl` + `_test_preds.csv`

### Таблицы БД

```
recentimmortalmatches:
  match_id, radiantwon,
  radiantpick1..5, direpick1..5,           — hero IDs
  radiantplayeronpick1..5, direplayeronpick1..5, — account IDs
  radiantheropick1pos..5, direheropick1pos..5     — positions 1-5

heroes:
  id, localized_name

playerheroes:
  account_id, hero_id, wins, games
```

---

## predict.py

Одиночная рекомендация. БД не нужна — всё из `_data.pkl`.

```
python predict.py --radiant "HERO_IDS" --dire "HERO_IDS" --slot SLOT --recommend [OPTIONS]
```

### Формат hero_ids

5 значений через запятую. Пусто = слот не занят:

```
"1,14,,,"     — r1=Anti-Mage, r2=Pudge, r3-r5 пусты
",,,,"        — все слоты пусты
"1,14,74,5,67" — полная команда
```

### Аргументы

| Аргумент | Описание |
|----------|----------|
| `--radiant` | Hero IDs radiant (обязательный) |
| `--dire` | Hero IDs dire (обязательный) |
| `--slot` | Целевой слот: r1..r5, d1..d5 |
| `--recommend` | Флаг рекомендации |
| `--top N` | Количество рекомендаций (default: 15) |
| `--model` | Путь к .cbm модели |
| `--player ID` | Account ID для mastery |
| `--target-pos N` | Позиция для рекомендуемого слота (1-5) |
| `--radiant-pos` | Позиции своих radiant: "1,2,,," |
| `--dire-pos` | Позиции своих dire: "5,4,,," |

### Примеры

```bash
# Кого взять carry на r3? У нас AM(r1, carry) + Pudge(r2, mid), враг неизвестен
python predict.py --radiant "1,14,,," --dire ",,,," \
    --slot r3 --recommend --target-pos 1 --radiant-pos "1,2,,,"

# Кого взять d5 support против известного состава? С учётом mastery игрока
python predict.py --radiant "1,14,74,5," --dire "86,33,39,," \
    --slot d5 --recommend --target-pos 5 --player 475863708

# Без позиций — модель использует модальную позицию каждого героя
python predict.py --radiant "1,14,,," --dire ",,,," --slot r3 --recommend
```

### Фильтрация по позиции

При `--target-pos` кандидаты фильтруются: только герои с >=0.5% всех игр на этой
позиции (~1350 игр). Это исключает off-role пики (Io carry, Luna support).

---

## picker.py

Интерактивный пошаговый драфт. БД не нужна.

```
python picker.py [--player ACCOUNT_ID] [--top 10] [--side r|d] [--model PATH]
```

### Процесс

1. Выбор стороны (Radiant / Dire)
2. Драфт по волнам:
   - **Wave 1**: r1, r2 (blind) → d1, d2 (blind)
   - **Wave 2**: r3, r4 (видят d1,d2) → d3, d4 (видят r1,r2)
   - **Wave 3**: r5 (видит d1-d4) → d5 (видит r1-r4)
3. На **вашем** пике:
   - Ввод позиции (1-5) или Enter = авто
   - Таблица рекомендаций с Win%, hero ID, mastery
   - Ввод героя: число (hero_id) или часть имени (`inv` → Invoker)
4. На **вражеском** пике: ввод что враг взял
5. Показ P(win) после каждого пика
6. Итоговый драфт с финальной вероятностью

### Ввод героя

- Число: hero_id (`74`)
- Текст: подстрока имени, case-insensitive (`pud` → Pudge, `anti` → Anti-Mage)
- Enter: пропустить слот

### Пример сессии

```
python picker.py --player 475863708 --side d

  DOTA 2 DRAFT HELPER
==================================================
Вы: DIRE
Player 475863708: mastery on 101 heroes loaded

==================================================
  WAVE 1 — Radiant blind
==================================================

  Враг r1 (id/имя, Enter=пропуск): invoker
  >> r1: Invoker

  Враг r2 (id/имя, Enter=пропуск): cm
  >> r2: Crystal Maiden

  d1 позиция (1-5, Enter=авто): 1

  Рекомендации для d1 (pos 1):
    #  Hero                         Win%     ID
  ----------------------------------------------
    1. Phantom Lancer             51.2%     12
    2. Slark                      50.8%      9  (30g 40%)
    3. Spectre                    50.5%     67
  ...
```

---

## evaluate.py

Метрики качества на test predictions.

```
python evaluate.py --preds draft_helper_abstract_test_preds.csv --model draft_helper_abstract.cbm
```

Выводит: AUC, Accuracy, LogLoss, Brier, ECE, per-state AUC, калибровку, feature importance.

---

## Бенчмарки (150k матчей immortal)

| Метрика | Значение |
|---------|---------|
| AUC overall | 0.582 |
| AUC full draft (4_5) | 0.613 |
| Accuracy | 0.561 |
| Brier skill | +2.5% |
| ECE | 0.008 |

### Per-state AUC

| Героев известно | AUC |
|-----------------|-----|
| 0 (пустой) | 0.500 |
| 1 | 0.554–0.575 |
| 2 | 0.563–0.580 |
| 5 | 0.589–0.593 |
| 6 | 0.596–0.598 |
| 9 (полный) | 0.609–0.613 |

### Feature importance

| Группа | % важности | Описание |
|--------|-----------|----------|
| Mastery | 27% | Персональная статистика игрока |
| Hero identity | 41% | One-hot имена героев |
| Hero×pos WR | 9% | Winrate hero на позиции |
| Global WR | 10% | Общий winrate героя |
| Matchups | 12% | Пайрвайзные vs/with |
