#pragma once
/*
 * shared_types.h — общие типы для межпоточного взаимодействия.
 * Используется всеми модулями: GSI, Portrait, Picker, GUI.
 */

#include <string>
#include <mutex>
#include <atomic>
#include <chrono>

// ─── Фаза игры ───────────────────────────────────────────────────────────────

enum class GamePhase { IDLE, DRAFT, INGAME, POSTGAME };

// Возвращает строковое имя фазы для логирования
inline const char* phaseName(GamePhase p) {
    switch (p) {
        case GamePhase::DRAFT:    return "DRAFT";
        case GamePhase::INGAME:   return "INGAME";
        case GamePhase::POSTGAME: return "POSTGAME";
        default:                  return "IDLE";
    }
}

// ─── Состояние матча (от GSI-сервера) ─────────────────────────────────────────

struct GameInfo {
    std::mutex  mtx;
    GamePhase   phase           = GamePhase::IDLE;
    std::string matchId;
    int         ourSide         = 1;   // 1 = Radiant, 0 = Dire
    int         ourSlot         = 1;   // 1-5
    bool        newMatch        = false;
    bool        isHeroSelection = false; // true только при HERO_SELECTION
    std::chrono::steady_clock::time_point lastUpdate;
};

// ─── Результат распознавания портрета ─────────────────────────────────────────

struct PortraitResult {
    std::string heroName;
    int         heroId = 0;
    float       score  = 0.0f;
    bool        valid() const { return heroId > 0 && score >= 0.5f; }
};

// 10 слотов портретов (0-4 Radiant, 5-9 Dire), mutex-protected
struct SharedPortraitState {
    std::mutex     mtx;
    PortraitResult slots[10];
    int            manualPos[10] = {};  // 0 = auto (screen capture), 1-5 = GUI override
    bool           active = false;
    void clear() {
        std::lock_guard<std::mutex> lk(mtx);
        for (auto& s : slots) s = {};
        for (auto& p : manualPos) p = 0;
        active = false;
    }
};

// ─── Состояние пикера для GUI ─────────────────────────────────────────────────
// Пишется потоком runPickerGui, читается GUI-потоком

struct HeroSlotGui {
    char name[64] = {};
    int  heroId   = 0;
    int  pos      = 0;   // позиция 1-5, 0 = неизвестна
    bool filled   = false;
    bool isYou    = false;
};

// Строка рекомендации: герой + вероятность победы + статистика
struct PickRowGui {
    char  name[64]  = {};
    int   heroId    = 0;
    float winProb   = 0.f;
    int   rank      = 0;
    int   gamesPlayer = 0;
    float wrPlayer  = 0.f;   // винрейт [0..1]
    int   gamesImm  = 0;
    float wrImm     = 0.f;
};

// Полное состояние пикера: слоты драфта + рекомендации top-10
struct GuiPickerState {
    std::mutex   mtx;

    bool         active       = false;  // поток пикера запущен
    bool         gameStarted  = false;  // получен DRAFT/INGAME

    HeroSlotGui  radiant[5];
    HeroSlotGui  dire[5];

    // Статус нашего героя
    bool  ourHeroPicked = false;
    char  ourHeroName[64] = {};
    int   ourHeroId       = 0;
    float winProb         = 0.f;
    int   ourPosition     = 0;   // позиция 1-5
    int   ourSlot         = 1;   // слот 1-5
    bool  isRadiant       = true;

    // Рекомендации top-10 (пока наш герой не выбран)
    PickRowGui   recs[10];
    int          recCount = 0;

    // Счётчик завершённых циклов инференса (атомарный, без мьютекса)
    std::atomic<int> inferenceGen{0};

    // Ошибка совместимости схемы данных
    bool schemaError = false;
    char schemaMsg[256] = {};

    void reset() {
        std::lock_guard<std::mutex> lk(mtx);
        gameStarted     = false;
        ourHeroPicked   = false;
        winProb         = 0.f;
        ourPosition     = 0;
        recCount        = 0;
        ourHeroName[0]  = '\0';
        ourHeroId       = 0;
        schemaError     = false;
        schemaMsg[0]    = '\0';
        for (auto& s : radiant) s = {};
        for (auto& s : dire)    s = {};
        for (auto& r : recs)    r = {};
    }
};

// ─── Декларации функций модулей ───────────────────────────────────────────────

// Фаза 1a: создание таблиц + справочник героев + живая мета-стата (не требует accountId).
int  runDataFetcherInit(const std::string& stratzToken);

// Фаза 1b: загрузка данных игрока. Возвращает 0 при успехе.
int  runDataFetcher(long long accountId, const std::string& stratzToken);

// Фаза 2: GSI HTTP-сервер на порту 62326 (localhost), обновляет gameInfo.
void runGsiServer(GameInfo& gameInfo);

// Захват портретов + позиций HUD каждые 500мс, запись в livepicks. Без accountId.
void runPortraitCapture(GameInfo& gameInfo, const std::string& dbPath,
                        std::atomic<bool>& running, SharedPortraitState& out);

// ML-пикер: читает livepicks + matchup data, пишет рекомендации в guiState. Требует accountId.
int  runPickerGui(const char* modelPath, const char* dbPath,
                  std::atomic<bool>& running,
                  GuiPickerState* guiState,
                  SharedPortraitState* portraitState);