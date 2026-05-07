#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <chrono>
#include <thread>
#include <sqlite3.h>
#include <c_api.h>

static const char* UNKNOWN_HERO = "unknown";
static const char* UNKNOWN_POS  = "0";
static constexpr int MIN_GAMES_ON_POS  = 0;
static constexpr int TOP_N             = 10;
static constexpr int POLL_INTERVAL_MS  = 500;

struct ProStats {
    int games = 0;
    int wins  = 0;
    int bans  = 0;
};

struct PlayerStats {
    int games_all    = 0;
    int wins_all     = 0;
    int games_ranked = 0;
    int wins_ranked  = 0;
};

struct ImmortalHeroStats {
    int games = 0;
    int wins  = 0;
};

struct SlotData {
    int hero_id  = 0;
    int position = 0;
};

struct LivePick {
    int       match_id       = 0;
    int       our_account_id = 0;
    int       our_side       = 0;
    int       our_slot       = 0;
    long long updated_at     = 0;
    SlotData  r[5];
    SlotData  d[5];

    bool operator==(const LivePick& o) const {
        if (match_id != o.match_id || updated_at != o.updated_at) return false;
        for (int i = 0; i < 5; ++i) {
            if (r[i].hero_id != o.r[i].hero_id || r[i].position != o.r[i].position) return false;
            if (d[i].hero_id != o.d[i].hero_id || d[i].position != o.d[i].position) return false;
        }
        return true;
    }
    bool operator!=(const LivePick& o) const { return !(*this == o); }
};

static constexpr int N_CAT   = 20;
static constexpr int N_FLOAT = 70;

struct FeatureVector {
    std::string cat_str[N_CAT];
    const char* cat[N_CAT];
    float       flt[N_FLOAT];

    void finalize() {
        for (int i = 0; i < N_CAT; ++i)
            cat[i] = cat_str[i].c_str();
    }
};

void buildVector(
    FeatureVector& v,
    const LivePick& lp,
    const std::map<int, std::string>& hero_map,
    const std::map<std::pair<int,int>, ProStats>& pro_map,
    const std::map<int, PlayerStats>& our_stats,
    int candidate_hero_id
) {
    int our_r = (lp.our_side == 1) ? (lp.our_slot - 1) : -1;
    int our_d = (lp.our_side == 0) ? (lp.our_slot - 1) : -1;

    auto hero_name = [&](int hid) -> std::string {
        if (hid == 0) return UNKNOWN_HERO;
        auto it = hero_map.find(hid);
        return it != hero_map.end() ? it->second : UNKNOWN_HERO;
    };
    auto pos_str = [](int pos) -> std::string {
        return (pos >= 1 && pos <= 6) ? std::to_string(pos) : UNKNOWN_POS;
    };
    auto get_pro = [&](int hid, int pos) -> ProStats {
        if (!hid) return {};
        auto it = pro_map.find({hid, pos});
        return it != pro_map.end() ? it->second : ProStats{};
    };
    auto get_pl = [&](int hid, bool is_our) -> PlayerStats {
        if (!is_our || !hid) return {};
        auto it = our_stats.find(hid);
        return it != our_stats.end() ? it->second : PlayerStats{};
    };

    for (int i = 0; i < 5; ++i) {
        int hid = lp.r[i].hero_id;
        if (i == our_r && candidate_hero_id) hid = candidate_hero_id;
        v.cat_str[i]      = hero_name(hid);
        v.cat_str[10 + i] = pos_str(lp.r[i].position);
    }
    for (int i = 0; i < 5; ++i) {
        int hid = lp.d[i].hero_id;
        if (i == our_d && candidate_hero_id) hid = candidate_hero_id;
        v.cat_str[5 + i]  = hero_name(hid);
        v.cat_str[15 + i] = pos_str(lp.d[i].position);
    }

    static constexpr float PRIOR_PLAYER = 20.0f;
    static constexpr float PRIOR_PRO    = 50.0f;
    auto swr = [](int games, int wins, float prior) -> float {
        return ((float)wins + prior * 0.5f) / ((float)games + prior);
    };

    int fi = 0;
    for (int i = 0; i < 5; ++i) {
        int hid = lp.r[i].hero_id;
        if (i == our_r && candidate_hero_id) hid = candidate_hero_id;
        int pos_r = lp.r[i].position;
        if (i == our_r && candidate_hero_id) pos_r = lp.r[i].position;
        auto ps = get_pro(hid, pos_r);
        auto pl = get_pl(hid, i == our_r);
        v.flt[fi++] = swr(pl.games_all,    pl.wins_all,    PRIOR_PLAYER);
        v.flt[fi++] = (float)pl.games_all;
        v.flt[fi++] = swr(pl.games_ranked, pl.wins_ranked, PRIOR_PLAYER);
        v.flt[fi++] = (float)pl.games_ranked;
        v.flt[fi++] = swr(ps.games,        ps.wins,        PRIOR_PRO);
        v.flt[fi++] = (float)ps.games;
        v.flt[fi++] = (float)ps.bans;
    }
    for (int i = 0; i < 5; ++i) {
        int hid = lp.d[i].hero_id;
        if (i == our_d && candidate_hero_id) hid = candidate_hero_id;
        int pos_d = lp.d[i].position;
        if (i == our_d && candidate_hero_id) pos_d = lp.d[i].position;
        auto ps = get_pro(hid, pos_d);
        auto pl = get_pl(hid, i == our_d);
        v.flt[fi++] = swr(pl.games_all,    pl.wins_all,    PRIOR_PLAYER);
        v.flt[fi++] = (float)pl.games_all;
        v.flt[fi++] = swr(pl.games_ranked, pl.wins_ranked, PRIOR_PLAYER);
        v.flt[fi++] = (float)pl.games_ranked;
        v.flt[fi++] = swr(ps.games,        ps.wins,        PRIOR_PRO);
        v.flt[fi++] = (float)ps.games;
        v.flt[fi++] = (float)ps.bans;
    }
    v.finalize();
}

class DB {
    sqlite3* db_ = nullptr;
public:
    explicit DB(const char* path) {
        if (sqlite3_open_v2(path, &db_,
            SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("SQLite open: ") + sqlite3_errmsg(db_));
    }
    ~DB() { if (db_) sqlite3_close(db_); }
    sqlite3* get() { return db_; }
};

class Stmt {
    sqlite3_stmt* s_ = nullptr;
public:
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("SQL prepare: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { if (s_) sqlite3_finalize(s_); }
    sqlite3_stmt* get() { return s_; }
    int  step()           { return sqlite3_step(s_); }
    bool row()            { return step() == SQLITE_ROW; }
    void bind_int(int i, int v)       { sqlite3_bind_int(s_, i, v); }
    int         col_int (int i) { return sqlite3_column_int(s_, i); }
    long long   col_int64(int i){ return sqlite3_column_int64(s_, i); }
    std::string col_text(int i) {
        auto* t = sqlite3_column_text(s_, i);
        return t ? (const char*)t : "";
    }
    bool col_null(int i) { return sqlite3_column_type(s_, i) == SQLITE_NULL; }
};

std::map<int, std::string> loadHeroes(sqlite3* db) {
    std::map<int, std::string> m;
    Stmt st(db, "SELECT id, localized_name FROM heroes");
    while (st.row()) m[st.col_int(0)] = st.col_text(1);
    return m;
}

std::map<std::pair<int,int>, ProStats> loadProStats(sqlite3* db) {
    std::map<std::pair<int,int>, ProStats> m;
    Stmt st(db, "SELECT hero_id, pos, games, wins, bans FROM proherostats");
    while (st.row()) {
        int hid = st.col_int(0);
        int pos = st.col_int(1);
        m[{hid, pos}] = { st.col_int(2), st.col_int(3), st.col_int(4) };
    }
    return m;
}

static constexpr int MIN_IMMORTAL_GAMES = 1000;

std::map<int, ImmortalHeroStats> loadImmortalHeroStats(sqlite3* db, int position) {
    std::map<int, ImmortalHeroStats> m;
    if (position <= 0 || position > 5) return m;
    Stmt st(db,
        "SELECT hero_id, games, wins FROM immortalherostats "
        "WHERE pos=? AND games>=? ORDER BY games DESC");
    st.bind_int(1, position);
    sqlite3_bind_int(st.get(), 2, MIN_IMMORTAL_GAMES);
    while (st.row())
        m[st.col_int(0)] = { st.col_int(1), st.col_int(2) };
    return m;
}

std::map<int, PlayerStats> loadPlayerStats(sqlite3* db, int account_id) {
    std::map<int, PlayerStats> m;
    {
        Stmt st(db, "SELECT hero_id, games, wins FROM playerheroes WHERE account_id=?");
        st.bind_int(1, account_id);
        while (st.row()) {
            int hid = st.col_int(0);
            m[hid].games_all = st.col_int(1);
            m[hid].wins_all  = st.col_int(2);
        }
    }
    {
        Stmt st(db, "SELECT hero_id, games, wins FROM playerheroesranked WHERE account_id=?");
        st.bind_int(1, account_id);
        while (st.row()) {
            int hid = st.col_int(0);
            m[hid].games_ranked = st.col_int(1);
            m[hid].wins_ranked  = st.col_int(2);
        }
    }
    return m;
}

bool loadLatestLivePick(sqlite3* db, LivePick& lp) {
    Stmt st(db, R"(
        SELECT match_id, our_account_id, our_side, our_slot, updated_at,
               r1_hero, r1_pos, r2_hero, r2_pos, r3_hero, r3_pos,
               r4_hero, r4_pos, r5_hero, r5_pos,
               d1_hero, d1_pos, d2_hero, d2_pos, d3_hero, d3_pos,
               d4_hero, d4_pos, d5_hero, d5_pos
        FROM livepicks ORDER BY updated_at DESC LIMIT 1
    )");
    if (!st.row()) return false;

    lp.match_id       = st.col_int(0);
    lp.our_account_id = st.col_int(1);
    lp.our_side       = st.col_int(2);
    lp.our_slot       = st.col_int(3);
    lp.updated_at     = st.col_int64(4);

    for (int i = 0; i < 5; ++i) {
        lp.r[i].hero_id  = st.col_null(5 + i*2)   ? 0 : st.col_int(5 + i*2);
        lp.r[i].position = st.col_null(5 + i*2+1) ? 0 : st.col_int(5 + i*2+1);
    }
    for (int i = 0; i < 5; ++i) {
        lp.d[i].hero_id  = st.col_null(15 + i*2)   ? 0 : st.col_int(15 + i*2);
        lp.d[i].position = st.col_null(15 + i*2+1) ? 0 : st.col_int(15 + i*2+1);
    }
    return true;
}

static inline double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }

std::vector<double> runBatch(ModelCalcerHandle* model, std::vector<FeatureVector>& batch) {
    size_t n = batch.size();
    std::vector<double>      result(n, 0.0);
    std::vector<const float*> fp(n);
    std::vector<const char**> cp(n);
    for (size_t i = 0; i < n; ++i) { fp[i] = batch[i].flt; cp[i] = batch[i].cat; }

    if (!CalcModelPrediction(model, n, fp.data(), N_FLOAT, cp.data(), N_CAT, result.data(), n))
        throw std::runtime_error(std::string("Inference failed: ") + GetErrorString());

    for (double& v : result) v = sigmoid(v);
    return result;
}

struct StageModels {
    ModelCalcerHandle* early = nullptr;
    ModelCalcerHandle* mid   = nullptr;
    ModelCalcerHandle* late  = nullptr;

    ~StageModels() {
        if (early) ModelCalcerDelete(early);
        if (mid)   ModelCalcerDelete(mid);
        if (late)  ModelCalcerDelete(late);
    }
};

ModelCalcerHandle* selectModel(const StageModels& m, const LivePick& lp) {
    int known = 0;
    for (int i = 0; i < 5; ++i) {
        if (lp.r[i].hero_id) ++known;
        if (lp.d[i].hero_id) ++known;
    }
    if (known <= 4) return m.early;
    if (known <= 7) return m.mid;
    return m.late;
}

const char* stageName(const LivePick& lp) {
    int known = 0;
    for (int i = 0; i < 5; ++i) {
        if (lp.r[i].hero_id) ++known;
        if (lp.d[i].hero_id) ++known;
    }
    if (known <= 4) return "early";
    if (known <= 7) return "mid";
    return "late";
}

void render(
    const LivePick& lp,
    const std::map<int, std::string>& hero_map,
    const std::map<std::pair<int,int>, ProStats>& pro_map,
    const std::map<int, PlayerStats>& our_stats,
    const std::map<int, ImmortalHeroStats>& immortal_map,
    const StageModels& models
) {
    ModelCalcerHandle* model = selectModel(models, lp);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD  home = {0, 0};
    DWORD  written;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    DWORD size = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacterA(hOut, ' ', size, home, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, size, home, &written);
    SetConsoleCursorPosition(hOut, home);

    auto hname = [&](int hid) -> std::string {
        if (!hid) return "?";
        auto it = hero_map.find(hid);
        return it != hero_map.end() ? it->second : "unknown";
    };

    int our_r = (lp.our_side == 1) ? (lp.our_slot - 1) : -1;
    int our_d = (lp.our_side == 0) ? (lp.our_slot - 1) : -1;

    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║           DOTA 2 DRAFT HELPER                   ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";
    std::cout << "  Match: " << lp.match_id
              << "   Side: " << (lp.our_side ? "Radiant" : "Dire")
              << "   Slot: " << lp.our_slot
              << "   Stage: " << stageName(lp) << "\n";
    if (lp.our_account_id != 0)
        std::cout << "  Account: " << lp.our_account_id << "\n\n";
    else
        std::cout << "  Account: unknown (no player stats)\n\n";

    std::cout << "  ┌─────────────────────────────────────────────┐\n";
    std::cout << "  │  RADIANT                                     │\n";
    for (int i = 0; i < 5; ++i) {
        bool is_our = (i == our_r);
        std::string h  = hname(lp.r[i].hero_id);
        std::string pos = lp.r[i].position > 0
            ? "pos " + std::to_string(lp.r[i].position)
            : "pos ?";
        std::cout << "  │  R" << (i+1) << " [" << pos << "]  ";
        std::cout << std::left << std::setw(22) << h;
        if (is_our && lp.r[i].hero_id == 0)
            std::cout << " ← PICK HERE";
        else if (is_our)
            std::cout << " ← OUR HERO";
        std::cout << "\n";
    }
    std::cout << "  ├─────────────────────────────────────────────┤\n";
    std::cout << "  │  DIRE                                        │\n";
    for (int i = 0; i < 5; ++i) {
        bool is_our = (i == our_d);
        std::string h  = hname(lp.d[i].hero_id);
        std::string pos = lp.d[i].position > 0
            ? "pos " + std::to_string(lp.d[i].position)
            : "pos ?";
        std::cout << "  │  D" << (i+1) << " [" << pos << "]  ";
        std::cout << std::left << std::setw(22) << h;
        if (is_our && lp.d[i].hero_id == 0)
            std::cout << " ← PICK HERE";
        else if (is_our)
            std::cout << " ← OUR HERO";
        std::cout << "\n";
    }
    std::cout << "  └─────────────────────────────────────────────┘\n\n";

    int our_hero = (our_r >= 0) ? lp.r[our_r].hero_id : 0;
    if (our_d >= 0) our_hero = lp.d[our_d].hero_id;

    if (our_hero != 0) {
        FeatureVector v;
        buildVector(v, lp, hero_map, pro_map, our_stats, 0);
        std::vector<FeatureVector> batch = {v};
        auto probs = runBatch(model, batch);
        double p = probs[0];

        std::cout << "  ┌─────────────────────────────────────────────┐\n";
        std::cout << "  │  P(RADIANT WIN)                              │\n";
        std::cout << "  │                                              │\n";

        std::string color;
        if      (p >= 0.60) color = "\033[32m";
        else if (p >= 0.50) color = "\033[33m";
        else                color = "\033[31m";

        std::cout << "  │    " << color
                  << std::fixed << std::setprecision(1) << (p * 100.0) << "%"
                  << "\033[0m"
                  << "  radiant win"
                  << "\n";
        std::cout << "  │    our hero: " << hname(our_hero) << "\n";
        std::cout << "  │    our side: " << (lp.our_side ? "Radiant" : "Dire") << "\n";
        std::cout << "  │                                              │\n";
        std::cout << "  └─────────────────────────────────────────────┘\n";

    } else {
        std::set<int> picked;
        for (int i = 0; i < 5; ++i) {
            if (lp.r[i].hero_id) picked.insert(lp.r[i].hero_id);
            if (lp.d[i].hero_id) picked.insert(lp.d[i].hero_id);
        }

        bool use_immortal_filter = !immortal_map.empty();
        std::vector<int> pool;
        for (auto& [hid, _] : hero_map) {
            if (picked.count(hid)) continue;
            if (use_immortal_filter && !immortal_map.count(hid)) continue;
            pool.push_back(hid);
        }

        std::vector<FeatureVector> batch(pool.size());
        for (size_t i = 0; i < pool.size(); ++i)
            buildVector(batch[i], lp, hero_map, pro_map, our_stats, pool[i]);

        std::cout << "\n  === INPUT VECTOR (first candidate: "
                  << hero_map.at(pool[0]) << ") ===\n";
        std::cout << "  CAT features (" << N_CAT << "):\n";
        const char* cat_names[] = {
            "r1_hero","r2_hero","r3_hero","r4_hero","r5_hero",
            "d1_hero","d2_hero","d3_hero","d4_hero","d5_hero",
            "r1_pos","r2_pos","r3_pos","r4_pos","r5_pos",
            "d1_pos","d2_pos","d3_pos","d4_pos","d5_pos"
        };
        for (int i = 0; i < N_CAT; ++i)
            std::cout << "    " << std::left << std::setw(12) << cat_names[i]
                      << " = " << batch[0].cat[i] << "\n";
        const char* num_names[] = {
            "wr_all","games_all","wr_ranked","games_ranked",
            "pro_wr","pro_games","pro_bans"
        };
        std::cout << "  NUM features (" << N_FLOAT << "):\n";
        const char* slots[] = {"r1","r2","r3","r4","r5","d1","d2","d3","d4","d5"};
        for (int s = 0; s < 10; ++s)
            for (int f = 0; f < 7; ++f)
                std::cout << "    " << std::left << std::setw(12)
                          << (std::string(slots[s]) + "_" + num_names[f])
                          << " = " << batch[0].flt[s*7+f] << "\n";
        std::cout << "\n";

        auto probs = runBatch(model, batch);
        bool we_are_radiant = (lp.our_side == 1);

        std::vector<std::pair<double, int>> ranked;
        ranked.reserve(pool.size());
        for (size_t i = 0; i < pool.size(); ++i)
            ranked.push_back({probs[i], pool[i]});
        if (we_are_radiant)
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto& a, const auto& b){ return a.first > b.first; });
        else
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto& a, const auto& b){ return a.first < b.first; });

        std::cout << "  ┌─────────────────────────────────────────────┐\n";
        std::cout << "  │  TOP " << TOP_N
                  << (lp.our_side ? " RECOMMENDATIONS  [radiant, pick high P(rad_win)]  "
                                  : " RECOMMENDATIONS  [dire, pick low P(rad_win)]     ")
                  << "│\n";
        std::cout << "  ├─────────────────────────────────────────────┤\n";

        int shown = 0;
        for (auto& [prob, hid] : ranked) {
            if (shown >= TOP_N) break;
            auto& h  = hero_map.at(hid);
            int   our_pos_for_display = (our_r >= 0) ? lp.r[our_r].position : (our_d >= 0 ? lp.d[our_d].position : 0);
            auto  ps = pro_map.count({hid, our_pos_for_display}) ? pro_map.at({hid, our_pos_for_display}) : ProStats{};
            auto  pl = our_stats.count(hid) ? our_stats.at(hid) : PlayerStats{};

            std::string col;
            if (we_are_radiant) {
                if      (prob >= 0.60) col = "\033[32m";
                else if (prob >= 0.52) col = "\033[33m";
                else                   col = "\033[0m";
            } else {
                if      (prob <= 0.40) col = "\033[32m";
                else if (prob <= 0.48) col = "\033[33m";
                else                   col = "\033[0m";
            }

            std::cout << "  │  #" << std::setw(2) << (shown+1) << "  "
                      << col
                      << std::left << std::setw(22) << h
                      << "\033[0m"
                      << "  P(rad_win)=" << std::fixed << std::setprecision(1)
                      << (prob * 100.0) << "%";

            if (pl.games_all > 0)
                std::cout << "  you:" << pl.games_all << "g "
                          << std::setprecision(0)
                          << (100.0 * pl.wins_all / pl.games_all) << "%";

            if (ps.games > 0)
                std::cout << "  pro:" << ps.games << "g";

            auto imm_it = immortal_map.find(hid);
            if (imm_it != immortal_map.end()) {
                double imm_wr = imm_it->second.games > 0
                    ? 100.0 * imm_it->second.wins / imm_it->second.games : 0.0;
                std::cout << "  imm:" << imm_it->second.games << "g "
                          << std::setprecision(0) << imm_wr << "%";
            }

            std::cout << "\n";
            ++shown;
        }
        std::cout << "  └─────────────────────────────────────────────┘\n";
    }

    std::cout << "\n  [Watching for changes... Ctrl+C to exit]\n";
    std::cout.flush();
}

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    const char* model_path = "draft_helper_v3";
    const char* db_path    = "playerandlivestats.db";
    if (argc >= 2) model_path = argv[1];
    if (argc >= 3) db_path    = argv[2];

    try {
        StageModels models;
        auto loadModel = [](const std::string& path) -> ModelCalcerHandle* {
            ModelCalcerHandle* m = ModelCalcerCreate();
            if (!LoadFullModelFromFile(m, path.c_str())) {
                ModelCalcerDelete(m);
                throw std::runtime_error("Cannot load model: " + path + " — " + GetErrorString());
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

        std::cout << "Models: " << base << "_{early,mid,late}.cbm\n"
                  << "Heroes: " << hero_map.size()
                  << "  Pro stats: " << pro_map.size() << "\n";
        std::cout << "Watching: " << db_path << "\n";
        std::cout << "Press Ctrl+C to exit.\n\n";

        LivePick last_lp;
        last_lp.match_id = -1;
        std::map<int, PlayerStats>      our_stats;
        std::map<int, ImmortalHeroStats> immortal_map;
        int last_account_id = -1;
        int last_our_pos    = -1;

        while (true) {
            LivePick lp;
            bool ok = false;
            try {
                ok = loadLatestLivePick(db.get(), lp);
            } catch (...) {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
                continue;
            }

            if (!ok) {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
                continue;
            }

            if (lp.our_account_id != last_account_id) {
                if (lp.our_account_id != 0)
                    our_stats = loadPlayerStats(db.get(), lp.our_account_id);
                else
                    our_stats.clear();
                last_account_id = lp.our_account_id;
            }

            int our_r = (lp.our_side == 1) ? (lp.our_slot - 1) : -1;
            int our_d = (lp.our_side == 0) ? (lp.our_slot - 1) : -1;
            int our_pos = 0;
            if (our_r >= 0) our_pos = lp.r[our_r].position;
            if (our_d >= 0) our_pos = lp.d[our_d].position;

            if (our_pos != last_our_pos) {
                immortal_map = loadImmortalHeroStats(db.get(), our_pos);
                last_our_pos = our_pos;
            }

            if (lp != last_lp) {
                last_lp = lp;
                render(lp, hero_map, pro_map, our_stats, immortal_map, models);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }

    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
