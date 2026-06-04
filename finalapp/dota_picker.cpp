/*
 * dota_picker.cpp — GUI-режим пикера.
 * Экспортирует runPickerGui() → пишет результат в GuiPickerState.
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

// ─── Константы ────────────────────────────────────────────────────────────────

static const char* UNKNOWN_HERO      = "unknown";
static const char* UNKNOWN_POS       = "0";
static constexpr int TOP_N           = 10;
static constexpr int POLL_INTERVAL_MS = 500;

// ─── Структуры данных ─────────────────────────────────────────────────────────

struct ProStats {
    int games = 0, wins = 0, bans = 0;
};
struct PlayerStats {
    int games_all=0, wins_all=0, games_ranked=0, wins_ranked=0;
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

// ─── Вектор признаков ─────────────────────────────────────────────────────────

static constexpr int N_CAT=20, N_FLOAT=70;
struct FeatureVector {
    std::string cat_str[N_CAT];
    const char* cat[N_CAT];
    float flt[N_FLOAT];
    void finalize() { for (int i=0;i<N_CAT;++i) cat[i]=cat_str[i].c_str(); }
};

void buildVector(FeatureVector& v, const LivePick& lp,
    const std::map<int,std::string>& hero_map,
    const std::map<std::pair<int,int>,ProStats>& pro_map,
    const std::map<int,PlayerStats>& our_stats,
    int candidate_hero_id)
{
    int our_r=(lp.our_side==1)?(lp.our_slot-1):-1;
    int our_d=(lp.our_side==0)?(lp.our_slot-1):-1;
    auto hero_name=[&](int hid)->std::string {
        if (!hid) return UNKNOWN_HERO;
        auto it=hero_map.find(hid);
        return it!=hero_map.end()?it->second:UNKNOWN_HERO;
    };
    auto pos_str=[](int pos)->std::string {
        return (pos>=1&&pos<=6)?std::to_string(pos):UNKNOWN_POS;
    };
    auto get_pro=[&](int hid,int pos)->ProStats {
        if (!hid) return {};
        auto it=pro_map.find({hid,pos});
        return it!=pro_map.end()?it->second:ProStats{};
    };
    auto get_pl=[&](int hid,bool is_our)->PlayerStats {
        if (!is_our||!hid) return {};
        auto it=our_stats.find(hid);
        return it!=our_stats.end()?it->second:PlayerStats{};
    };
    for (int i=0;i<5;++i) {
        int hid=lp.r[i].hero_id; if (i==our_r&&candidate_hero_id) hid=candidate_hero_id;
        v.cat_str[i]=hero_name(hid); v.cat_str[10+i]=pos_str(lp.r[i].position);
    }
    for (int i=0;i<5;++i) {
        int hid=lp.d[i].hero_id; if (i==our_d&&candidate_hero_id) hid=candidate_hero_id;
        v.cat_str[5+i]=hero_name(hid); v.cat_str[15+i]=pos_str(lp.d[i].position);
    }
    static constexpr float PRIOR_PLAYER=20.0f, PRIOR_PRO=50.0f;
    auto swr=[](int games,int wins,float prior)->float {
        return ((float)wins+prior*0.5f)/((float)games+prior);
    };
    int fi=0;
    for (int i=0;i<5;++i) {
        int hid=lp.r[i].hero_id; if (i==our_r&&candidate_hero_id) hid=candidate_hero_id;
        int pos_r=lp.r[i].position;
        auto ps=get_pro(hid,pos_r); auto pl=get_pl(hid,i==our_r);
        v.flt[fi++]=swr(pl.games_all,pl.wins_all,PRIOR_PLAYER);
        v.flt[fi++]=(float)pl.games_all;
        v.flt[fi++]=swr(pl.games_ranked,pl.wins_ranked,PRIOR_PLAYER);
        v.flt[fi++]=(float)pl.games_ranked;
        v.flt[fi++]=swr(ps.games,ps.wins,PRIOR_PRO);
        v.flt[fi++]=(float)ps.games;
        v.flt[fi++]=(float)ps.bans;
    }
    for (int i=0;i<5;++i) {
        int hid=lp.d[i].hero_id; if (i==our_d&&candidate_hero_id) hid=candidate_hero_id;
        int pos_d=lp.d[i].position;
        auto ps=get_pro(hid,pos_d); auto pl=get_pl(hid,i==our_d);
        v.flt[fi++]=swr(pl.games_all,pl.wins_all,PRIOR_PLAYER);
        v.flt[fi++]=(float)pl.games_all;
        v.flt[fi++]=swr(pl.games_ranked,pl.wins_ranked,PRIOR_PLAYER);
        v.flt[fi++]=(float)pl.games_ranked;
        v.flt[fi++]=swr(ps.games,ps.wins,PRIOR_PRO);
        v.flt[fi++]=(float)ps.games;
        v.flt[fi++]=(float)ps.bans;
    }
    v.finalize();
}

// ─── SQLite helpers ───────────────────────────────────────────────────────────

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
};

// ─── Загрузка данных ──────────────────────────────────────────────────────────

static std::map<int,std::string> loadHeroes(sqlite3* db) {
    std::map<int,std::string> m;
    Stmt st(db,"SELECT id,localized_name FROM heroes");
    while (st.row()) m[st.col_int(0)]=st.col_text(1);
    return m;
}
static std::map<std::pair<int,int>,ProStats> loadProStats(sqlite3* db) {
    std::map<std::pair<int,int>,ProStats> m;
    try {
        Stmt st(db,"SELECT hero_id,pos,games,wins,bans FROM proherostats");
        while (st.row()) {
            int hid=st.col_int(0),pos=st.col_int(1);
            m[{hid,pos}]={st.col_int(2),st.col_int(3),st.col_int(4)};
        }
    } catch (...) {
        std::fprintf(stderr,"[picker] proherostats не найдена, используем пустую статистику\n");
    }
    return m;
}
static constexpr int MIN_IMMORTAL_GAMES=1000;
static std::map<int,ImmortalHeroStats> loadImmortalHeroStats(sqlite3* db,int position) {
    std::map<int,ImmortalHeroStats> m;
    if (position<=0||position>5) return m;
    try {
        Stmt st(db,"SELECT hero_id,games,wins FROM immortalherostats WHERE pos=? AND games>=? ORDER BY games DESC");
        st.bind_int(1,position); sqlite3_bind_int(st.get(),2,MIN_IMMORTAL_GAMES);
        while (st.row()) m[st.col_int(0)]={st.col_int(1),st.col_int(2)};
    } catch (...) {
        std::fprintf(stderr,"[picker] immortalherostats не найдена, фильтрация пула отключена\n");
    }
    return m;
}
static std::map<int,PlayerStats> loadPlayerStats(sqlite3* db,int account_id) {
    std::map<int,PlayerStats> m;
    {
        Stmt st(db,"SELECT hero_id,games,wins FROM playerheroes WHERE account_id=?");
        st.bind_int(1,account_id);
        while (st.row()) { int hid=st.col_int(0); m[hid].games_all=st.col_int(1); m[hid].wins_all=st.col_int(2); }
    }
    {
        Stmt st(db,"SELECT hero_id,games,wins FROM playerheroesranked WHERE account_id=?");
        st.bind_int(1,account_id);
        while (st.row()) { int hid=st.col_int(0); m[hid].games_ranked=st.col_int(1); m[hid].wins_ranked=st.col_int(2); }
    }
    return m;
}
static bool loadLatestLivePick(sqlite3* db,LivePick& lp) {
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

// ─── CatBoost ─────────────────────────────────────────────────────────────────

static inline double sigmoid(double x){ return 1.0/(1.0+std::exp(-x)); }
static std::vector<double> runBatch(ModelCalcerHandle* model,std::vector<FeatureVector>& batch) {
    size_t n=batch.size();
    std::vector<double> result(n,0.0);
    std::vector<const float*> fp(n); std::vector<const char**> cp(n);
    for (size_t i=0;i<n;++i){ fp[i]=batch[i].flt; cp[i]=batch[i].cat; }
    if (!CalcModelPrediction(model,n,fp.data(),N_FLOAT,cp.data(),N_CAT,result.data(),n))
        throw std::runtime_error(std::string("Inference failed: ")+GetErrorString());
    for (double& v:result) v=sigmoid(v);
    return result;
}
struct StageModels {
    ModelCalcerHandle *early=nullptr,*mid=nullptr,*late=nullptr;
    ~StageModels(){
        if (early) ModelCalcerDelete(early);
        if (mid)   ModelCalcerDelete(mid);
        if (late)  ModelCalcerDelete(late);
    }
};
static ModelCalcerHandle* selectModel(const StageModels& m,const LivePick& lp) {
    int known=0;
    for (int i=0;i<5;++i){ if (lp.r[i].hero_id) ++known; if (lp.d[i].hero_id) ++known; }
    if (known<=4) return m.early;
    if (known<=7) return m.mid;
    return m.late;
}
// =============================================================================
//  GUI режим: renderToGui + runPickerGui
//  Вместо вывода в консоль — пишет в GuiPickerState (читает GUI-поток)
// =============================================================================

static void renderToGui(
    GuiPickerState*                              state,
    const LivePick&                              lp,
    const std::map<int,std::string>&             hero_map,
    const std::map<std::pair<int,int>,ProStats>& pro_map,
    const std::map<int,PlayerStats>&             our_stats,
    const std::map<int,ImmortalHeroStats>&       immortal_map,
    const StageModels&                           models)
{
    if (!state) return;

    auto hname = [&](int hid) -> std::string {
        if (!hid) return "";
        auto it = hero_map.find(hid);
        return it != hero_map.end() ? it->second : "";
    };

    // our_slot хранится как 1-based (от GSI team_slot+1).
    // our_side: 1=Radiant, 0=Dire.
    // our_r/our_d: 0-based индекс в массиве radiant[]/dire[].
    int our_r = -1, our_d = -1;
    if (lp.our_slot >= 1 && lp.our_slot <= 5) {
        int idx = lp.our_slot - 1;           // 0-4
        if (lp.our_side == 1) our_r = idx;   // Radiant
        else                   our_d = idx;   // Dire
    }

    ModelCalcerHandle* model = selectModel(models, lp);

    std::lock_guard<std::mutex> lk(state->mtx);

    state->isRadiant = (lp.our_side == 1);
    state->ourSlot   = lp.our_slot;

    // ── Hero slots ────────────────────────────────────────────────────────────
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

    // ── Our hero & position ───────────────────────────────────────────────────
    int our_hero = (our_r >= 0 && our_r < 5) ? lp.r[our_r].hero_id : 0;
    if (our_d >= 0 && our_d < 5) our_hero = lp.d[our_d].hero_id;

    state->ourHeroPicked = (our_hero != 0);
    state->ourPosition   = 0;
    if (our_r >= 0 && our_r < 5) state->ourPosition = lp.r[our_r].position;
    if (our_d >= 0 && our_d < 5) state->ourPosition = lp.d[our_d].position;

    // ── Win probability OR top-10 recs ───────────────────────────────────────
    if (our_hero != 0) {
        // Mode 1: show P(win) for current draft
        FeatureVector v;
        buildVector(v, lp, hero_map, pro_map, our_stats, 0);
        std::vector<FeatureVector> batch = {v};
        auto probs = runBatch(model, batch);
        state->winProb  = (float)probs[0];
        auto on = hname(our_hero);
        std::snprintf(state->ourHeroName, sizeof(state->ourHeroName), "%s", on.c_str());
        state->recCount = 0;

    } else {
        // Mode 2: рекомендации top-10
        // Сначала считаем P(win) для текущего частичного драфта (без нашего героя)
        {
            FeatureVector v0;
            buildVector(v0, lp, hero_map, pro_map, our_stats, 0);
            std::vector<FeatureVector> b0 = {v0};
            auto p0 = runBatch(model, b0);
            state->winProb = (float)p0[0];
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
                buildVector(batch[i], lp, hero_map, pro_map, our_stats, pool[i]);
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
                r.winProb = (float)prob;
                auto hn   = hname(hid);
                std::snprintf(r.name, sizeof(r.name), "%s", hn.c_str());

                auto pl  = our_stats.count(hid)        ? our_stats.at(hid)            : PlayerStats{};
                auto ps2 = pro_map.count({hid,our_pos}) ? pro_map.at({hid,our_pos})   : ProStats{};
                auto imm = immortal_map.count(hid)      ? immortal_map.at(hid)         : ImmortalHeroStats{};

                r.gamesPlayer = pl.games_all;
                r.wrPlayer    = pl.games_all > 0 ? (float)pl.wins_all / pl.games_all : 0.f;
                r.gamesPro    = ps2.games;
                r.gamesImm    = imm.games;
                r.wrImm       = imm.games > 0 ? (float)imm.wins / imm.games : 0.f;
            }
        }
    }

    state->gameStarted = true;
}

// ─── runPickerGui ─────────────────────────────────────────────────────────────

int runPickerGui(const char* model_path, const char* db_path,
                 std::atomic<bool>& running,
                 GuiPickerState* guiState,
                 SharedPortraitState* /*portraitState*/)
{
    try {
        StageModels models;
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
        models.early = loadModel(base + "_early.cbm");
        models.mid   = loadModel(base + "_mid.cbm");
        models.late  = loadModel(base + "_late.cbm");

        DB db(db_path);
        auto hero_map = loadHeroes(db.get());
        auto pro_map  = loadProStats(db.get());

        if (guiState) {
            std::lock_guard<std::mutex> lk(guiState->mtx);
            guiState->active = true;
        }

        LivePick last_lp; last_lp.match_id = -1;
        std::map<int, PlayerStats>       our_stats;
        std::map<int, ImmortalHeroStats> immortal_map;
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
                immortal_map = loadImmortalHeroStats(db.get(), our_pos);
                last_our_pos = our_pos;
            }

            if (lp != last_lp) {
                last_lp = lp;
                renderToGui(guiState, lp, hero_map, pro_map,
                            our_stats, immortal_map, models);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }

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