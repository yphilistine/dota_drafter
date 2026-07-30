/*
 * dota_picker.cpp - ML-пикер: CatBoost инференс + рекомендации героев.
 *
 * Модель: draft_helper_abstract.cbm (одна, все фазы драфта).
 * Данные: draft_helper_abstract_data.db (matchup, modal_pos, hero_pos_wr, pick_rates).
 * Мета-стата (immortal, DIVINE_IMMORTAL): playerandlivestats.db, таблица `stats`
 * (живой STRATZ heroStats, обновляется фазой 1a при каждом старте - см. datafetcher.cpp).
 *
 * Фичи: 10 cat (hero names) + 117 float, порядок побитово соответствует
 * ALL_FEATURES из draft_features.py (datafetcher-репо, Python-сторона модели):
 *   global_wr(10) -> vs_adv(10) -> with_adv(10) -> hero_pos_wr(10) ->
 *   best_vs(10) -> worst_vs(10) -> pick_rate(10) -> best_with(10) -> worst_with(10) ->
 *   team aggregates(17) -> composition/role-shape(10)
 * Без mastery в самом векторе - Component A "чисто про драфт", без привязки к
 * аккаунту. Персонализация (Component B, portировано из personal_score.py,
 * датафетчер-репо) применяется ПОСЛЕ модели: sigmoid(logit(p_ours) + beta*adj),
 * см. personalAdjustment/combinePersonal ниже. beta по умолчанию 0.0 - датафетчер
 * `calibrate_personal.py` не нашёл прироста accuracy при beta>0 ни при одном
 * протестированном приоре сглаживания; Component B посчитан и подключён к
 * ранжированию, но эффективно выключен, пока PERSONAL_BETA не поменяют осознанно.
 *
 * Composition/role-shape нужен hero_pos_wr.games (raw), которого в старых
 * draft_helper_abstract_data.db (schema_version=1) нет - loadMatchupData тянет
 * его отдельным запросом и деградирует до нулевого presence (макс. дефицит по
 * всем измерениям), если колонки нет, вместо падения.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "shared_types.h"
#include "version.h"
#include "version_utils.h"
#include "app_state.h"
#include "common.h"

#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <thread>
#include <atomic>

#include <sqlite3.h>
#include <c_api.h>

// --- Константы ---------------------------------------------------------------

static const char* UNKNOWN_HERO      = "unknown";
static constexpr int TOP_N           = 10;
static constexpr int POLL_INTERVAL_MS = 500;

// --- Внутренние структуры ----------------------------------------------------

struct PlayerStats {
    int games=0, wins=0;
};
// Не ImmortalHeroStats - так называется другая, несовместимая по форме
// структура в clouddatafetcher.h (hero_id/pos/games/wins/bans); разные имена
// снимают коллизию, хотя ODR-конфликта сегодня нет (файлы не инклюдят друг друга).
struct PickerHeroStat {
    int games=0, wins=0;
};
struct SlotData {
    int hero_id=0, position=0;
};
struct LivePick {
    int match_id=0, our_account_id=0, our_side=0, our_slot=0;
    long long updated_at=0;
    SlotData r[5], d[5];
    bool operator==(const LivePick& o) const {
        if (match_id!=o.match_id||updated_at!=o.updated_at) return false;
        for (int i=0;i<5;++i) {
            if (r[i].hero_id!=o.r[i].hero_id||r[i].position!=o.r[i].position) return false;
            if (d[i].hero_id!=o.d[i].hero_id||d[i].position!=o.d[i].position) return false;
        }
        return true;
    }
    bool operator!=(const LivePick& o) const { return !(*this==o); }
};

struct MatchupData {
    std::map<int, float> global_wr;
    std::map<std::pair<int,int>, float> vs_wr;
    std::map<std::pair<int,int>, float> with_wr;
    std::map<int, int> modal_pos;
    std::map<std::pair<int,int>, float> hero_pos_wr;
    std::map<std::pair<int,int>, int>   hero_pos_games; // raw games - для composition
    std::map<int, float> pick_rates;
};

// --- Team aggregates helper --------------------------------------------------

struct TeamAgg {
    int n=0;
    float meanVs=0.f, meanWith=0.f, meanGwr=0.5f, stdGwr=0.f, meanPr=0.f,
          bestVsMax=0.f, worstVsMin=0.f;
};

// base=0 -> слоты 0..4 (radiant), base=5 -> слоты 5..9 (dire). Портирует
// _team_aggregates из draft_features.py: при n==0 - те же дефолты
// (meanGwr=0.5, остальное 0), иначе среднее/std по раскрытым слотам стороны.
static TeamAgg computeTeamAgg(int base, const int ids[10], const float gwr[10],
    const float vsAdvAvg[10], const float withAdvAvg[10], const float vsAdvBest[10],
    const float vsAdvWorst[10], const float pickrate[10])
{
    TeamAgg a;
    float sumVs=0.f, sumWith=0.f, sumGwr=0.f, sumPr=0.f;
    bool first = true;
    for (int i = 0; i < 5; ++i) {
        int s = base + i;
        if (!ids[s]) continue;
        ++a.n;
        sumVs += vsAdvAvg[s]; sumWith += withAdvAvg[s]; sumGwr += gwr[s]; sumPr += pickrate[s];
        if (first) { a.bestVsMax = vsAdvBest[s]; a.worstVsMin = vsAdvWorst[s]; first = false; }
        else {
            if (vsAdvBest[s]  > a.bestVsMax)  a.bestVsMax  = vsAdvBest[s];
            if (vsAdvWorst[s] < a.worstVsMin) a.worstVsMin = vsAdvWorst[s];
        }
    }
    if (a.n == 0) return a;
    a.meanVs = sumVs / a.n; a.meanWith = sumWith / a.n;
    a.meanGwr = sumGwr / a.n; a.meanPr = sumPr / a.n;
    float var = 0.f;
    for (int i = 0; i < 5; ++i) {
        int s = base + i;
        if (!ids[s]) continue;
        float d = gwr[s] - a.meanGwr;
        var += d * d;
    }
    a.stdGwr = std::sqrt(var / a.n);
    return a;
}

// --- Composition / role-shape helper -----------------------------------------
// Портирует _team_composition/_hero_dim_presence из draft_features.py дословно.
// ROLE_DIMS порядок: core, support, safe, mid, off.

static constexpr int N_ROLE_DIMS = 5;
static constexpr float DIM_TARGET[N_ROLE_DIMS] = {3.f, 2.f, 2.f, 1.f, 2.f};
// POS_DIMS_MASK[pos], pos=1..5 (0 не используется) - бит i = вклад в dim i.
static constexpr int POS_DIMS_MASK[6] = {
    0,
    (1<<0)|(1<<2), // pos1 carry:        core, safe
    (1<<0)|(1<<3), // pos2 mid:          core, mid
    (1<<0)|(1<<4), // pos3 offlane:      core, off
    (1<<1)|(1<<4), // pos4 soft support: support, off
    (1<<1)|(1<<2), // pos5 hard support: support, safe
};

// presence(hero, dim) = доля игр героя (по всем позициям) в данной ролевой
// размерности. Без данных по герою (games=0 везде) - все 0, деградация
// graceful (не UB, не exception) для старых БД без колонки hero_pos_wr.games.
static void heroDimPresence(int hid, const MatchupData& md, float presence[N_ROLE_DIMS]) {
    int games[6] = {}; int total = 0;
    for (int p = 1; p <= 5; ++p) {
        auto it = md.hero_pos_games.find({hid, p});
        games[p] = (it != md.hero_pos_games.end()) ? it->second : 0;
        total += games[p];
    }
    for (int i = 0; i < N_ROLE_DIMS; ++i) presence[i] = 0.f;
    if (total <= 0) return;
    float freq[6];
    for (int p = 1; p <= 5; ++p) freq[p] = (float)games[p] / (float)total;
    presence[0] = freq[1] + freq[2] + freq[3]; // core
    presence[1] = freq[4] + freq[5];           // support
    presence[2] = freq[1] + freq[5];           // safe
    presence[3] = freq[2];                     // mid
    presence[4] = freq[3] + freq[4];           // off
}

// out[d] = -max(0, target(d) - filled(d)) / target(d) - 0 = размерность
// закрыта (или переукомплектована), -1 = совсем не закрыта.
static void teamComposition(int base, const int ids[10], const int positions[10],
    const MatchupData& md, float out[N_ROLE_DIMS])
{
    float filled[N_ROLE_DIMS] = {};
    for (int i = 0; i < 5; ++i) {
        int s = base + i;
        if (!ids[s] || positions[s] <= 0) continue;
        float presence[N_ROLE_DIMS];
        heroDimPresence(ids[s], md, presence);
        int mask = POS_DIMS_MASK[positions[s]];
        for (int d = 0; d < N_ROLE_DIMS; ++d)
            if (mask & (1 << d)) filled[d] += presence[d];
    }
    for (int d = 0; d < N_ROLE_DIMS; ++d) {
        float deficit = std::max(0.f, DIM_TARGET[d] - filled[d]) / DIM_TARGET[d];
        out[d] = -deficit;
    }
}

// --- Вектор признаков для CatBoost -------------------------------------------

static constexpr int N_CAT=10, N_FLOAT=117;
struct FeatureVector {
    std::string cat_str[N_CAT];
    const char* cat[N_CAT];
    float flt[N_FLOAT];
    void finalize() { for (int i=0;i<N_CAT;++i) cat[i]=cat_str[i].c_str(); }
};

static void buildVector(FeatureVector& v, const LivePick& lp,
    const std::map<int,std::string>& hero_map,
    const MatchupData& md,
    int candidate_hero_id)
{
    int our_r = (lp.our_side==1) ? (lp.our_slot-1) : -1;
    int our_d = (lp.our_side==0) ? (lp.our_slot-1) : -1;

    auto hero_name = [&](int hid) -> std::string {
        if (!hid) return UNKNOWN_HERO;
        auto it = hero_map.find(hid);
        return it != hero_map.end() ? it->second : UNKNOWN_HERO;
    };

    int ids[10] = {};
    for (int i = 0; i < 5; ++i) {
        ids[i]   = lp.r[i].hero_id;
        ids[5+i] = lp.d[i].hero_id;
    }
    if (candidate_hero_id) {
        if (our_r >= 0 && our_r < 5) ids[our_r]   = candidate_hero_id;
        if (our_d >= 0 && our_d < 5) ids[5+our_d]  = candidate_hero_id;
    }

    int positions[10] = {};
    for (int i = 0; i < 5; ++i) {
        positions[i]   = lp.r[i].position;
        positions[5+i] = lp.d[i].position;
    }
    int enemyBase = (lp.our_side == 1) ? 5 : 0;
    for (int i = enemyBase; i < enemyBase + 5; ++i) {
        if (positions[i] <= 0 && ids[i] > 0) {
            auto it = md.modal_pos.find(ids[i]);
            positions[i] = (it != md.modal_pos.end()) ? it->second : 0;
        }
    }

    // -- 10 categorical: hero names --
    for (int i = 0; i < 10; ++i)
        v.cat_str[i] = hero_name(ids[i]);

    // -- Per-slot precompute: global_wr --
    float gwr[10];
    for (int s = 0; s < 10; ++s) {
        gwr[s] = 0.5f;
        if (ids[s]) {
            auto it = md.global_wr.find(ids[s]);
            if (it != md.global_wr.end()) gwr[s] = it->second;
        }
    }

    // -- Per-slot precompute: vs_adv / with_adv raw arrays (для avg+best+worst) --
    float vs_adv_arr[10][5];   int vs_adv_cnt[10] = {};
    float with_adv_arr[10][5]; int with_adv_cnt[10] = {};
    for (int s = 0; s < 10; ++s) {
        if (!ids[s]) continue;
        int enemyStart = (s < 5) ? 5 : 0;
        for (int e = enemyStart; e < enemyStart + 5; ++e) {
            if (!ids[e]) continue;
            auto it = md.vs_wr.find({ids[s], ids[e]});
            if (it != md.vs_wr.end())
                vs_adv_arr[s][vs_adv_cnt[s]++] = it->second - gwr[s];
        }
        int allyStart = (s < 5) ? 0 : 5;
        for (int a = allyStart; a < allyStart + 5; ++a) {
            if (a == s || !ids[a]) continue;
            auto it = md.with_wr.find({ids[s], ids[a]});
            if (it != md.with_wr.end())
                with_adv_arr[s][with_adv_cnt[s]++] = it->second - gwr[s];
        }
    }

    // -- Per-slot reduce: avg/best/worst для vs_adv и with_adv --
    float vs_adv_avg[10]={}, vs_adv_best[10]={}, vs_adv_worst[10]={};
    float with_adv_avg[10]={}, with_adv_best[10]={}, with_adv_worst[10]={};
    for (int s = 0; s < 10; ++s) {
        if (vs_adv_cnt[s] > 0) {
            float sum = 0.f, best = vs_adv_arr[s][0], worst = vs_adv_arr[s][0];
            for (int j = 0; j < vs_adv_cnt[s]; ++j) {
                sum += vs_adv_arr[s][j];
                if (vs_adv_arr[s][j] > best)  best  = vs_adv_arr[s][j];
                if (vs_adv_arr[s][j] < worst) worst = vs_adv_arr[s][j];
            }
            vs_adv_avg[s] = sum / vs_adv_cnt[s];
            vs_adv_best[s] = best; vs_adv_worst[s] = worst;
        }
        if (with_adv_cnt[s] > 0) {
            float sum = 0.f, best = with_adv_arr[s][0], worst = with_adv_arr[s][0];
            for (int j = 0; j < with_adv_cnt[s]; ++j) {
                sum += with_adv_arr[s][j];
                if (with_adv_arr[s][j] > best)  best  = with_adv_arr[s][j];
                if (with_adv_arr[s][j] < worst) worst = with_adv_arr[s][j];
            }
            with_adv_avg[s] = sum / with_adv_cnt[s];
            with_adv_best[s] = best; with_adv_worst[s] = worst;
        }
    }

    // -- Per-slot precompute: hero_pos_wr, pick_rate --
    float hpwr[10], pickrate[10];
    for (int s = 0; s < 10; ++s) {
        hpwr[s] = 0.5f;
        if (ids[s] && positions[s] > 0) {
            auto it = md.hero_pos_wr.find({ids[s], positions[s]});
            if (it != md.hero_pos_wr.end()) hpwr[s] = it->second;
        }
        pickrate[s] = 0.f;
        if (ids[s]) {
            auto it = md.pick_rates.find(ids[s]);
            if (it != md.pick_rates.end()) pickrate[s] = it->second;
        }
    }

    // -- Запись в порядке ALL_FEATURES (draft_features.py) --
    int fi = 0;
    for (int s = 0; s < 10; ++s) v.flt[fi++] = ids[s] ? gwr[s] : 0.5f;      // global_wr
    for (int s = 0; s < 10; ++s) v.flt[fi++] = vs_adv_avg[s];              // vs_adv
    for (int s = 0; s < 10; ++s) v.flt[fi++] = with_adv_avg[s];            // with_adv
    for (int s = 0; s < 10; ++s) v.flt[fi++] = ids[s] ? hpwr[s] : 0.5f;    // hero_pos_wr
    for (int s = 0; s < 10; ++s) v.flt[fi++] = vs_adv_best[s];             // best_vs
    for (int s = 0; s < 10; ++s) v.flt[fi++] = vs_adv_worst[s];            // worst_vs
    for (int s = 0; s < 10; ++s) v.flt[fi++] = pickrate[s];                // pick_rate
    for (int s = 0; s < 10; ++s) v.flt[fi++] = with_adv_best[s];           // best_with
    for (int s = 0; s < 10; ++s) v.flt[fi++] = with_adv_worst[s];          // worst_with

    // -- Team aggregates (17): метрика-внешний-цикл / сторона-внутренний --
    TeamAgg rAgg = computeTeamAgg(0, ids, gwr, vs_adv_avg, with_adv_avg,
                                  vs_adv_best, vs_adv_worst, pickrate);
    TeamAgg dAgg = computeTeamAgg(5, ids, gwr, vs_adv_avg, with_adv_avg,
                                  vs_adv_best, vs_adv_worst, pickrate);
    v.flt[fi++] = (float)rAgg.n;   v.flt[fi++] = (float)dAgg.n;
    v.flt[fi++] = rAgg.meanVs;     v.flt[fi++] = dAgg.meanVs;
    v.flt[fi++] = rAgg.meanWith;   v.flt[fi++] = dAgg.meanWith;
    v.flt[fi++] = rAgg.meanGwr;    v.flt[fi++] = dAgg.meanGwr;
    v.flt[fi++] = rAgg.stdGwr;     v.flt[fi++] = dAgg.stdGwr;
    v.flt[fi++] = rAgg.meanPr;     v.flt[fi++] = dAgg.meanPr;
    v.flt[fi++] = rAgg.bestVsMax;  v.flt[fi++] = dAgg.bestVsMax;
    v.flt[fi++] = rAgg.worstVsMin; v.flt[fi++] = dAgg.worstVsMin;
    v.flt[fi++] = rAgg.meanVs - dAgg.meanVs; // team_vs_adv_diff

    // -- Composition/role-shape (10): сторона-внешний / dim-внутренний --
    float rComp[N_ROLE_DIMS], dComp[N_ROLE_DIMS];
    teamComposition(0, ids, positions, md, rComp);
    teamComposition(5, ids, positions, md, dComp);
    for (int d = 0; d < N_ROLE_DIMS; ++d) v.flt[fi++] = rComp[d];
    for (int d = 0; d < N_ROLE_DIMS; ++d) v.flt[fi++] = dComp[d];

    v.finalize();
}

// SQLite-обёртки (readonly) - SqliteDB/SqliteStmt из common.h.

// --- Загрузка данных из SQLite ------------------------------------------------

static std::map<int,std::string> loadHeroes(sqlite3* db) {
    std::map<int,std::string> m;
    SqliteStmt st(db,"SELECT id,localized_name FROM heroes");
    while (st.row()) m[st.col_int(0)]=st.col_text(1);
    return m;
}

static MatchupData loadMatchupData(sqlite3* db) {
    MatchupData md;

    try {
        SqliteStmt st(db,"SELECT hero_id,wr FROM global_wr");
        while (st.row()) md.global_wr[st.col_int(0)] = (float)st.col_double(1);
    } catch (...) { LOG_WARN("[picker] global_wr not found"); }

    try {
        SqliteStmt st(db,"SELECT hero_id,opp_hero_id,wr FROM vs_wr");
        while (st.row()) md.vs_wr[{st.col_int(0),st.col_int(1)}] = (float)st.col_double(2);
    } catch (...) { LOG_WARN("[picker] vs_wr not found"); }

    try {
        SqliteStmt st(db,"SELECT hero_id,ally_hero_id,wr FROM with_wr");
        while (st.row()) md.with_wr[{st.col_int(0),st.col_int(1)}] = (float)st.col_double(2);
    } catch (...) { LOG_WARN("[picker] with_wr not found"); }

    try {
        SqliteStmt st(db,"SELECT hero_id,pos FROM modal_pos");
        while (st.row()) md.modal_pos[st.col_int(0)] = st.col_int(1);
    } catch (...) { LOG_WARN("[picker] modal_pos not found"); }

    try {
        SqliteStmt st(db,"SELECT hero_id,pos,wr FROM hero_pos_wr");
        while (st.row()) md.hero_pos_wr[{st.col_int(0),st.col_int(1)}] = (float)st.col_double(2);
    } catch (...) { LOG_WARN("[picker] hero_pos_wr not found"); }

    // Отдельный запрос от wr - на старой БД (schema_version=1, без колонки games)
    // должен упасть только этот try, не потеряв уже загруженный hero_pos_wr.
    // Без games composition-фича деградирует до нулевого presence (см. heroDimPresence),
    // не падает.
    try {
        SqliteStmt st(db,"SELECT hero_id,pos,games FROM hero_pos_wr");
        while (st.row()) md.hero_pos_games[{st.col_int(0),st.col_int(1)}] = st.col_int(2);
    } catch (...) { LOG_WARN("[picker] hero_pos_wr.games not found (old schema - composition feature degraded)"); }

    try {
        SqliteStmt st(db,"SELECT hero_id,rate FROM pick_rates");
        while (st.row()) md.pick_rates[st.col_int(0)] = (float)st.col_double(1);
    } catch (...) { LOG_WARN("[picker] pick_rates not found"); }

    LOG_INFO("[picker] Matchup data: gwr=" << md.global_wr.size() << " vs=" << md.vs_wr.size()
        << " with=" << md.with_wr.size() << " modal=" << md.modal_pos.size()
        << " hpwr=" << md.hero_pos_wr.size() << " hpwr_games=" << md.hero_pos_games.size()
        << " pr=" << md.pick_rates.size());
    return md;
}

// Порог отсечения кандидатов - динамический: 1% от суммарных игр на позиции за неделю.
// Данные - недельный STRATZ-снимок, а не all-time агрегат, поэтому фиксированное
// число игр не подошло бы как порог.
static std::map<int,PickerHeroStat> loadImmortalHeroStats(sqlite3* db, int position) {
    std::map<int,PickerHeroStat> m;
    if (position <= 0 || position > 5) return m;
    try {
        std::vector<std::tuple<int,int,int>> rows; // hero_id, games, wins
        long long totalGames = 0;
        SqliteStmt st(db,"SELECT hero_id,games,wins FROM stats WHERE pos=?");
        st.bind_int(1,position);
        while (st.row()) {
            int hid=st.col_int(0), g=st.col_int(1), w=st.col_int(2);
            rows.emplace_back(hid,g,w);
            totalGames += g;
        }
        long long minGames = (long long)(totalGames * 0.01);
        for (auto& [hid,g,w] : rows)
            if (g >= minGames) m[hid] = {g, w};
    } catch (...) {}
    return m;
}

static std::map<int,PlayerStats> loadPlayerStats(sqlite3* db, int account_id) {
    std::map<int,PlayerStats> m;
    try {
        SqliteStmt st(db,"SELECT hero_id,games,wins FROM playerheroes WHERE account_id=?");
        st.bind_int(1,account_id);
        while (st.row()) m[st.col_int(0)] = {st.col_int(1), st.col_int(2)};
    } catch (...) {}
    return m;
}

// Component B: "форма" - recent ranked, короче история/слабее приор, чем all-time.
static std::map<int,PlayerStats> loadPlayerHeroesRanked(sqlite3* db, int account_id) {
    std::map<int,PlayerStats> m;
    try {
        SqliteStmt st(db,"SELECT hero_id,games,wins FROM playerheroesranked WHERE account_id=?");
        st.bind_int(1,account_id);
        while (st.row()) m[st.col_int(0)] = {st.col_int(1), st.col_int(2)};
    } catch (...) {}
    return m;
}

// Component B: перс. WR героя на конкретной позиции. Без фильтра по позиции -
// как в personal_score.py, все позиции аккаунта загружаются одним запросом.
static std::map<std::pair<int,int>,PlayerStats> loadPlayerHeroPos(sqlite3* db, int account_id) {
    std::map<std::pair<int,int>,PlayerStats> m;
    try {
        SqliteStmt st(db,"SELECT heroId,position,games,wins FROM relevantplayerherobyposstats WHERE account_id=?");
        st.bind_int(1,account_id);
        while (st.row()) m[{st.col_int(0),st.col_int(1)}] = {st.col_int(2), st.col_int(3)};
    } catch (...) {}
    return m;
}

static bool loadLatestLivePick(sqlite3* db, LivePick& lp) {
    SqliteStmt st(db,R"(
        SELECT match_id,our_account_id,our_side,our_slot,updated_at,
               r1_hero,r1_pos,r2_hero,r2_pos,r3_hero,r3_pos,r4_hero,r4_pos,r5_hero,r5_pos,
               d1_hero,d1_pos,d2_hero,d2_pos,d3_hero,d3_pos,d4_hero,d4_pos,d5_hero,d5_pos
        FROM livepicks ORDER BY updated_at DESC LIMIT 1
    )");
    if (!st.row()) return false;
    lp.match_id=st.col_int(0); lp.our_account_id=st.col_int(1);
    lp.our_side=st.col_int(2); lp.our_slot=st.col_int(3);
    lp.updated_at=st.col_int64(4);
    for (int i=0;i<5;++i) {
        lp.r[i].hero_id  = st.col_null(5+i*2)   ? 0 : st.col_int(5+i*2);
        lp.r[i].position = st.col_null(5+i*2+1) ? 0 : st.col_int(5+i*2+1);
    }
    for (int i=0;i<5;++i) {
        lp.d[i].hero_id  = st.col_null(15+i*2)   ? 0 : st.col_int(15+i*2);
        lp.d[i].position = st.col_null(15+i*2+1) ? 0 : st.col_int(15+i*2+1);
    }
    return true;
}

// --- CatBoost инференс -------------------------------------------------------

static inline double sigmoid(double x) { return 1.0/(1.0+std::exp(-x)); }

static std::vector<double> runBatch(ModelCalcerHandle* model, std::vector<FeatureVector>& batch) {
    size_t n = batch.size();
    std::vector<double> result(n, 0.0);
    std::vector<const float*> fp(n);
    std::vector<const char**> cp(n);
    for (size_t i = 0; i < n; ++i) { fp[i] = batch[i].flt; cp[i] = batch[i].cat; }
    if (!CalcModelPrediction(model, n, fp.data(), N_FLOAT, cp.data(), N_CAT, result.data(), n))
        throw std::runtime_error(std::string("Inference failed: ") + GetErrorString());
    for (double& v : result) v = sigmoid(v);
    return result;
}

// --- Component B: персональная поправка (personal_score.py, датафетчер-репо) --
// Портирует personal_adjustment/combine дословно, кроме источника base_wr: там
// это immortalherostats (таблица удалена из finalapp - см. header-комментарий),
// здесь - уже загруженный immortal_map (живая STRATZ-стата,
// playerandlivestats.db::stats), с тем же порогом PERSONAL_BASE_WR_MIN_GAMES.

static constexpr float PERSONAL_PRIOR_ALLTIME     = 40.0f;
static constexpr float PERSONAL_PRIOR_RANKED      = 15.0f;
static constexpr float PERSONAL_W_ALLTIME         = 0.6f;
static constexpr float PERSONAL_W_RANKED          = 0.4f;
static constexpr float PERSONAL_COMFORT_SCALE     = 0.01f;
static constexpr float PERSONAL_COMFORT_CAP       = 0.03f;
static constexpr int   PERSONAL_BASE_WR_MIN_GAMES = 200;
// Дефолт 0.0 по калибровке (calibrate_personal.py): AUC монотонно падает при
// beta>0 для всех протестированных приоров сглаживания, оптимум везде beta=0.
// Component B посчитан и подключён к ранжированию, но эффективно выключен,
// пока эту константу не поменяют осознанно.
static constexpr float PERSONAL_BETA              = 0.0f;

static float personalAdjustment(int hero_id, int pos,
    const std::map<int,PlayerStats>&                heroAlltime,
    const std::map<int,PlayerStats>&                heroRanked,
    const std::map<std::pair<int,int>,PlayerStats>& heroPos,
    const std::map<int,PickerHeroStat>&              basePosStats)
{
    if (pos <= 0 || !hero_id) return 0.f;

    float baseWr = 0.5f;
    auto bit = basePosStats.find(hero_id);
    if (bit != basePosStats.end() && bit->second.games >= PERSONAL_BASE_WR_MIN_GAMES)
        baseWr = (float)bit->second.wins / (float)bit->second.games;

    int wAt = 0, gAt = 0;
    auto pit = heroPos.find({hero_id, pos});
    if (pit != heroPos.end() && pit->second.games >= 3) {
        wAt = pit->second.wins; gAt = pit->second.games;
    } else {
        auto ait = heroAlltime.find(hero_id);
        if (ait != heroAlltime.end()) { wAt = ait->second.wins; gAt = ait->second.games; }
    }
    float smoothedAlltime = (wAt + PERSONAL_PRIOR_ALLTIME * baseWr) / (gAt + PERSONAL_PRIOR_ALLTIME);

    int wRk = 0, gRk = 0;
    auto rit = heroRanked.find(hero_id);
    if (rit != heroRanked.end()) { wRk = rit->second.wins; gRk = rit->second.games; }
    float smoothedRanked = (wRk + PERSONAL_PRIOR_RANKED * baseWr) / (gRk + PERSONAL_PRIOR_RANKED);

    int gAll = 0;
    auto ait2 = heroAlltime.find(hero_id);
    if (ait2 != heroAlltime.end()) gAll = ait2->second.games;
    float comfort = std::min(PERSONAL_COMFORT_CAP,
                             PERSONAL_COMFORT_SCALE * std::log1p((float)gAll));

    return PERSONAL_W_ALLTIME * (smoothedAlltime - baseWr) +
           PERSONAL_W_RANKED  * (smoothedRanked  - baseWr) +
           comfort;
}

// final = sigmoid(logit(p_ours) + beta*adj). p_ours - вероятность победы НАШЕЙ
// стороны (уже ориентированная), не сырой radiant-вероятности - см. вызов ниже.
static inline double combinePersonal(double pOurs, float adj, float beta) {
    double p = std::min(std::max(pOurs, 1e-6), 1.0 - 1e-6);
    double logit = std::log(p / (1.0 - p));
    return 1.0 / (1.0 + std::exp(-(logit + (double)beta * (double)adj)));
}

// --- GUI режим: результаты -> GuiPickerState ---------------------------------

static void renderToGui(
    GuiPickerState*                        state,
    const LivePick&                        lp,
    const std::map<int,std::string>&       hero_map,
    const MatchupData&                     md,
    const std::map<int,PlayerStats>&       our_stats,
    const std::map<int,PlayerStats>&       our_stats_ranked,
    const std::map<std::pair<int,int>,PlayerStats>& our_stats_pos,
    const std::map<int,PickerHeroStat>&    immortal_map,
    ModelCalcerHandle*                     model)
{
    if (!state) return;

    auto hname = [&](int hid) -> std::string {
        if (!hid) return "";
        auto it = hero_map.find(hid);
        return it != hero_map.end() ? it->second : "";
    };

    int our_r = -1, our_d = -1;
    if (lp.our_slot >= 1 && lp.our_slot <= 5) {
        int idx = lp.our_slot - 1;
        if (lp.our_side == 1) our_r = idx; else our_d = idx;
    }

    std::lock_guard<std::mutex> lk(state->mtx);

    state->isRadiant = (lp.our_side == 1);
    state->ourSlot   = lp.our_slot;
    bool flipProb    = (lp.our_side != 1);

    for (int i = 0; i < 5; i++) {
        HeroSlotGui& s = state->radiant[i];
        s.heroId = lp.r[i].hero_id;
        s.pos    = lp.r[i].position;
        s.filled = (lp.r[i].hero_id != 0);
        s.isYou  = (i == our_r);
        auto n   = hname(lp.r[i].hero_id);
        std::snprintf(s.name, sizeof(s.name), "%s", n.c_str());
    }
    for (int i = 0; i < 5; i++) {
        HeroSlotGui& s = state->dire[i];
        s.heroId = lp.d[i].hero_id;
        s.pos    = lp.d[i].position;
        s.filled = (lp.d[i].hero_id != 0);
        s.isYou  = (i == our_d);
        auto n   = hname(lp.d[i].hero_id);
        std::snprintf(s.name, sizeof(s.name), "%s", n.c_str());
    }

    int our_hero = (our_r >= 0 && our_r < 5) ? lp.r[our_r].hero_id : 0;
    if (our_d >= 0 && our_d < 5) our_hero = lp.d[our_d].hero_id;

    state->ourHeroPicked = (our_hero != 0);
    state->ourPosition   = 0;
    if (our_r >= 0 && our_r < 5) state->ourPosition = lp.r[our_r].position;
    if (our_d >= 0 && our_d < 5) state->ourPosition = lp.d[our_d].position;

    if (our_hero != 0) {
        FeatureVector v;
        buildVector(v, lp, hero_map, md, 0);
        std::vector<FeatureVector> batch = {v};
        auto probs = runBatch(model, batch);
        bool rad = (lp.our_side == 1);
        double pOurs = rad ? probs[0] : (1.0 - probs[0]);
        float adj = personalAdjustment(our_hero, state->ourPosition, our_stats,
                                       our_stats_ranked, our_stats_pos, immortal_map);
        double pOursFinal = combinePersonal(pOurs, adj, PERSONAL_BETA);
        state->winProb = (float)pOursFinal;
        auto on = hname(our_hero);
        std::snprintf(state->ourHeroName, sizeof(state->ourHeroName), "%s", on.c_str());
        state->ourHeroId = our_hero;
        state->recCount = 0;

    } else {
        {
            FeatureVector v0;
            buildVector(v0, lp, hero_map, md, 0);
            std::vector<FeatureVector> b0 = {v0};
            auto p0 = runBatch(model, b0);
            state->winProb = flipProb ? 1.f - (float)p0[0] : (float)p0[0];
        }
        std::set<int> picked;
        for (int i = 0; i < 5; i++) {
            if (lp.r[i].hero_id) picked.insert(lp.r[i].hero_id);
            if (lp.d[i].hero_id) picked.insert(lp.d[i].hero_id);
        }
        bool use_immortal = !immortal_map.empty();
        std::vector<int> pool;
        for (auto& [hid, _] : hero_map) {
            if (picked.count(hid)) continue;
            if (use_immortal && !immortal_map.count(hid)) continue;
            pool.push_back(hid);
        }

        state->recCount = 0;
        if (!pool.empty()) {
            std::vector<FeatureVector> batch(pool.size());
            for (size_t i = 0; i < pool.size(); i++)
                buildVector(batch[i], lp, hero_map, md, pool[i]);
            auto probs = runBatch(model, batch);

            bool rad = (lp.our_side == 1);
            int our_pos = state->ourPosition;

            // Component B: probs[i] - сырая radiant-вероятность; ориентируем в
            // "нашу" сторону, применяем персональную поправку, ориентируем обратно
            // для сортировки/флипа на дисплее (при PERSONAL_BETA=0.0 pRawFinal
            // равен probs[i] побитово - поправка эффективно не действует).
            std::vector<std::pair<double,int>> ranked;
            ranked.reserve(pool.size());
            for (size_t i = 0; i < pool.size(); i++) {
                double pOurs = rad ? probs[i] : (1.0 - probs[i]);
                float adj = personalAdjustment(pool[i], our_pos, our_stats,
                                               our_stats_ranked, our_stats_pos, immortal_map);
                double pOursFinal = combinePersonal(pOurs, adj, PERSONAL_BETA);
                double pRawFinal = rad ? pOursFinal : (1.0 - pOursFinal);
                ranked.push_back({pRawFinal, pool[i]});
            }
            if (rad)
                std::sort(ranked.begin(), ranked.end(),
                    [](auto& a, auto& b){ return a.first > b.first; });
            else
                std::sort(ranked.begin(), ranked.end(),
                    [](auto& a, auto& b){ return a.first < b.first; });

            int n = (int)std::min(ranked.size(), (size_t)10);
            state->recCount = n;

            for (int i = 0; i < n; i++) {
                auto& [prob, hid] = ranked[i];
                PickRowGui& r = state->recs[i];
                r.rank    = i + 1;
                r.heroId  = hid;
                r.winProb = flipProb ? 1.f - (float)prob : (float)prob;
                auto hn   = hname(hid);
                std::snprintf(r.name, sizeof(r.name), "%s", hn.c_str());

                auto pl  = our_stats.count(hid)   ? our_stats.at(hid)   : PlayerStats{};
                auto imm = immortal_map.count(hid) ? immortal_map.at(hid) : PickerHeroStat{};

                r.gamesPlayer = pl.games;
                r.wrPlayer    = pl.games > 0 ? (float)pl.wins / pl.games : 0.f;
                r.gamesImm    = imm.games;
                r.wrImm       = imm.games > 0 ? (float)imm.wins / imm.games : 0.f;
            }
        }
    }

    state->gameStarted = true;
    state->inferenceGen.fetch_add(1, std::memory_order_release);
}

// --- CatBoost model RAII -----------------------------------------------------
// Без этого ModelCalcerDelete вызывался только после нормального завершения
// цикла (см. ниже) - исключение из runBatch/renderToGui посреди цикла
// пропускало освобождение и утекало модель.
class ModelHandle {
    ModelCalcerHandle* h_;
public:
    explicit ModelHandle(ModelCalcerHandle* h) : h_(h) {}
    ~ModelHandle() { if (h_) ModelCalcerDelete(h_); }
    ModelCalcerHandle* get() const { return h_; }
    ModelHandle(const ModelHandle&) = delete;
    ModelHandle& operator=(const ModelHandle&) = delete;
};

// --- Наблюдаемая игра (спектейт): Component A без личных данных -------------
// Та же модель/buildVector/runBatch, что и для своей игры, но без Component B
// (personalAdjustment/combinePersonal) - для чужого матча нет accountId,
// сверяться не с чем, нужно только "чисто про драфт" предсказание.
// candidate_hero_id всегда 0 (все 10 героев уже известны из GSI, ранжировать
// нечего) - LivePick.our_side/our_slot здесь ни на что не влияют.
static void computeSpectatorPrediction(ModelCalcerHandle* model,
    const std::map<int,std::string>& hero_map, const MatchupData& md,
    const SpectatorHeroSlot spec[10], SpectatorDraftState* specState)
{
    LivePick lp; // match_id/our_account_id/our_side/our_slot/updated_at не используются
    for (int i = 0; i < 5; ++i) {
        lp.r[i].hero_id  = spec[i].heroId;
        lp.d[i].hero_id  = spec[5+i].heroId;
        lp.r[i].position = spec[i].manualPos;
        lp.d[i].position = spec[5+i].manualPos;
    }

    // modal_pos fallback - симметрично для обеих сторон. buildVector делает это
    // только для "вражеской" стороны (в своей игре позиции своей команды идут
    // из OCR/portrait capture) - здесь такого источника нет ни у той, ни у
    // другой стороны, обе заполняются одинаково, до вызова buildVector.
    for (int i = 0; i < 5; ++i) {
        if (lp.r[i].position <= 0 && lp.r[i].hero_id) {
            auto it = md.modal_pos.find(lp.r[i].hero_id);
            if (it != md.modal_pos.end()) lp.r[i].position = it->second;
        }
        if (lp.d[i].position <= 0 && lp.d[i].hero_id) {
            auto it = md.modal_pos.find(lp.d[i].hero_id);
            if (it != md.modal_pos.end()) lp.d[i].position = it->second;
        }
    }

    FeatureVector v;
    buildVector(v, lp, hero_map, md, 0);
    std::vector<FeatureVector> batch = {v};
    auto probs = runBatch(model, batch); // может бросить - ловит вызывающий цикл

    std::lock_guard<std::mutex> lk(specState->mtx);
    specState->radiantWinProb = (float)probs[0];
    specState->hasPrediction  = true;
}

int runSpectatorPickerGui(const char* model_path, const char* db_path,
                          std::atomic<bool>& running,
                          SpectatorDraftState* specState)
{
    try {
        auto loadModel = [](const std::string& path) -> ModelCalcerHandle* {
            ModelCalcerHandle* m = ModelCalcerCreate();
            if (!LoadFullModelFromFile(m, path.c_str())) {
                ModelCalcerDelete(m);
                throw std::runtime_error("Cannot load model: " + path
                                         + " - " + GetErrorString());
            }
            return m;
        };
        std::string base(model_path);
        std::string dataDbPath = base + "_data.db";

        try {
            auto dm = readDataDbMeta(dataDbPath);
            if (dm.schema != kSupportedSchema) {
                LOG_WARN("[spectator_picker] Data schema " << dm.schema
                         << " != app schema " << kSupportedSchema << " - skipping predictions");
                return 1;
            }
        } catch (const std::exception& ex) {
            LOG_WARN("[spectator_picker] Cannot read data meta: " << ex.what()
                     << " - proceeding (legacy data)");
        }

        ModelHandle model(loadModel(base + ".cbm"));

        SqliteDB db(db_path, /*readOnly=*/true);
        auto hero_map = loadHeroes(db.get());

        SqliteDB dataDb(dataDbPath, /*readOnly=*/true);
        auto md = loadMatchupData(dataDb.get());

        SpectatorHeroSlot last[10] = {};
        bool haveLast = false;

        while (running.load()) {
            SpectatorHeroSlot cur[10];
            bool active;
            {
                std::lock_guard<std::mutex> lk(specState->mtx);
                active = specState->active;
                for (int i = 0; i < 10; ++i) cur[i] = specState->slots[i];
            }
            if (!active) {
                haveLast = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
                continue;
            }

            bool changed = !haveLast;
            for (int i = 0; i < 10 && !changed; ++i)
                if (cur[i].heroId != last[i].heroId || cur[i].manualPos != last[i].manualPos)
                    changed = true;

            if (changed) {
                try {
                    computeSpectatorPrediction(model.get(), hero_map, md, cur, specState);
                    requestRedraw();
                } catch (const std::exception& e) {
                    LOG_WARN("[spectator_picker] inference failed: " << e.what());
                }
                for (int i = 0; i < 10; ++i) last[i] = cur[i];
                haveLast = true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }

    } catch (const std::exception& e) {
        LOG_ERR("[spectator_picker] Error: " << e.what());
        return 1;
    }
    return 0;
}

// --- Главный цикл пикера -----------------------------------------------------

int runPickerGui(const char* model_path, const char* db_path,
                 std::atomic<bool>& running,
                 GuiPickerState* guiState,
                 SharedPortraitState* /*portraitState*/)
{
    try {
        auto loadModel = [](const std::string& path) -> ModelCalcerHandle* {
            ModelCalcerHandle* m = ModelCalcerCreate();
            if (!LoadFullModelFromFile(m, path.c_str())) {
                ModelCalcerDelete(m);
                throw std::runtime_error("Cannot load model: " + path
                                         + " - " + GetErrorString());
            }
            return m;
        };
        std::string base(model_path);
        std::string dataDbPath = base + "_data.db";

        // -- Schema compatibility gate ------------------------------------
        try {
            auto dm = readDataDbMeta(dataDbPath);
            if (dm.schema != kSupportedSchema) {
                LOG_ERR("[picker] Data schema " << dm.schema
                        << " != app schema " << kSupportedSchema);
                if (guiState) {
                    std::lock_guard<std::mutex> lk(guiState->mtx);
                    guiState->schemaError = true;
                    std::snprintf(guiState->schemaMsg, sizeof(guiState->schemaMsg),
                        "Incompatible data (schema %d, app supports %d). Update the app.",
                        dm.schema, kSupportedSchema);
                }
                requestRedraw();
                return 1;
            }
        } catch (const std::exception& ex) {
            LOG_WARN("[picker] Cannot read data meta: " << ex.what()
                     << " - proceeding (legacy data)");
        }

        ModelHandle model(loadModel(base + ".cbm"));

        SqliteDB db(db_path, /*readOnly=*/true);
        auto hero_map = loadHeroes(db.get());

        SqliteDB dataDb(dataDbPath, /*readOnly=*/true);
        auto md = loadMatchupData(dataDb.get());

        if (guiState) {
            std::lock_guard<std::mutex> lk(guiState->mtx);
            guiState->active = true;
        }

        LivePick last_lp; last_lp.match_id = -1;
        std::map<int, PlayerStats>                      our_stats;
        std::map<int, PlayerStats>                      our_stats_ranked;
        std::map<std::pair<int,int>, PlayerStats>       our_stats_pos;
        std::map<int, PickerHeroStat>    immortal_map;
        int last_account_id = -1, last_our_pos = -1;

        while (running.load()) {
            LivePick lp;
            bool ok = false;
            try { ok = loadLatestLivePick(db.get(), lp); }
            catch (...) {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
                continue;
            }
            if (!ok) {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
                continue;
            }

            if (lp.our_account_id != last_account_id) {
                our_stats = (lp.our_account_id != 0)
                    ? loadPlayerStats(db.get(), lp.our_account_id)
                    : std::map<int,PlayerStats>{};
                our_stats_ranked = (lp.our_account_id != 0)
                    ? loadPlayerHeroesRanked(db.get(), lp.our_account_id)
                    : std::map<int,PlayerStats>{};
                our_stats_pos = (lp.our_account_id != 0)
                    ? loadPlayerHeroPos(db.get(), lp.our_account_id)
                    : std::map<std::pair<int,int>,PlayerStats>{};
                last_account_id = lp.our_account_id;
            }

            int our_r = (lp.our_side == 1) ? (lp.our_slot - 1) : -1;
            int our_d = (lp.our_side == 0) ? (lp.our_slot - 1) : -1;
            int our_pos = 0;
            if (our_r >= 0 && our_r < 5) our_pos = lp.r[our_r].position;
            if (our_d >= 0 && our_d < 5) our_pos = lp.d[our_d].position;

            if (our_pos != last_our_pos) {
                immortal_map = loadImmortalHeroStats(db.get(), our_pos);
                last_our_pos = our_pos;
            }

            if (lp != last_lp) {
                last_lp = lp;
                renderToGui(guiState, lp, hero_map, md,
                            our_stats, our_stats_ranked, our_stats_pos,
                            immortal_map, model.get());
                requestRedraw();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }

    } catch (const std::exception& e) {
        LOG_ERR("[picker_gui] Error: " << e.what());
        if (guiState) {
            std::lock_guard<std::mutex> lk(guiState->mtx);
            guiState->active = false;
        }
        return 1;
    }

    if (guiState) {
        std::lock_guard<std::mutex> lk(guiState->mtx);
        guiState->active = false;
    }
    return 0;
}
