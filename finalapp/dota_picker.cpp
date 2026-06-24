/*
 * dota_picker.cpp — ML-пикер: CatBoost инференс + рекомендации героев.
 *
 * Модель: draft_helper_abstract.cbm (одна модель, 10 cat + 42 float).
 * Фичи: 10 hero names, 30 matchup (global_wr/avg_vs/avg_with),
 *        2 mastery (target_hero_wr/games), 10 hero_pos_wr.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "shared_types.h"

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

// ─── Константы ───────────────────────────────────────────────────────────────

static const char* UNKNOWN_HERO      = "unknown";
static constexpr int TOP_N           = 10;
static constexpr int POLL_INTERVAL_MS = 500;

static constexpr float MASTERY_PRIOR = 30.0f;

// ─── Внутренние структуры ────────────────────────────────────────────────────

struct PlayerStats {
    int games=0, wins=0;
};
struct ImmortalHeroStats {
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
};

// ─── Вектор признаков для CatBoost ───────────────────────────────────────────

static constexpr int N_CAT=10, N_FLOAT=42;
struct FeatureVector {
    std::string cat_str[N_CAT];
    const char* cat[N_CAT];
    float flt[N_FLOAT];
    void finalize() { for (int i=0;i<N_CAT;++i) cat[i]=cat_str[i].c_str(); }
};

static void buildVector(FeatureVector& v, const LivePick& lp,
    const std::map<int,std::string>& hero_map,
    const MatchupData& md,
    const std::map<int,PlayerStats>& our_stats,
    int candidate_hero_id)
{
    int our_r = (lp.our_side==1) ? (lp.our_slot-1) : -1;
    int our_d = (lp.our_side==0) ? (lp.our_slot-1) : -1;

    auto hero_name = [&](int hid) -> std::string {
        if (!hid) return UNKNOWN_HERO;
        auto it = hero_map.find(hid);
        return it != hero_map.end() ? it->second : UNKNOWN_HERO;
    };

    // Собираем hero_id для всех 10 слотов (с подстановкой кандидата)
    int ids[10] = {};
    for (int i = 0; i < 5; ++i) {
        ids[i]   = lp.r[i].hero_id;
        ids[5+i] = lp.d[i].hero_id;
    }
    if (candidate_hero_id) {
        if (our_r >= 0 && our_r < 5) ids[our_r]   = candidate_hero_id;
        if (our_d >= 0 && our_d < 5) ids[5+our_d]  = candidate_hero_id;
    }

    // Позиции: своя команда = из livepicks, враг = modal
    int positions[10] = {};
    for (int i = 0; i < 5; ++i) {
        positions[i]   = lp.r[i].position;
        positions[5+i] = lp.d[i].position;
    }
    // Для вражеских слотов без позиции → modal
    int enemyBase = (lp.our_side == 1) ? 5 : 0;
    for (int i = enemyBase; i < enemyBase + 5; ++i) {
        if (positions[i] <= 0 && ids[i] > 0) {
            auto it = md.modal_pos.find(ids[i]);
            positions[i] = (it != md.modal_pos.end()) ? it->second : 0;
        }
    }

    // ── 10 categorical: hero names ──
    for (int i = 0; i < 10; ++i)
        v.cat_str[i] = hero_name(ids[i]);

    // ── 30 matchup floats (3 per slot) ──
    int fi = 0;
    for (int s = 0; s < 10; ++s) {
        int hid = ids[s];
        bool isRadSlot = (s < 5);

        // global_wr
        float gwr = 0.5f;
        if (hid) {
            auto it = md.global_wr.find(hid);
            if (it != md.global_wr.end()) gwr = it->second;
        }
        v.flt[fi++] = hid ? gwr : 0.5f;

        // avg_vs: среднее vs_wr против раскрытых врагов
        float avg_vs = 0.5f;
        if (hid) {
            int enemyStart = isRadSlot ? 5 : 0;
            float sum = 0.f; int cnt = 0;
            for (int e = enemyStart; e < enemyStart + 5; ++e) {
                if (!ids[e]) continue;
                auto it = md.vs_wr.find({hid, ids[e]});
                if (it != md.vs_wr.end()) { sum += it->second; ++cnt; }
            }
            if (cnt > 0) avg_vs = sum / cnt;
        }
        v.flt[fi++] = hid ? avg_vs : 0.5f;

        // avg_with: среднее with_wr с раскрытыми союзниками
        float avg_with = 0.5f;
        if (hid) {
            int allyStart = isRadSlot ? 0 : 5;
            float sum = 0.f; int cnt = 0;
            for (int a = allyStart; a < allyStart + 5; ++a) {
                if (a == s || !ids[a]) continue;
                auto it = md.with_wr.find({hid, ids[a]});
                if (it != md.with_wr.end()) { sum += it->second; ++cnt; }
            }
            if (cnt > 0) avg_with = sum / cnt;
        }
        v.flt[fi++] = hid ? avg_with : 0.5f;
    }

    // ── 2 mastery: target hero ──
    int target_hid = 0;
    if (our_r >= 0 && our_r < 5) target_hid = ids[our_r];
    if (our_d >= 0 && our_d < 5) target_hid = ids[5+our_d];

    float mastery_wr = 0.5f;
    float mastery_games = 0.f;
    if (target_hid) {
        auto it = our_stats.find(target_hid);
        if (it != our_stats.end()) {
            float g = (float)it->second.games;
            float w = (float)it->second.wins;
            mastery_wr    = (w + MASTERY_PRIOR * 0.5f) / (g + MASTERY_PRIOR);
            mastery_games = std::log1p(g);
        }
    }
    v.flt[fi++] = mastery_wr;
    v.flt[fi++] = mastery_games;

    // ── 10 hero_pos_wr ──
    for (int s = 0; s < 10; ++s) {
        int hid = ids[s];
        int pos = positions[s];
        float hpwr = 0.5f;
        if (hid && pos > 0) {
            auto it = md.hero_pos_wr.find({hid, pos});
            if (it != md.hero_pos_wr.end()) hpwr = it->second;
        }
        v.flt[fi++] = hid ? hpwr : 0.5f;
    }

    v.finalize();
}

// ─── SQLite-обёртки (readonly) ────────────────────────────────────────────────

class DB {
    sqlite3* db_=nullptr;
public:
    explicit DB(const char* path) {
        if (sqlite3_open_v2(path,&db_,SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK)
            throw std::runtime_error(std::string("SQLite open: ")+sqlite3_errmsg(db_));
    }
    ~DB() { if (db_) sqlite3_close(db_); }
    sqlite3* get() { return db_; }
};
class Stmt {
    sqlite3_stmt* s_=nullptr;
public:
    Stmt(sqlite3* db,const char* sql) {
        if (sqlite3_prepare_v2(db,sql,-1,&s_,nullptr)!=SQLITE_OK)
            throw std::runtime_error(std::string("SQL prepare: ")+sqlite3_errmsg(db));
    }
    ~Stmt() { if (s_) sqlite3_finalize(s_); }
    sqlite3_stmt* get() { return s_; }
    bool row()            { return sqlite3_step(s_)==SQLITE_ROW; }
    void bind_int(int i,int v){ sqlite3_bind_int(s_,i,v); }
    int         col_int(int i)  { return sqlite3_column_int(s_,i); }
    long long   col_int64(int i){ return sqlite3_column_int64(s_,i); }
    std::string col_text(int i) { auto*t=sqlite3_column_text(s_,i); return t?(const char*)t:""; }
    bool col_null(int i){ return sqlite3_column_type(s_,i)==SQLITE_NULL; }
    double col_double(int i){ return sqlite3_column_double(s_,i); }
};

// ─── Загрузка данных из SQLite ────────────────────────────────────────────────

static std::map<int,std::string> loadHeroes(sqlite3* db) {
    std::map<int,std::string> m;
    Stmt st(db,"SELECT id,localized_name FROM heroes");
    while (st.row()) m[st.col_int(0)]=st.col_text(1);
    return m;
}

static MatchupData loadMatchupData(sqlite3* db) {
    MatchupData md;

    try {
        Stmt st(db,"SELECT hero_id,wr FROM global_wr");
        while (st.row()) md.global_wr[st.col_int(0)] = (float)st.col_double(1);
    } catch (...) { std::fprintf(stderr,"[picker] global_wr not found\n"); }

    try {
        Stmt st(db,"SELECT hero_id,opp_hero_id,wr FROM vs_wr");
        while (st.row()) md.vs_wr[{st.col_int(0),st.col_int(1)}] = (float)st.col_double(2);
    } catch (...) { std::fprintf(stderr,"[picker] vs_wr not found\n"); }

    try {
        Stmt st(db,"SELECT hero_id,ally_hero_id,wr FROM with_wr");
        while (st.row()) md.with_wr[{st.col_int(0),st.col_int(1)}] = (float)st.col_double(2);
    } catch (...) { std::fprintf(stderr,"[picker] with_wr not found\n"); }

    try {
        Stmt st(db,"SELECT hero_id,pos FROM modal_pos");
        while (st.row()) md.modal_pos[st.col_int(0)] = st.col_int(1);
    } catch (...) { std::fprintf(stderr,"[picker] modal_pos not found\n"); }

    try {
        Stmt st(db,"SELECT hero_id,pos,wr FROM hero_pos_wr");
        while (st.row()) md.hero_pos_wr[{st.col_int(0),st.col_int(1)}] = (float)st.col_double(2);
    } catch (...) { std::fprintf(stderr,"[picker] hero_pos_wr not found\n"); }

    std::printf("[picker] Matchup data: gwr=%zu vs=%zu with=%zu modal=%zu hpwr=%zu\n",
        md.global_wr.size(), md.vs_wr.size(), md.with_wr.size(),
        md.modal_pos.size(), md.hero_pos_wr.size());
    return md;
}

static constexpr int MIN_IMMORTAL_GAMES = 1000;
static std::map<int,ImmortalHeroStats> loadImmortalHeroStats(sqlite3* dataDb, int position) {
    std::map<int,ImmortalHeroStats> m;
    if (position <= 0 || position > 5) return m;
    try {
        Stmt st(dataDb,"SELECT hero_id,games,wins FROM immortalherostats WHERE pos=? AND games>=? ORDER BY games DESC");
        st.bind_int(1,position); sqlite3_bind_int(st.get(),2,MIN_IMMORTAL_GAMES);
        while (st.row()) m[st.col_int(0)] = {st.col_int(1), st.col_int(2)};
    } catch (...) {}
    return m;
}

static std::map<int,PlayerStats> loadPlayerStats(sqlite3* db, int account_id) {
    std::map<int,PlayerStats> m;
    try {
        Stmt st(db,"SELECT hero_id,games,wins FROM playerheroes WHERE account_id=?");
        st.bind_int(1,account_id);
        while (st.row()) m[st.col_int(0)] = {st.col_int(1), st.col_int(2)};
    } catch (...) {}
    return m;
}

static bool loadLatestLivePick(sqlite3* db, LivePick& lp) {
    Stmt st(db,R"(
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

// ─── CatBoost инференс ───────────────────────────────────────────────────────

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

// ─── GUI режим: результаты → GuiPickerState ─────────────────────────────────

static void renderToGui(
    GuiPickerState*                        state,
    const LivePick&                        lp,
    const std::map<int,std::string>&       hero_map,
    const MatchupData&                     md,
    const std::map<int,PlayerStats>&       our_stats,
    const std::map<int,ImmortalHeroStats>& immortal_map,
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
        buildVector(v, lp, hero_map, md, our_stats, 0);
        std::vector<FeatureVector> batch = {v};
        auto probs = runBatch(model, batch);
        state->winProb = flipProb ? 1.f - (float)probs[0] : (float)probs[0];
        auto on = hname(our_hero);
        std::snprintf(state->ourHeroName, sizeof(state->ourHeroName), "%s", on.c_str());
        state->recCount = 0;

    } else {
        {
            FeatureVector v0;
            buildVector(v0, lp, hero_map, md, our_stats, 0);
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
                buildVector(batch[i], lp, hero_map, md, our_stats, pool[i]);
            auto probs = runBatch(model, batch);

            bool rad = (lp.our_side == 1);
            std::vector<std::pair<double,int>> ranked;
            ranked.reserve(pool.size());
            for (size_t i = 0; i < pool.size(); i++)
                ranked.push_back({probs[i], pool[i]});
            if (rad)
                std::sort(ranked.begin(), ranked.end(),
                    [](auto& a, auto& b){ return a.first > b.first; });
            else
                std::sort(ranked.begin(), ranked.end(),
                    [](auto& a, auto& b){ return a.first < b.first; });

            int n = (int)std::min(ranked.size(), (size_t)10);
            state->recCount = n;
            int our_pos = state->ourPosition;

            for (int i = 0; i < n; i++) {
                auto& [prob, hid] = ranked[i];
                PickRowGui& r = state->recs[i];
                r.rank    = i + 1;
                r.heroId  = hid;
                r.winProb = flipProb ? 1.f - (float)prob : (float)prob;
                auto hn   = hname(hid);
                std::snprintf(r.name, sizeof(r.name), "%s", hn.c_str());

                auto pl  = our_stats.count(hid)   ? our_stats.at(hid)   : PlayerStats{};
                auto imm = immortal_map.count(hid) ? immortal_map.at(hid) : ImmortalHeroStats{};

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

// ─── Главный цикл пикера ─────────────────────────────────────────────────────

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
                                         + " — " + GetErrorString());
            }
            return m;
        };
        std::string base(model_path);
        ModelCalcerHandle* model = loadModel(base + ".cbm");

        DB db(db_path);
        auto hero_map = loadHeroes(db.get());

        std::string dataDbPath = std::string(model_path) + "_data.db";
        DB dataDb(dataDbPath.c_str());
        auto md = loadMatchupData(dataDb.get());

        if (guiState) {
            std::lock_guard<std::mutex> lk(guiState->mtx);
            guiState->active = true;
        }

        LivePick last_lp; last_lp.match_id = -1;
        std::map<int, PlayerStats>       our_stats;
        std::map<int, ImmortalHeroStats>  immortal_map;
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
                last_account_id = lp.our_account_id;
            }

            int our_r = (lp.our_side == 1) ? (lp.our_slot - 1) : -1;
            int our_d = (lp.our_side == 0) ? (lp.our_slot - 1) : -1;
            int our_pos = 0;
            if (our_r >= 0 && our_r < 5) our_pos = lp.r[our_r].position;
            if (our_d >= 0 && our_d < 5) our_pos = lp.d[our_d].position;

            if (our_pos != last_our_pos) {
                immortal_map = loadImmortalHeroStats(dataDb.get(), our_pos);
                last_our_pos = our_pos;
            }

            if (lp != last_lp) {
                last_lp = lp;
                renderToGui(guiState, lp, hero_map, md,
                            our_stats, immortal_map, model);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }

        ModelCalcerDelete(model);

    } catch (const std::exception& e) {
        std::fprintf(stderr, "[picker_gui] Error: %s\n", e.what());
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
