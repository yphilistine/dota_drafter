#pragma once
/*
 * shared_types.h
 */

#include <string>
#include <mutex>
#include <atomic>

enum class GamePhase { IDLE, DRAFT, INGAME, POSTGAME };

inline const char* phaseName(GamePhase p) {
    switch (p) {
        case GamePhase::DRAFT:    return "DRAFT";
        case GamePhase::INGAME:   return "INGAME";
        case GamePhase::POSTGAME: return "POSTGAME";
        default:                  return "IDLE";
    }
}

struct GameInfo {
    std::mutex  mtx;
    GamePhase   phase           = GamePhase::IDLE;
    std::string matchId;
    int         ourSide         = 1;
    int         ourSlot         = 1;
    bool        newMatch        = false;
    // true только при HERO_SELECTION (не STRATEGY_TIME / TEAM_SHOWCASE)
    bool        isHeroSelection = false;
};

struct PortraitResult {
    std::string heroName;
    int         heroId = 0;
    float       score  = 0.0f;
    bool        valid() const { return heroId > 0 && score >= 0.5f; }
};

struct SharedPortraitState {
    std::mutex     mtx;
    PortraitResult slots[10];
    bool           active = false;
    void clear() {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& s : slots) s = {};
        active = false;
    }
};

// ─── GUI Picker State ─────────────────────────────────────────────────────────
// Written by runPickerGui (picker thread), read by GUI thread

struct HeroSlotGui {
    char name[64] = {};
    int  heroId   = 0;
    int  pos      = 0;   // 1-5, 0=unknown
    bool filled   = false;
    bool isYou    = false;
};

struct PickRowGui {
    char  name[64]  = {};
    int   heroId    = 0;
    float winProb   = 0.f;
    int   rank      = 0;
    int   gamesPlayer = 0;
    float wrPlayer  = 0.f;   // win rate [0..1]
    int   gamesPro  = 0;
    int   gamesImm  = 0;
    float wrImm     = 0.f;
};

struct GuiPickerState {
    std::mutex   mtx;

    bool         active       = false;  // picker thread is running
    bool         gameStarted  = false;  // received DRAFT/INGAME state

    HeroSlotGui  radiant[5];
    HeroSlotGui  dire[5];

    // Our hero status
    bool  ourHeroPicked = false;
    char  ourHeroName[64] = {};
    float winProb         = 0.f;
    int   ourPosition     = 0;   // detected position 1-5
    int   ourSlot         = 1;   // slot 1-5
    bool  isRadiant       = true;

    // Top-10 recommendations (when our hero not yet picked)
    PickRowGui   recs[10];
    int          recCount = 0;

    void reset() {
        std::lock_guard<std::mutex> lk(mtx);
        gameStarted     = false;
        ourHeroPicked   = false;
        winProb         = 0.f;
        ourPosition     = 0;
        recCount        = 0;
        ourHeroName[0]  = '\0';
        for (auto& s : radiant) s = {};
        for (auto& s : dire)    s = {};
        for (auto& r : recs)    r = {};
    }
};

// ─── Function declarations ────────────────────────────────────────────────────

int  runDataFetcher(long long accountId, const std::string& stratzToken);
void runGsiServer(GameInfo& gameInfo, const std::string& steamApiKey);
void runPortraitCapture(GameInfo& gameInfo, const std::string& dbPath,
                        std::atomic<bool>& running, SharedPortraitState& out);

// GUI picker — writes to GuiPickerState instead of console
int  runPickerGui(const char* modelPath, const char* dbPath,
                  std::atomic<bool>& running,
                  GuiPickerState* guiState,
                  SharedPortraitState* portraitState);