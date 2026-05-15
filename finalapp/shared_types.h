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

int  runDataFetcher(long long accountId, const std::string& stratzToken);
void runGsiServer(GameInfo& gameInfo, const std::string& steamApiKey);
void runPortraitCapture(GameInfo& gameInfo, const std::string& dbPath,
                        std::atomic<bool>& running, SharedPortraitState& out);
int  runPicker(const char* modelPath, const char* dbPath,
               std::atomic<bool>& running, SharedPortraitState* portraitState);