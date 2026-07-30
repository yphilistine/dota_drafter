/*
 * gui_draw.cpp - панели ImGui (Draft/Picks/Meta Heroes/Header/StatusBar),
 * кэш портретов героев/меты/имён.
 *
 * DrawPicksPanel и DrawMetaHeroesPanel используют общую DrawHeroStatRow -
 * тот же расчёт колонок/портрета/бара, отличается только источник вторичной
 * статистики.
 */

#include "gui_draw.h"
#include "orchestrator.h"
#include "common.h"
#include "portrait_runner.h"

#include <gdiplus.h>
#include <objidl.h>

#include <shellapi.h>

#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <functional>

// --- Состояние GUI (только из GUI-потока) -------------------------------------
static bool  s_editMode     = false;
static char  s_inputBuf[32] = {};

// --- heroId → localized_name (для отображения) --------------------------------
// Заполняется лениво из heroes: на момент первого вызова таблица могла ещё не
// наполниться (runDataFetcherInit работает в фоне), поэтому пробуем снова,
// пока она пустая.
static std::map<int, std::string> g_heroDisplayNames;
std::string heroDisplayName(int heroId, const char* fallback) {
    if (g_heroDisplayNames.empty()) {
        try {
            SqliteDB db(DB_PATH, /*readOnly=*/true);
            SqliteStmt st(db.get(), "SELECT id, localized_name FROM heroes");
            while (st.row()) g_heroDisplayNames[st.col_int(0)] = st.col_text(1);
        } catch (...) { /* таблица ещё не создана фазой 1a - попробуем в следующий раз */ }
    }
    auto it = g_heroDisplayNames.find(heroId);
    return it != g_heroDisplayNames.end() ? it->second : (fallback ? fallback : "");
}

// --- Meta Heroes: живая стата по героям (таблица `stats`, живой STRATZ heroStats) -
// Тот же ленивый паттерн, что и heroDisplayName(): читаем DB_PATH напрямую из
// GUI-потока, повторяем попытку, пока фаза 1a (фоновый поток) не наполнит таблицу.
struct MetaHeroRow { char name[64] = {}; int heroId = 0; int games = 0; float winRate = 0.f; };
// Ключ 0 = агрегат по всем позициям (используется, когда позиция игрока ещё не определена).
static std::map<int, std::vector<MetaHeroRow>> g_metaHeroStats;
// Полный (нефильтрованный по 1%-порогу) lookup pos -> heroId -> row, для показа статы
// выбранного героя, даже если он не попал в топ-10/порог отсечения.
static std::map<int, std::map<int, MetaHeroRow>> g_metaHeroLookup;
static bool g_metaHeroStatsLoaded = false;
// Позиция, выбранная вручную в Meta Heroes, пока нет активной игры (0 = All).
// Как только начинается реальный драфт/матч, панель переключается на настоящую
// позицию из g_pickerState.ourPosition - это превью только для просмотра меты.
static int s_metaPreviewPos = 0;

static void loadMetaHeroStatsIfNeeded() {
    if (g_metaHeroStatsLoaded) return;
    try {
        SqliteDB db(DB_PATH, /*readOnly=*/true);
        SqliteStmt st(db.get(),
            "SELECT s.hero_id, s.pos, s.games, s.wins, h.localized_name "
            "FROM stats s JOIN heroes h ON h.id = s.hero_id "
            "WHERE s.pos BETWEEN 1 AND 5");

        struct Raw { int heroId, games, wins; std::string name; };
        std::map<int, std::vector<Raw>> byPos;      // pos(1-5) -> raw rows
        std::map<int, std::pair<int,int>> aggByHero; // heroId -> (games, wins) summed over all pos
        std::map<int, std::string> nameByHero;
        bool any = false;
        while (st.row()) {
            any = true;
            int hid = st.col_int(0);
            int pos = st.col_int(1);
            int g   = st.col_int(2);
            int w   = st.col_int(3);
            std::string nm = st.col_text(4);
            byPos[pos].push_back({hid, g, w, nm});
            auto& agg = aggByHero[hid];
            agg.first  += g;
            agg.second += w;
            nameByHero[hid] = nm;
        }
        if (!any) return; // таблица ещё пуста - попробуем в следующий кадр

        auto toRow = [](int hid, int g, int w, const std::string& nm) {
            MetaHeroRow r;
            r.heroId  = hid;
            r.games   = g;
            r.winRate = g > 0 ? (float)w / g : 0.f;
            std::snprintf(r.name, sizeof(r.name), "%s", nm.c_str());
            return r;
        };
        for (auto& [pos, rows] : byPos) {
            long long total = 0;
            for (auto& r : rows) total += r.games;
            long long floor = (long long)(total * 0.01);
            std::vector<MetaHeroRow> kept;
            auto& lookup = g_metaHeroLookup[pos];
            for (auto& r : rows) {
                MetaHeroRow row = toRow(r.heroId, r.games, r.wins, r.name);
                lookup[r.heroId] = row;
                if (r.games >= floor) kept.push_back(row);
            }
            std::sort(kept.begin(), kept.end(), [](auto&a, auto&b){ return a.games > b.games; });
            g_metaHeroStats[pos] = std::move(kept);
        }
        long long totalAll = 0;
        for (auto& [hid, gw] : aggByHero) totalAll += gw.first;
        long long aggFloor = (long long)(totalAll * 0.01);
        std::vector<MetaHeroRow> aggRows;
        auto& aggLookup = g_metaHeroLookup[0];
        for (auto& [hid, gw] : aggByHero) {
            MetaHeroRow row = toRow(hid, gw.first, gw.second, nameByHero[hid]);
            aggLookup[hid] = row;
            if (gw.first >= aggFloor) aggRows.push_back(row);
        }
        std::sort(aggRows.begin(), aggRows.end(), [](auto&a, auto&b){ return a.games > b.games; });
        g_metaHeroStats[0] = std::move(aggRows); // 0 = агрегат по всем позициям (ордер по сумме матчей)

        g_metaHeroStatsLoaded = true;
    } catch (...) { /* таблица ещё не создана фазой 1a - попробуем в следующий раз */ }
}

// --- Создание D3D11 текстуры из байтов изображения (JPEG/PNG) -----------------
ID3D11ShaderResourceView* createTextureFromImageData(const uint8_t* data, size_t size) {
    if (!data || size == 0 || !g_Device) return nullptr;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return nullptr;
    memcpy(GlobalLock(hMem), data, size);
    GlobalUnlock(hMem);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &stream))) {
        GlobalFree(hMem);
        return nullptr;
    }

    ID3D11ShaderResourceView* result = nullptr;
    {
        Gdiplus::Bitmap bmp(stream);
        if (bmp.GetLastStatus() == Gdiplus::Ok) {
            int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
            if (w > 0 && h > 0) {
                Gdiplus::BitmapData bd{};
                Gdiplus::Rect rect(0, 0, w, h);
                bmp.LockBits(&rect, Gdiplus::ImageLockModeRead,
                             PixelFormat32bppARGB, &bd);

                D3D11_TEXTURE2D_DESC desc{};
                desc.Width  = w;  desc.Height = h;
                desc.MipLevels = 1; desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.Usage     = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                D3D11_SUBRESOURCE_DATA init{};
                init.pSysMem      = bd.Scan0;
                init.SysMemPitch  = bd.Stride;

                ID3D11Texture2D* tex = nullptr;
                HRESULT hr = g_Device->CreateTexture2D(&desc, &init, &tex);
                bmp.UnlockBits(&bd);

                if (SUCCEEDED(hr) && tex) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.Format = desc.Format;
                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Texture2D.MipLevels = 1;
                    g_Device->CreateShaderResourceView(tex, &srvDesc, &result);
                    tex->Release();
                }
            }
        }
    }
    stream->Release();
    return result;
}

// --- Загрузка портретов героев из папки assets/ ------------------------------
// Файлы assets/*.png именованы по heroes.name (без префикса npc_dota_hero_,
// напр. "antimage.png"), а не по localized_name - тот Valve иногда временно
// меняет. Портреты кэшируются по heroId, поэтому переименование Valve не влияет
// на отображение.
static std::map<int, ID3D11ShaderResourceView*> g_heroPortraits;

// Суффикс heroes.name → heroId. Зашито в бинарник (не читается из БД), т.к.
// heroId в Dota 2 неизменны, а таблица heroes на первом запуске приложения
// ещё не заполнена (runDataFetcherInit работает в фоновом потоке и не успевает
// до вызова loadHeroPortraits() при старте GUI). При добавлении нового героя
// сюда нужно добавить строку вместе с assets/<suffix>.png.
static const std::map<std::string, int>& heroNameSuffixToId() {
    static const std::map<std::string, int> m = {
        {"antimage", 1}, {"axe", 2}, {"bane", 3}, {"bloodseeker", 4},
        {"crystal_maiden", 5}, {"drow_ranger", 6}, {"earthshaker", 7}, {"juggernaut", 8},
        {"mirana", 9}, {"morphling", 10}, {"nevermore", 11}, {"phantom_lancer", 12},
        {"puck", 13}, {"pudge", 14}, {"razor", 15}, {"sand_king", 16},
        {"storm_spirit", 17}, {"sven", 18}, {"tiny", 19}, {"vengefulspirit", 20},
        {"windrunner", 21}, {"zuus", 22}, {"kunkka", 23}, {"lina", 25},
        {"lion", 26}, {"shadow_shaman", 27}, {"slardar", 28}, {"tidehunter", 29},
        {"witch_doctor", 30}, {"lich", 31}, {"riki", 32}, {"enigma", 33},
        {"tinker", 34}, {"sniper", 35}, {"necrolyte", 36}, {"warlock", 37},
        {"beastmaster", 38}, {"queenofpain", 39}, {"venomancer", 40}, {"faceless_void", 41},
        {"skeleton_king", 42}, {"death_prophet", 43}, {"phantom_assassin", 44}, {"pugna", 45},
        {"templar_assassin", 46}, {"viper", 47}, {"luna", 48}, {"dragon_knight", 49},
        {"dazzle", 50}, {"rattletrap", 51}, {"leshrac", 52}, {"furion", 53},
        {"life_stealer", 54}, {"dark_seer", 55}, {"clinkz", 56}, {"omniknight", 57},
        {"enchantress", 58}, {"huskar", 59}, {"night_stalker", 60}, {"broodmother", 61},
        {"bounty_hunter", 62}, {"weaver", 63}, {"jakiro", 64}, {"batrider", 65},
        {"chen", 66}, {"spectre", 67}, {"ancient_apparition", 68}, {"doom_bringer", 69},
        {"ursa", 70}, {"spirit_breaker", 71}, {"gyrocopter", 72}, {"alchemist", 73},
        {"invoker", 74}, {"silencer", 75}, {"obsidian_destroyer", 76}, {"lycan", 77},
        {"brewmaster", 78}, {"shadow_demon", 79}, {"lone_druid", 80}, {"chaos_knight", 81},
        {"meepo", 82}, {"treant", 83}, {"ogre_magi", 84}, {"undying", 85},
        {"rubick", 86}, {"disruptor", 87}, {"nyx_assassin", 88}, {"naga_siren", 89},
        {"keeper_of_the_light", 90}, {"wisp", 91}, {"visage", 92}, {"slark", 93},
        {"medusa", 94}, {"troll_warlord", 95}, {"centaur", 96}, {"magnataur", 97},
        {"shredder", 98}, {"bristleback", 99}, {"tusk", 100}, {"skywrath_mage", 101},
        {"abaddon", 102}, {"elder_titan", 103}, {"legion_commander", 104}, {"techies", 105},
        {"ember_spirit", 106}, {"earth_spirit", 107}, {"abyssal_underlord", 108}, {"terrorblade", 109},
        {"phoenix", 110}, {"oracle", 111}, {"winter_wyvern", 112}, {"arc_warden", 113},
        {"monkey_king", 114}, {"dark_willow", 119}, {"pangolier", 120}, {"grimstroke", 121},
        {"hoodwink", 123}, {"void_spirit", 126}, {"snapfire", 128}, {"mars", 129},
        {"ringmaster", 131}, {"dawnbreaker", 135}, {"marci", 136}, {"primal_beast", 137},
        {"muerta", 138}, {"kez", 145}, {"largo", 155},
    };
    return m;
}

void loadHeroPortraits() {
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(L"assets\\*.png", &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    const auto& nameToId = heroNameSuffixToId();

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, L"assets\\%s", fd.cFileName);

        HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) continue;

        DWORD sz = GetFileSize(hFile, nullptr);
        if (sz == 0 || sz == INVALID_FILE_SIZE) { CloseHandle(hFile); continue; }

        std::vector<uint8_t> buf(sz);
        DWORD bytesRead = 0;
        ReadFile(hFile, buf.data(), sz, &bytesRead, nullptr);
        CloseHandle(hFile);
        if (bytesRead != sz) continue;

        auto* srv = createTextureFromImageData(buf.data(), buf.size());
        if (!srv) continue;

        // Имя файла (без .png) = суффикс heroes.name → находим heroId
        int len = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                      nullptr, 0, nullptr, nullptr);
        std::string name(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                            name.data(), len, nullptr, nullptr);
        if (name.size() > 4) name.resize(name.size() - 4); // strip ".png"
        for (auto& c : name) c = static_cast<char>(std::tolower((unsigned char)c));

        auto it = nameToId.find(name);
        if (it == nameToId.end()) { srv->Release(); continue; }

        g_heroPortraits[it->second] = srv;
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void unloadHeroPortraits() {
    for (auto& [id, srv] : g_heroPortraits) if (srv) srv->Release();
    g_heroPortraits.clear();
}

// --- Вспомогательные функции отрисовки ----------------------------------------
static void DrawPortrait(ImDrawList* dl, ImVec2 p, float sz,
                         ImU32 fill, ImU32 border, const char* label,
                         ImTextureID tex = 0) {
    if (tex) {
        dl->AddRectFilled(p, {p.x+sz, p.y+sz}, C(kCard));
        dl->AddImage(tex, p, {p.x+sz, p.y+sz});
    } else {
        dl->AddRectFilled(p, {p.x+sz, p.y+sz}, fill);
        if (label && *label) {
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText({p.x+(sz-ts.x)*0.5f, p.y+(sz-ts.y)*0.5f}, C(kText), label);
        }
    }
    dl->AddRect(p, {p.x+sz, p.y+sz}, border, 0.f, 0, 1.f);
}
static void DrawDashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    const float D=5.f, G=4.f;
    auto hline = [&](float x0, float x1, float y) {
        for (float x=x0; x<x1; x+=D+G)
            dl->AddLine({x,y},{std::min(x+D,x1),y},col);
    };
    auto vline = [&](float y0, float y1, float x) {
        for (float y=y0; y<y1; y+=D+G)
            dl->AddLine({x,y},{x,std::min(y+D,y1)},col);
    };
    hline(a.x,b.x,a.y); hline(a.x,b.x,b.y);
    vline(a.y,b.y,a.x); vline(a.y,b.y,b.x);
}
static void DrawBar(ImDrawList* dl, ImVec2 p, float w, float h, float frac, ImU32 fill) {
    frac = std::max(0.f, std::min(frac, 1.f));
    dl->AddRectFilled(p, {p.x+w,      p.y+h}, C(kCard2));
    dl->AddRectFilled(p, {p.x+w*frac, p.y+h}, fill);
}
static ImVec4 WinColor(float w) {
    if (w >= 0.55f) return kGreen;
    if (w >= 0.50f) return kAmber;
    return kRed;
}

// --- Отрисовка слота героя ----------------------------------------------------
// absSlot: 0-9 (0-4 Radiant, 5-9 Dire). usedPos: позиции 1-5 занятые другими слотами в команде.
// setPos(slot, pos) - вызывается попапом выбора позиции (на absSlot и, при свапе,
// на слоте партнёра); хранилище позиций не хардкожено здесь - своя игра пишет в
// g_portraitState (см. DrawDraftPanel), наблюдаемая - в g_spectatorState
// (см. DrawSpectatorDraftPanel), чтобы ручная метка на чужом матче не задевала
// нашу реальную позицию в собственном драфте.
static void DrawHeroSlot(float rowW, const HeroSlotGui& h,
                         bool radiantSide, bool showPos, int slotNum,
                         int absSlot, const int usedPos[5],
                         const std::function<void(int,int)>& setPos) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      rp  = ImGui::GetCursorScreenPos();
    const float lh  = ImGui::GetTextLineHeight();
    const float H   = lh * 3.0f;
    const float PSZ = lh * 2.1f;
    const float PAD = lh * 0.35f;

    ImU32 bgCol     = h.filled ? C(kCard) : Ca(kCard2, h.isYou ? 0.45f : 0.55f);
    ImU32 borderCol = h.isYou  ? Ca(kText, 0.55f) : C(kBorder);
    float bThick    = h.isYou  ? 2.f : 1.f;

    dl->AddRectFilled(rp, {rp.x+rowW, rp.y+H}, bgCol);
    if (!h.filled && !h.isYou)
        DrawDashedRect(dl, rp, {rp.x+rowW, rp.y+H}, borderCol);
    else
        dl->AddRect(rp, {rp.x+rowW, rp.y+H}, borderCol, 0.f, 0, bThick);

    ImVec2 pp      = {rp.x+PAD, rp.y+(H-PSZ)*0.5f};
    ImU32  heroFill = radiantSide ? IM_COL32(28,72,40,220) : IM_COL32(88,32,28,220);

    if (!h.filled && h.isYou) {
        dl->AddRectFilled(pp, {pp.x+PSZ, pp.y+PSZ}, C(kCard2));
        dl->AddRect      (pp, {pp.x+PSZ, pp.y+PSZ}, Ca(kText, 0.2f));
        float cx=pp.x+PSZ/2.f, cy=pp.y+PSZ/2.f, arm=PSZ*0.22f;
        dl->AddLine({cx-arm,cy},{cx+arm,cy},C(kText),1.5f);
        dl->AddLine({cx,cy-arm},{cx,cy+arm},C(kText),1.5f);
    } else if (!h.filled) {
        dl->AddRectFilled(pp, {pp.x+PSZ, pp.y+PSZ}, C(kCard2));
        dl->AddRect      (pp, {pp.x+PSZ, pp.y+PSZ}, C(kBorder));
        ImVec2 ts = ImGui::CalcTextSize("?");
        dl->AddText({pp.x+(PSZ-ts.x)/2.f, pp.y+(PSZ-ts.y)/2.f}, C(kMuted), "?");
    } else {
        char ab[3] = {h.name[0], h.name[1] ? h.name[1] : '\0', '\0'};
        ImTextureID htex = 0;
        auto it = g_heroPortraits.find(h.heroId);
        if (it != g_heroPortraits.end()) htex = (ImTextureID)it->second;
        DrawPortrait(dl, pp, PSZ, heroFill, Ca(kText, 0.18f), ab, htex);
    }

    float tx = pp.x + PSZ + PAD;

    if (!h.filled && h.isYou) {
        dl->AddText({tx, rp.y+(H-lh)*0.5f}, C(kText), "Your Pick");
    } else if (!h.filled) {
        dl->AddText({tx, rp.y+(H-lh)*0.5f}, C(kMuted), "Unknown");
    } else {
        dl->AddText({tx, rp.y+(H-lh)*0.5f}, C(kText), h.name);
    }

    // Тег позиции - кликабельный popup для своей команды.
    if (showPos) {
        int dispPos = (h.pos > 0) ? h.pos : 0;
        char posStr[8];
        if (dispPos > 0)
            std::snprintf(posStr, sizeof(posStr), "Pos%d", dispPos);
        else
            std::snprintf(posStr, sizeof(posStr), "Pos?");

        ImVec2 ts  = ImGui::CalcTextSize(posStr);
        float  tgW = ts.x + PAD*1.5f;
        float  tgH = lh + 4.f;
        float  tgX = rp.x + rowW - tgW - PAD;
        float  tgY = rp.y + (H - tgH) * 0.5f;

        dl->AddRectFilled({tgX,tgY},{tgX+tgW,tgY+tgH}, C(kCard2));
        dl->AddText({tgX+PAD*0.75f,tgY+2.f}, C(kMuted), posStr);

        char popupId[32];
        std::snprintf(popupId, sizeof(popupId), "##pos_%d", absSlot);
        ImGui::SetCursorScreenPos({tgX, tgY});
        if (ImGui::InvisibleButton(popupId, {tgW, tgH}))
            ImGui::OpenPopup(popupId);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to set position");

        if (ImGui::BeginPopup(popupId)) {
            int myIdx = absSlot % 5;
            int teamBase = absSlot - myIdx; // 0 for radiant, 5 for dire

            // "?" = All/сброс - унифицировано с бейджами Picks/Meta (SectionLabelWithPosBadge):
            // там это "All", здесь, т.к. слот всегда занят конкретным героем, а не
            // агрегатом - снимает ручной override и возвращает авто-детект с экрана.
            if (ImGui::Selectable("?", dispPos <= 0)) {
                setPos(absSlot, 0);
            }
            for (int p = 1; p <= 5; ++p) {
                char label[16];
                std::snprintf(label, sizeof(label), "Pos %d", p);
                if (ImGui::Selectable(label, p == dispPos)) {
                    // Свап: если позиция занята другим слотом, обменять
                    for (int j = 0; j < 5; ++j) {
                        if (j != myIdx && usedPos[j] == p) {
                            setPos(teamBase + j, dispPos);
                            break;
                        }
                    }
                    setPos(absSlot, p);
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::SetCursorScreenPos(rp);
    ImGui::Dummy({rowW, H});
    ImGui::Spacing();
}

// --- Метка секции ------------------------------------------------------------
static void SectionLabel(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(">");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6.f);
    ImGui::TextUnformatted(label);
}

// --- Метка секции + бейдж текущей позиции (RECOMMENDED PICKS / META HEROES) ---
// Бейдж по умолчанию делит строку с заголовком (справа). Если панель узкая и
// полный текст бейджа перекрыл бы заголовок - сначала пробуем сократить текст
// бейджа ("[=] Position 5" → "Pos 5"), а если и он не влезает - переносим
// бейдж на отдельную строку под заголовком. Так бейдж никогда не наезжает на
// текст секции, при любой ширине панели.
// onPositionPick != nullptr делает бейдж кликабельным: клик открывает popup с
// выбором All/Position 1-5, выбор вызывает onPositionPick(p) (0 = All). Унифицировано
// для всех трёх мест выбора позиции (Draft-слот/Picks/Meta) - везде один и тот же
// набор вариантов и один и тот же эффект: меняет НАШУ настоящую позицию в драфте
// (см. SetOurPosition) - нет отдельного "auto"-режима или независимого фильтра.
// onPositionPick == nullptr - бейдж остаётся некликабельным (напр. до старта игры).
static void SectionLabelWithPosBadge(const char* label, float panelW, int position,
                                      std::function<void(int)> onPositionPick = nullptr) {
    const float lh     = ImGui::GetTextLineHeight();
    const float startX = ImGui::GetCursorPosX();

    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(">");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6.f);
    ImGui::TextUnformatted(label);

    // Порог "влезает/не влезает" считаем по самому длинному из возможных
    // заголовков секции ("RECOMMENDED PICKS"), а не по текущему тексту -
    // иначе соседние панели (RECOMMENDED PICKS/META HEROES, YOUR PICK/YOUR HERO)
    // схлопывались бы каждая на своей ширине, вразнобой.
    float reservedLabelW = ImGui::CalcTextSize("RECOMMENDED PICKS").x;
    float labelEndX = startX + ImGui::CalcTextSize(">").x + 6.f + reservedLabelW;

    char fullText[40], shortText[32];
    if (position > 0) {
        std::snprintf(fullText,  sizeof(fullText),  "[=] Position %d", position);
        std::snprintf(shortText, sizeof(shortText), "Pos %d", position);
    } else {
        std::snprintf(fullText,  sizeof(fullText),  "[=] All Positions");
        std::snprintf(shortText, sizeof(shortText), "All Pos");
    }

    const float GAP = 8.f;
    const float bH  = lh + 6.f;
    const char* badgeText = fullText;
    float bW = ImGui::CalcTextSize(badgeText).x + 14.f;
    bool sameLine = (labelEndX + GAP + bW <= panelW);
    if (!sameLine) {
        badgeText = shortText;
        bW = ImGui::CalcTextSize(badgeText).x + 14.f;
        sameLine = (labelEndX + GAP + bW <= panelW);
    }

    if (sameLine)
        ImGui::SameLine(panelW - bW);
    else
        // Ни полный, ни сокращённый текст не помещаются рядом с заголовком -
        // бейдж уходит на новую строку (курсор уже на ней после TextUnformatted).
        ImGui::SetCursorPosX((std::max)(startX, panelW - bW));

    bool clickable = (bool)onPositionPick;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bp = ImGui::GetCursorScreenPos();
    bool hovered = clickable && ImGui::IsMouseHoveringRect(bp, {bp.x+bW,bp.y+bH});
    dl->AddRectFilled(bp,{bp.x+bW,bp.y+bH}, hovered ? C(kCard) : C(kCard2));
    dl->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder));
    dl->AddText({bp.x+7.f,bp.y+3.f},C(kMuted),badgeText);

    if (clickable) {
        char btnId[48], popupId[48];
        std::snprintf(btnId,   sizeof(btnId),   "##posBadgeBtn_%s",    label);
        std::snprintf(popupId, sizeof(popupId), "##posBadgePopup_%s",  label);

        ImGui::SetCursorScreenPos(bp);
        if (ImGui::InvisibleButton(btnId, {bW, bH}))
            ImGui::OpenPopup(popupId);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to change position");

        if (ImGui::BeginPopup(popupId)) {
            if (ImGui::Selectable("All", position <= 0)) onPositionPick(0);
            for (int p = 1; p <= 5; ++p) {
                char plabel[16];
                std::snprintf(plabel, sizeof(plabel), "Pos %d", p);
                if (ImGui::Selectable(plabel, position == p)) onPositionPick(p);
            }
            ImGui::EndPopup();
        }
    } else {
        ImGui::Dummy({bW,bH});
    }
}

// Меняет НАШУ настоящую позицию в текущем драфте - тот же эффект, что и клик
// по позиции нашего слота в Draft-панели (DrawHeroSlot: свап с тиммейтом, если
// позиция уже занята, запись в g_portraitState.manualPos + requestPositionRefresh).
// newPos: 0 = All/сброс (снять ручной override, вернуться к авто-детекту с экрана),
// 1-5 = конкретная позиция. Вынесено отдельно, т.к. это единая точка изменения нашей
// позиции - вызывается и из бейджа Picks-панели, и из бейджа Meta-панели, держит
// Draft/Picks/Meta синхронизированными без отдельного "auto"-режима.
static void SetOurPosition(int newPos) {
    if (newPos < 0 || newPos > 5) return;

    bool isRadiant;
    int  ourSlot;
    int  usedPos[5] = {};
    {
        std::lock_guard<std::mutex> lk(g_pickerState.mtx);
        isRadiant = g_pickerState.isRadiant;
        ourSlot   = g_pickerState.ourSlot;
        const HeroSlotGui* team = isRadiant ? g_pickerState.radiant : g_pickerState.dire;
        for (int i = 0; i < 5; ++i) usedPos[i] = team[i].pos;
    }

    int myIdx = ourSlot - 1;
    if (myIdx < 0 || myIdx > 4) return;
    int teamBase = isRadiant ? 0 : 5;
    int absSlot  = teamBase + myIdx;
    int dispPos  = usedPos[myIdx];

    std::lock_guard<std::mutex> lk(g_portraitState.mtx);
    // Свап нужен только при назначении конкретной позиции - сброс на "All" (0)
    // ни у кого ничего не отнимает, свапать с тиммейтом нечего.
    if (newPos >= 1) {
        for (int j = 0; j < 5; ++j) {
            if (j != myIdx && usedPos[j] == newPos) {
                g_portraitState.manualPos[teamBase + j] = dispPos;
                break;
            }
        }
    }
    g_portraitState.manualPos[absSlot] = newPos;
    requestPositionRefresh();
}

// Смена позиции для превью в Meta Heroes вне игры - просто локальный выбор
// в GUI, без livepicks/оркестратора (нет активного драфта, не с чем сверяться).
static void SetMetaPreviewPosition(int newPos) {
    if (newPos < 0 || newPos > 5) return;
    s_metaPreviewPos = newPos;
}

// Колбэк позиции для слотов СВОЕЙ игры (DrawDraftPanel) - прежнее поведение
// DrawHeroSlot до вынесения записи в g_portraitState за колбэк setPos.
static void SetOwnGameManualPos(int absSlot, int pos) {
    std::lock_guard<std::mutex> lk(g_portraitState.mtx);
    g_portraitState.manualPos[absSlot] = pos;
    requestPositionRefresh();
}

// Колбэк позиции для слотов НАБЛЮДАЕМОЙ игры (DrawSpectatorDraftPanel) - чисто
// локальная метка в g_spectatorState, не задевает g_portraitState/livepicks:
// это не наш драфт, оркестратору и пикеру сверяться не с чем.
static void SetSpectatorManualPos(int absSlot, int pos) {
    std::lock_guard<std::mutex> lk(g_spectatorState.mtx);
    if (absSlot >= 0 && absSlot < 10) g_spectatorState.slots[absSlot].manualPos = pos;
    requestRedraw();
}

// --- Общая строка героя со статистикой ---------------------------------------
// Общий рендер строки (портрет/имя/вторичная стата/win% + бар) для
// DrawPicksPanel и DrawMetaHeroesPanel - отличается только вторичная строка статы.
struct HeroStatRowParams {
    int         rank;
    const char* name;
    int         heroId;
    float       winFrac;     // 0..1, определяет цвет и заполнение бара
    bool        highlight;
    std::string secondaryStats;  // "you 12g 55%" / "340 games" / "" (пусто = без колонки статы)
};
static void DrawHeroStatRow(float rowW, const HeroStatRowParams& p) {
    const float WIN_COL_W = 88.f;
    const float BAR_W     = 68.f;

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      rp  = ImGui::GetCursorScreenPos();
    const float lh2 = ImGui::GetTextLineHeight();
    const float RH  = lh2 * 2.9f;    // ~64px at lh=22
    const float PSZ = lh2 * 2.0f;    // ~44px
    ImVec4      wc  = WinColor(p.winFrac);

    // Фиксированные позиции колонок: rank(+4) / портрет(+28) / имя(+70,
    // фиксирован) / стата(47% ширины строки) / win% (rowW - WIN_COL_W).
    const float portX  = (p.rank > 0) ? rp.x + lh2 * 1.8f : rp.x + lh2 * 0.3f;
    const float nameX  = portX + PSZ + lh2 * 0.5f;
    const float statsX = rp.x + rowW * 0.47f;
    const float winX   = rp.x + rowW - WIN_COL_W;

    dl->AddRectFilled(rp,{rp.x+rowW,rp.y+RH},C(kCard));
    if (p.highlight)
        dl->AddRect(rp,{rp.x+rowW,rp.y+RH},Ca(wc,0.6f),0.f,0,1.5f);
    else
        dl->AddRect(rp,{rp.x+rowW,rp.y+RH},C(kBorder));

    if (p.rank > 0) {
        char rb[4]; std::snprintf(rb,sizeof(rb),"%2d",p.rank);
        dl->AddText({rp.x+4.f, rp.y+(RH-lh2)*0.5f}, C(kMuted), rb);
    }

    ImVec2 pp  = {portX, rp.y+(RH-PSZ)*0.5f};
    ImU32  fill = p.highlight ? Ca(wc,0.20f) : C(kCard2);
    ImU32  bord = p.highlight ? Ca(wc,0.45f) : Ca(kText,0.12f);
    char   ab[3] = {p.name[0], p.name[1] ? p.name[1] : '\0', '\0'};
    ImTextureID htex = 0;
    auto hit = g_heroPortraits.find(p.heroId);
    if (hit != g_heroPortraits.end()) htex = (ImTextureID)hit->second;
    DrawPortrait(dl, pp, PSZ, fill, bord, ab, htex);

    dl->PushClipRect({nameX, rp.y}, {statsX - 6.f, rp.y+RH}, true);
    dl->AddText({nameX, rp.y+(RH-lh2)*0.5f}, C(kText), p.name);
    dl->PopClipRect();

    if (!p.secondaryStats.empty()) {
        dl->PushClipRect({statsX, rp.y}, {winX - 4.f, rp.y+RH}, true);
        dl->AddText({statsX, rp.y+(RH-lh2)*0.5f}, Ca(kMuted,0.85f), p.secondaryStats.c_str());
        dl->PopClipRect();
    }

    char   wb[8]; std::snprintf(wb,sizeof(wb),"%.1f%%", p.winFrac*100.f);
    ImVec2 wts = ImGui::CalcTextSize(wb);
    dl->AddText({winX+(WIN_COL_W-wts.x)*0.5f, rp.y+5.f}, C(wc), wb);
    float barX = winX + (WIN_COL_W-BAR_W)*0.5f;
    DrawBar(dl, {barX, rp.y+lh2+12.f}, BAR_W, 4.f, p.winFrac, C(wc));

    ImGui::Dummy({rowW,RH});
    ImGui::Spacing();
}

// Легенда цветов win-rate - общая для DrawPicksPanel и DrawMetaHeroesPanel.
static void DrawWinRateLegend(float rowW) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 fp  = ImGui::GetCursorScreenPos();
    const float lhL = ImGui::GetTextLineHeight();
    const float dotSz = lhL * 0.55f;
    const float dotY  = fp.y + (lhL - dotSz) * 0.5f;
    const float gap   = lhL * 0.5f;

    struct LegItem { ImVec4 col; const char* txt; };
    LegItem items[] = {
        {kGreen, ">=55%"},
        {kAmber, "50-55%"},
        {kRed,   "<50%"}
    };

    float x = fp.x + 4.f;
    for (auto& it : items) {
        dl->AddRectFilled({x, dotY}, {x+dotSz, dotY+dotSz}, C(it.col));
        x += dotSz + 4.f;
        ImVec2 ts = ImGui::CalcTextSize(it.txt);
        dl->AddText({x, fp.y}, Ca(kMuted, 0.8f), it.txt);
        x += ts.x + gap * 2.f;
    }
    ImGui::Dummy({rowW, lhL + 4.f});
}

// --- "Powered by" - иконки источников данных (STRATZ/OpenDota/D2PT) ----------
// Иконки - favicon'ы сайтов, тянутся по HTTP (тот же приём, что и аватар игрока
// в fetchOpenDotaProfile: байты через httpGet в фоновом потоке под мьютексом,
// декодирование в D3D11-текстуру - только на GUI-потоке, при отрисовке).
struct PoweredByIcon {
    const char*                label;
    const char*                url;
    const char*                faviconUrl;
    std::vector<uint8_t>       imgData;
    bool                       fetched = false;
    ID3D11ShaderResourceView*  srv = nullptr;
};
static std::mutex     g_poweredByMtx;
static bool            g_poweredByFetchStarted = false;
static PoweredByIcon   g_poweredByIcons[3] = {
    { "STRATZ",   "https://stratz.com",       "https://www.google.com/s2/favicons?domain=stratz.com&sz=64"   },
    { "OpenDota", "https://www.opendota.com", "https://www.google.com/s2/favicons?domain=opendota.com&sz=64" },
    { "D2PT",     "https://d2pt.gg",          "https://www.google.com/s2/favicons?domain=d2pt.gg&sz=64"      },
};

// Иконки контактов/репозитория - тот же приём (favicon по HTTP), отдельная
// строка под "POWERED BY": концептуально не источник данных, а ссылки на
// автора/проект.
static std::mutex     g_contactMtx;
static bool            g_contactFetchStarted = false;
static PoweredByIcon   g_contactIcons[2] = {
    { "Discord", "https://discordapp.com/users/693097611674255400",  "https://www.google.com/s2/favicons?domain=discord.com&sz=64" },
    { "GitHub",  "https://github.com/yphilistine/dota_drafter/tree/main/finalapp", "https://www.google.com/s2/favicons?domain=github.com&sz=64"  },
};

static void fetchIconsAsync(PoweredByIcon* icons, int n, std::mutex* mtx) {
    std::thread([icons, n, mtx]{
        for (int i = 0; i < n; ++i) {
            std::vector<uint8_t> data;
            try {
                std::string body = httpGet(icons[i].faviconUrl);
                data.assign(body.begin(), body.end());
            } catch (...) {}
            std::lock_guard<std::mutex> lk(*mtx);
            icons[i].imgData = std::move(data);
            icons[i].fetched = true;
        }
    }).detach();
}

void unloadPoweredByIcons() {
    for (auto& ic : g_poweredByIcons)
        if (ic.srv) { ic.srv->Release(); ic.srv = nullptr; }
    for (auto& ic : g_contactIcons)
        if (ic.srv) { ic.srv->Release(); ic.srv = nullptr; }
}

// Строка иконок-ссылок с явным origin (не текущий курсор ImGui) - чтобы можно
// было разместить две такие строки рядом на одном уровне, а не одну под другой.
// Заголовок + ряд кликабельных favicon'ов (ShellExecute на клик). Общая для
// "POWERED BY" (источники данных) и "CONTACT INFO" (Discord/GitHub) - один
// расчёт layout/клика для обеих строк вместо двух копий одного и того же тела.
static constexpr float kIconRowIconSz  = 56.f;
static constexpr float kIconRowGap     = 16.f; // расстояние между иконками внутри группы
static constexpr float kIconRowSectGap = 32.f; // расстояние между группами (POWERED BY / CONTACT INFO)

// Ширина группы из n иконок - нужна DrawPoweredBy, чтобы поставить вторую
// группу сразу после первой, а не на фиксированной доле panelW.
static float IconRowWidth(int n) {
    return n * kIconRowIconSz + (n - 1) * kIconRowGap;
}

static void DrawIconRowAt(ImVec2 origin, const char* title, PoweredByIcon* icons, int n,
                           std::mutex& mtx, bool& fetchStarted, const char* idPrefix) {
    if (!fetchStarted) {
        fetchStarted = true;
        fetchIconsAsync(icons, n, &mtx);
    }

    // Декодируем в текстуру то, что успело докачаться - по одной попытке на иконку.
    for (int i = 0; i < n; ++i) {
        PoweredByIcon& ic = icons[i];
        if (ic.srv) continue;
        std::vector<uint8_t> data;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (!ic.fetched || ic.imgData.empty()) continue;
            data = ic.imgData;
        }
        ic.srv = createTextureFromImageData(data.data(), data.size());
    }

    const float iconSz   = kIconRowIconSz;
    const float groupGap = kIconRowGap;
    const float titleGap = ImGui::GetTextLineHeightWithSpacing();

    ImGui::SetCursorScreenPos(origin);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();

    ImDrawList* dl       = ImGui::GetWindowDrawList();
    ImVec2      rowStart = { origin.x, origin.y + titleGap };

    // Прижаты к левому краю своей группы, вплотную друг к другу.
    float x = rowStart.x;
    for (int i = 0; i < n; ++i) {
        const PoweredByIcon& ic = icons[i];
        ImVec2 iconPos = { x, rowStart.y };

        if (ic.srv)
            dl->AddImage((ImTextureID)ic.srv, iconPos, {iconPos.x + iconSz, iconPos.y + iconSz});
        else {
            dl->AddRectFilled(iconPos, {iconPos.x + iconSz, iconPos.y + iconSz}, C(kCard2));
            dl->AddRect      (iconPos, {iconPos.x + iconSz, iconPos.y + iconSz}, C(kBorder));
        }

        char btnId[32];
        std::snprintf(btnId, sizeof(btnId), "##%s%d", idPrefix, i);
        ImGui::SetCursorScreenPos(iconPos);
        if (ImGui::InvisibleButton(btnId, {iconSz, iconSz}))
            ShellExecuteA(nullptr, "open", ic.url, nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ic.url);
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        x += iconSz + groupGap;
    }
}

static void DrawPoweredBy(float panelW) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImVec2 origin = ImGui::GetCursorScreenPos();

    DrawIconRowAt(origin, "POWERED BY", g_poweredByIcons, 3,
                  g_poweredByMtx, g_poweredByFetchStarted, "poweredBy");

    float contactX = origin.x + IconRowWidth(3) + kIconRowSectGap;
    DrawIconRowAt({contactX, origin.y}, "CONTACT INFO", g_contactIcons, 2,
                  g_contactMtx, g_contactFetchStarted, "contact");

    ImGui::SetCursorScreenPos({origin.x, origin.y + ImGui::GetTextLineHeightWithSpacing() + kIconRowIconSz});
}

// Заголовок колонки команды (цветная метка + RADIANT/DIRE + n/5) - общий для
// DrawDraftPanel и DrawSpectatorDraftPanel.
static void DrawTeamColumnHeader(float colW, bool radiantSide, int count) {
    const float lh = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 hp = ImGui::GetCursorScreenPos();
    dl->AddRectFilled({hp.x+1,hp.y+4},{hp.x+9,hp.y+12}, radiantSide ? C(kGreen) : C(kRed));
    ImGui::Dummy({10.f, lh});
    ImGui::SameLine(0, 4.f);
    ImGui::PushStyleColor(ImGuiCol_Text, radiantSide ? kGreen : kRed);
    ImGui::TextUnformatted(radiantSide ? "RADIANT" : "DIRE");
    ImGui::PopStyleColor();
    ImGui::SameLine(colW-24.f);
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::Text("%d/5", count);
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

// --- Наблюдаемая игра (спектейт чужого матча) ---------------------------------
// Герои - из g_spectatorState (GSI player.team2/team3.playerN.hero_id, см.
// livestatsfetcher.cpp), без portrait capture и без ML: это не наш драфт, нет
// "нашей" стороны/позиции. Позиции - только ручная метка, кликабельна для
// обеих команд (в отличие от DrawDraftPanel, где popup виден только на своей
// стороне) и ни на что за пределами этой панели не влияет - Picks/Meta-панели
// в это время остаются в обычном idle-состоянии (gameStarted у пикера не
// трогается, см. orchestrator.cpp).
static void DrawSpectatorDraftPanel(float panelW, const SpectatorHeroSlot spec[10]) {
    const float PAD  = 8.f;
    const float colW = (panelW - PAD) * 0.5f;
    const float lh   = ImGui::GetTextLineHeight();

    SectionLabel("SPECTATING");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    HeroSlotGui rad[5], dir[5];
    int radUsedPos[5] = {}, dirUsedPos[5] = {};
    int rCount = 0, dCount = 0;
    for (int i = 0; i < 5; ++i) {
        rad[i].heroId = spec[i].heroId;
        rad[i].filled = spec[i].heroId > 0;
        rad[i].pos    = spec[i].manualPos;
        if (rad[i].filled) {
            std::snprintf(rad[i].name, sizeof(rad[i].name), "%s",
                          heroDisplayName(spec[i].heroId, "").c_str());
            ++rCount;
        }
        radUsedPos[i] = rad[i].pos;
    }
    for (int i = 0; i < 5; ++i) {
        dir[i].heroId = spec[5+i].heroId;
        dir[i].filled = spec[5+i].heroId > 0;
        dir[i].pos    = spec[5+i].manualPos;
        if (dir[i].filled) {
            std::snprintf(dir[i].name, sizeof(dir[i].name), "%s",
                          heroDisplayName(spec[5+i].heroId, "").c_str());
            ++dCount;
        }
        dirUsedPos[i] = dir[i].pos;
    }

    ImGui::BeginGroup();
    DrawTeamColumnHeader(colW, true, rCount);
    for (int i = 0; i < 5; ++i)
        DrawHeroSlot(colW, rad[i], true, true, i+1, i, radUsedPos, SetSpectatorManualPos);
    ImGui::EndGroup();

    ImGui::SameLine(0, PAD);

    ImGui::BeginGroup();
    DrawTeamColumnHeader(colW, false, dCount);
    for (int i = 0; i < 5; ++i)
        DrawHeroSlot(colW, dir[i], false, true, i+1, 5+i, dirUsedPos, SetSpectatorManualPos);
    ImGui::EndGroup();

    // Вместо win probability bar - нет ML-предсказания для чужого драфта.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bp = ImGui::GetCursorScreenPos();
    float  bW = panelW, bH = 52.f;
    dl->AddRectFilled(bp,{bp.x+bW,bp.y+bH},Ca(kCard2,0.4f));
    dl->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder));
    const char* msg = "Spectating - no personal predictions";
    ImVec2 mts = ImGui::CalcTextSize(msg);
    dl->AddText({bp.x+(bW-mts.x)*0.5f, bp.y+(bH-lh)*0.5f}, C(kMuted), msg);
    ImGui::Dummy({bW, bH});

    DrawPoweredBy(panelW);
}

// --- Панель драфта (Radiant/Dire слоты + win probability bar) ----------------
void DrawDraftPanel(float panelW) {
    const float PAD  = 8.f;
    const float colW = (panelW - PAD) * 0.5f;
    const float lh   = ImGui::GetTextLineHeight();

    // Снимок состояния пикера
    bool         gameStarted;
    bool         isRadiant;
    float        winProb;
    HeroSlotGui  rad[5], dir[5];
    bool         ourHeroPicked;
    GamePhase    phase;
    bool         isWaitingForPlayers;
    {
        std::lock_guard<std::mutex> lk(g_pickerState.mtx);
        gameStarted   = g_pickerState.gameStarted;
        isRadiant     = g_pickerState.isRadiant;
        winProb       = g_pickerState.winProb;
        ourHeroPicked = g_pickerState.ourHeroPicked;
        for (int i=0;i<5;i++) rad[i] = g_pickerState.radiant[i];
        for (int i=0;i<5;i++) dir[i] = g_pickerState.dire[i];
    }
    {
        std::lock_guard<std::mutex> lk(g_gameInfo.mtx);
        phase               = g_gameInfo.phase;
        isWaitingForPlayers = g_gameInfo.isWaitingForPlayers;
    }

    // Наблюдаемая игра (спектейт) имеет приоритет только когда своего активного
    // драфта нет - своя игра физически не может идти параллельно со спектейтом
    // одного и того же GSI-клиента, но на всякий случай не перекрываем реальный
    // драфт наблюдением.
    if (!gameStarted) {
        bool specActive;
        SpectatorHeroSlot specSlots[10];
        {
            std::lock_guard<std::mutex> lk(g_spectatorState.mtx);
            specActive = g_spectatorState.active;
            for (int i=0;i<10;i++) specSlots[i] = g_spectatorState.slots[i];
        }
        if (specActive) {
            DrawSpectatorDraftPanel(panelW, specSlots);
            return;
        }
    }

    int rCount=0, dCount=0;
    for (int i=0;i<5;i++) { if (rad[i].filled) ++rCount; if (dir[i].filled) ++dCount; }

    // WAIT_FOR_PLAYERS_TO_LOAD маппится на INGAME (gameStarted уже true с драфта),
    // поэтому без этого флага панель молча показывала бы обычные слоты драфта без
    // какой-либо индикации, что матч уже начал загружаться.
    SectionLabel(isWaitingForPlayers ? "LOADING INTO MATCH"
                 : (rCount==5 && dCount==5) ? "STRATEGY PHASE" : "DRAFT PHASE");

    // [screenshot] - сохраняет 10 регионов героев + 10 регионов позиций текущего
    // кадра HUD + fullscreen_regions.png (полный кадр с рамками всех 20 регионов)
    // в screenshots/ (папка создаётся инсталлятором). Диагностика для случаев,
    // когда распознавание портретов/позиций ошибается. Оформлен как
    // position-бейдж (SectionLabelWithPosBadge) - тот же rect/border/text через
    // ImDrawList + InvisibleButton поверх, а не нативная ImGui::Button.
    {
        const float lhBtn = ImGui::GetTextLineHeight();
        const char* btnLabel = "[screenshot]";
        ImVec2 lts = ImGui::CalcTextSize(btnLabel);
        float  bW  = lts.x + 14.f;
        float  bH  = lhBtn + 6.f;
        ImGui::SameLine(panelW - bW);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        bool hovered = ImGui::IsMouseHoveringRect(bp, {bp.x+bW, bp.y+bH});
        dl->AddRectFilled(bp,{bp.x+bW,bp.y+bH}, hovered ? C(kCard) : C(kCard2));
        dl->AddRect      (bp,{bp.x+bW,bp.y+bH}, C(kBorder));
        dl->AddText({bp.x+7.f,bp.y+3.f}, C(kMuted), btnLabel);

        ImGui::SetCursorScreenPos(bp);
        if (ImGui::InvisibleButton("##screenshotBtn", {bW, bH})) {
            std::thread([]{
                bool ok = captureDebugScreenshotWithReport(DB_PATH, "screenshots");
                if (ok) LOG_INFO("[screenshot] Saved draft regions + fullscreen_regions.png + report.txt to screenshots/");
                else    LOG_WARN("[screenshot] Dota 2 window not found - nothing captured");
            }).detach();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Press in case of draft recognition error");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // -- Waiting overlay ----------------------------------------------------
    if (!gameStarted) {
        ImDrawList* dl  = ImGui::GetWindowDrawList();
        ImVec2      cp  = ImGui::GetCursorScreenPos();
        float       aH  = 300.f;
        float       aW  = panelW;

        dl->AddRectFilled(cp, {cp.x+aW, cp.y+aH}, Ca(kCard2, 0.4f));
        dl->AddRect      (cp, {cp.x+aW, cp.y+aH}, C(kBorder));

        const char* line1 = "Waiting for a game...";
        const char* line2 = (phase == GamePhase::IDLE) ? "GSI server: listening on :62326" : phaseName(phase);

        ImVec2 ts1 = ImGui::CalcTextSize(line1);
        ImVec2 ts2 = ImGui::CalcTextSize(line2);
        dl->AddText({cp.x+(aW-ts1.x)*0.5f, cp.y+aH*0.5f-lh},       C(kMuted),       line1);
        dl->AddText({cp.x+(aW-ts2.x)*0.5f, cp.y+aH*0.5f+2.f},       Ca(kMuted,0.6f), line2);

        ImGui::Dummy({aW, aH});
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        ImVec2 bp  = ImGui::GetCursorScreenPos();
        float  bW  = panelW;
        float  bH  = 52.f;
        dl2->AddRectFilled(bp,{bp.x+bW,bp.y+bH},Ca(kCard2,0.4f));
        dl2->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder));
        float cy = bp.y+(bH-lh)*0.5f;
        dl2->AddText({bp.x+10.f,cy},C(kMuted),">  Current draft win probability");
        dl2->AddText({bp.x+bW-50.f,cy},C(kMuted),"--.--%");
        ImGui::Dummy({bW,bH});
        DrawPoweredBy(panelW);
        return;
    }

    // Позиции, занятые в каждой команде (для popup без дубликатов)
    int radUsedPos[5] = {}, dirUsedPos[5] = {};
    for (int i=0;i<5;i++) { radUsedPos[i] = rad[i].pos; dirUsedPos[i] = dir[i].pos; }

    // -- Radiant column -----------------------------------------------------
    ImGui::BeginGroup();
    DrawTeamColumnHeader(colW, true, rCount);
    for (int i=0;i<5;i++) DrawHeroSlot(colW, rad[i], true,  isRadiant,  i+1, i, radUsedPos, SetOwnGameManualPos);
    ImGui::EndGroup();

    ImGui::SameLine(0, PAD);

    // -- Dire column --------------------------------------------------------
    ImGui::BeginGroup();
    DrawTeamColumnHeader(colW, false, dCount);
    for (int i=0;i<5;i++) DrawHeroSlot(colW, dir[i], false, !isRadiant, i+1, 5+i, dirUsedPos, SetOwnGameManualPos);
    ImGui::EndGroup();

    // -- Win probability bar ------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bp  = ImGui::GetCursorScreenPos();
    float  bW  = panelW;
    float  bH  = 52.f;
    ImVec4 wc  = WinColor(winProb);

    dl->AddRectFilled(bp,{bp.x+bW,bp.y+bH},Ca(kCard2,0.4f));
    dl->AddRect      (bp,{bp.x+bW,bp.y+bH},C(kBorder),0.f,0,1.f);

    float cy = bp.y+(bH-lh)*0.5f;
    dl->AddText({bp.x+10.f,cy}, C(kMuted), ">");
    dl->AddText({bp.x+24.f,cy}, C(kMuted), "Current draft win probability");

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f%%", winProb*100.f);
    ImVec2 vts = ImGui::CalcTextSize(buf);
    dl->AddText({bp.x+bW-vts.x-10.f, cy}, ourHeroPicked ? C(wc) : C(kMuted), buf);

    DrawBar(dl, {bp.x+1.f, bp.y+bH-6.f}, bW-2.f, 4.f, winProb, C(wc));
    ImGui::Dummy({bW, bH});

    DrawPoweredBy(panelW);
}

// --- Панель рекомендаций (top-10 / выбранный герой) ---------------------------
void DrawPicksPanel(float panelW) {
    const float rW_ref = panelW;
    const float lh     = ImGui::GetTextLineHeight();

    // Снимок состояния
    bool       gameStarted, ourHeroPicked;
    char       ourHeroName[64];
    int        ourHeroId;
    float      winProb;
    int        ourPosition, ourSlot;
    bool       isRadiant;
    PickRowGui recs[10];
    int        recCount;
    {
        std::lock_guard<std::mutex> lk(g_pickerState.mtx);
        gameStarted   = g_pickerState.gameStarted;
        ourHeroPicked = g_pickerState.ourHeroPicked;
        winProb       = g_pickerState.winProb;
        ourPosition   = g_pickerState.ourPosition;
        ourSlot       = g_pickerState.ourSlot;
        isRadiant     = g_pickerState.isRadiant;
        std::memcpy(ourHeroName, g_pickerState.ourHeroName, sizeof(ourHeroName));
        ourHeroId = g_pickerState.ourHeroId;
        recCount = g_pickerState.recCount;
        for (int i=0;i<recCount && i<10;i++) recs[i] = g_pickerState.recs[i];
    }

    // Заголовок секции + бейдж позиции (всегда виден, даже без определённой
    // позиции); бейдж сокращается/переносится на узкой панели - см. SectionLabelWithPosBadge.
    // Кликабелен только когда игра идёт (до этого DrawHeroSlot в Draft-панели тоже
    // не рендерит попапы позиций - тот же гейт, что и там). Клик меняет НАШУ
    // настоящую позицию (SetOurPosition) - то же самое действие, что и клик по
    // позиции нашего слота в Draft-панели, поэтому держит панели синхронизированными.
    SectionLabelWithPosBadge(ourHeroPicked ? "YOUR HERO" : "RECOMMENDED PICKS", panelW, ourPosition,
                              gameStarted ? SetOurPosition : nullptr);

    ImGui::Spacing();
    ImGui::Separator();

    // -- Waiting overlay ----------------------------------------------------
    if (!gameStarted) {
        ImGui::Spacing();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 cp = ImGui::GetCursorScreenPos();
        float  aW = rW_ref;
        float  aH = 280.f;
        dl->AddRectFilled(cp,{cp.x+aW,cp.y+aH},Ca(kCard2,0.3f));
        dl->AddRect      (cp,{cp.x+aW,cp.y+aH},C(kBorder));

        bool hasPlayer_ = false;
        { std::lock_guard<std::mutex> lk(g_player.mtx); hasPlayer_ = g_player.hasPlayer; }
        const char* msg  = hasPlayer_ ? "Waiting for a game..." : "Enter Friend ID";
        const char* msg2 = hasPlayer_ ? "" : "to start recommendations";

        ImVec2 ts  = ImGui::CalcTextSize(msg);
        ImVec2 ts2 = ImGui::CalcTextSize(msg2);
        float  cy  = cp.y + aH*0.5f - lh*(msg2[0] ? 1.0f : 0.5f);
        dl->AddText({cp.x+(aW-ts.x)*0.5f,  cy},       C(kMuted),       msg);
        if (msg2[0])
            dl->AddText({cp.x+(aW-ts2.x)*0.5f, cy+lh+2.f}, Ca(kMuted,0.6f), msg2);
        ImGui::Dummy({aW,aH});
        return;
    }

    // Заголовки колонок
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+4.f);
    ImGui::TextUnformatted("#  Hero");
    {
        const float WIN_COL_W = 88.f;
        float colX  = rW_ref - WIN_COL_W;
        ImVec2 ts   = ImGui::CalcTextSize("Win Chance");
        float textX = colX + (WIN_COL_W - ts.x)*0.5f;
        ImGui::SameLine(textX);
        ImGui::TextUnformatted("Win Chance");
    }
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Отрисовка строки рекомендации - тонкая обёртка над общим DrawHeroStatRow.
    auto drawPickRow = [&](int rank, const char* name, int heroId, float win, bool highlight,
                           int gamesPlayer, float wrPlayer)
    {
        std::string sub;
        if (gamesPlayer > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "you %dg %.0f%%", gamesPlayer, wrPlayer*100.f);
            sub = buf;
        }
        DrawHeroStatRow(rW_ref, {rank, name, heroId, win, highlight, sub});
    };

    if (ourHeroPicked) {
        // Наш герой + P(win)
        drawPickRow(0, ourHeroName, ourHeroId, winProb, true, 0,0);
    } else {
        // Top-10 рекомендаций
        for (int i=0;i<recCount && i<10;i++) {
            const PickRowGui& r = recs[i];
            drawPickRow(r.rank, r.name, r.heroId, r.winProb, r.rank==1,
                        r.gamesPlayer, r.wrPlayer);
        }
        if (recCount == 0) {
            bool hasPlayer_ = false;
            { std::lock_guard<std::mutex> lk(g_player.mtx); hasPlayer_ = g_player.hasPlayer; }
            ImGui::TextColored(kMuted, hasPlayer_
                ? "  Computing recommendations..."
                : "  Enter Friend ID for recommendations");
        }
    }

    // Легенда цветов
    ImGui::Separator();
    ImGui::Spacing();
    DrawWinRateLegend(rW_ref);
}

// --- Панель "Meta Heroes" (живая стата по героям из STRATZ, без ML) -----------
// Позиция кликабельна всегда, а не только во время игры: вне игры это просто
// превью меты по позиции (SetMetaPreviewPosition, локальный выбор в GUI, без
// livepicks/оркестратора). Как только начинается реальный драфт/матч, панель
// переключается на настоящую позицию из g_pickerState.ourPosition (SetOurPosition,
// как в Draft/Picks) - превью полностью вытесняется настоящей позицией.
void DrawMetaHeroesPanel(float panelW) {
    loadMetaHeroStatsIfNeeded();

    const float rW_ref = panelW;

    bool gameStarted   = false;
    int  ourPosition   = 0;
    bool ourHeroPicked = false;
    int  ourHeroId     = 0;
    char ourHeroName[64] = {};
    {
        std::lock_guard<std::mutex> lk(g_pickerState.mtx);
        gameStarted   = g_pickerState.gameStarted;
        ourPosition   = g_pickerState.ourPosition;
        ourHeroPicked = g_pickerState.ourHeroPicked;
        ourHeroId     = g_pickerState.ourHeroId;
        std::memcpy(ourHeroName, g_pickerState.ourHeroName, sizeof(ourHeroName));
    }
    int displayPos = gameStarted ? ourPosition : s_metaPreviewPos;

    SectionLabelWithPosBadge(ourHeroPicked ? "YOUR HERO" : "META HEROES", panelW, displayPos,
                              gameStarted ? SetOurPosition : SetMetaPreviewPosition);

    ImGui::Spacing();
    ImGui::Separator();

    if (!g_metaHeroStatsLoaded) {
        ImGui::TextColored(kMuted, "  Loading meta stats...");
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+4.f);
    ImGui::TextUnformatted("#  Hero");
    {
        const float WIN_COL_W = 88.f;
        float colX  = rW_ref - WIN_COL_W;
        ImVec2 ts   = ImGui::CalcTextSize("Win Rate");
        float textX = colX + (WIN_COL_W - ts.x)*0.5f;
        ImGui::SameLine(textX);
        ImGui::TextUnformatted("Win Rate");
    }
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Тонкая обёртка над общим DrawHeroStatRow.
    auto drawMetaRow = [&](int rank, const MetaHeroRow& row, bool highlight = false) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d games", row.games);
        DrawHeroStatRow(rW_ref, {rank, row.name, row.heroId, row.winRate, highlight, std::string(buf)});
    };

    if (ourHeroPicked) {
        MetaHeroRow row;
        bool        found = false;
        auto posIt = g_metaHeroLookup.find(displayPos);
        if (posIt != g_metaHeroLookup.end()) {
            auto heroIt = posIt->second.find(ourHeroId);
            if (heroIt != posIt->second.end()) { row = heroIt->second; found = true; }
        }
        if (found) {
            drawMetaRow(0, row, true);
        } else {
            std::string nm = ourHeroName[0] ? ourHeroName : heroDisplayName(ourHeroId, "");
            ImGui::TextColored(kMuted, "  No meta data for %s yet",
                                nm.empty() ? "this hero" : nm.c_str());
        }
    } else {
        auto it = g_metaHeroStats.find(displayPos);
        if (it == g_metaHeroStats.end() || it->second.empty()) {
            ImGui::TextColored(kMuted, "  No data for this position yet");
        } else {
            auto& rows = it->second;
            int n = (int)std::min(rows.size(), (size_t)10);
            for (int i=0;i<n;i++) drawMetaRow(i+1, rows[i]);
        }
    }

    // Легенда цветов - то же оформление, что и в DrawPicksPanel
    ImGui::Separator();
    ImGui::Spacing();
    DrawWinRateLegend(rW_ref);
}

// --- Полоса статуса (Data + Game phase + match ID) ----------------------------
void DrawStatusBar(float fullW) {
    // Снимок состояния player state
    bool   phase1Running, phase1Done, phase1Error;
    char   phase1Msg[256];
    bool   hasPlayer;
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        phase1Running = g_player.phase1Running;
        phase1Done    = g_player.phase1Done;
        phase1Error   = g_player.phase1Error;
        std::memcpy(phase1Msg, g_player.phase1Msg, sizeof(phase1Msg));
        hasPlayer = g_player.hasPlayer;
    }
    GamePhase phase;
    std::string matchId;
    {
        std::lock_guard<std::mutex> lk(g_gameInfo.mtx);
        phase   = g_gameInfo.phase;
        matchId = g_gameInfo.matchId;
    }

    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      sp  = ImGui::GetCursorScreenPos();
    const float H   = 26.f;
    const float lh  = ImGui::GetTextLineHeight();
    const float ty  = sp.y + (H-lh)*0.5f;

    dl->AddRectFilled(sp,{sp.x+fullW,sp.y+H},Ca(kCard2,0.5f));

    const float dotR  = 4.f;
    const float dotCY = ty + lh * 0.5f;

    // Player data dot + text
    float refreshEndX = 0.f;
    float playerEndX = sp.x + 10.f;
    {
        ImVec4 dot1col = !hasPlayer ? kMuted :
            (phase1Error ? kRed : (phase1Done ? kGreen : (phase1Running ? kAmber : kMuted)));
        dl->AddCircleFilled({sp.x+10.f, dotCY}, dotR, C(dot1col));
        char p1buf[320];
        if (!hasPlayer)
            std::snprintf(p1buf, sizeof(p1buf), " Player data: no ID");
        else if (phase1Running)
            std::snprintf(p1buf, sizeof(p1buf), " Player data: fetching...");
        else if (phase1Done)
            std::snprintf(p1buf, sizeof(p1buf), " Player data: ready");
        else if (phase1Error)
            std::snprintf(p1buf, sizeof(p1buf), " Player data: %s",
                          phase1Msg[0] ? phase1Msg : "error");
        else
            std::snprintf(p1buf, sizeof(p1buf), " Player data: pending");
        dl->AddText({sp.x+18.f, ty}, C(kMuted), p1buf);
        ImVec2 p1sz = ImGui::CalcTextSize(p1buf);
        playerEndX = sp.x + 18.f + p1sz.x;
    }
    if (!phase1Running && hasPlayer) {
        float btnSz = H - 2.f;
        float btnX = playerEndX + 4.f;
        float cx = btnX + btnSz * 0.5f;
        float cy = sp.y + H * 0.5f;
        ImGui::SetCursorScreenPos({btnX, sp.y + 1.f});
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  ImVec4(1,1,1,0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,   ImVec4(1,1,1,0.2f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, btnSz * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0,0});
        bool clicked = ImGui::Button("##refresh", {btnSz, btnSz});
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        bool refreshHovered = ImGui::IsItemHovered();
        if (refreshHovered) ImGui::SetTooltip("Reload player data");

        // Иконка refresh: две дуги + наконечники
        constexpr float PI = 3.14159265f;
        float r = btnSz * 0.34f;
        ImU32 col = refreshHovered ? IM_COL32(220,220,220,255) : C(kMuted);

        // Дуга 1: верх (10 → 2 часа по часовой)
        dl->PathArcTo({cx,cy}, r, 200.f*PI/180.f, 340.f*PI/180.f, 12);
        dl->PathStroke(col, 0, 2.f);
        // Дуга 2: низ (4 → 8 часов по часовой)
        dl->PathArcTo({cx,cy}, r, 20.f*PI/180.f, 160.f*PI/180.f, 12);
        dl->PathStroke(col, 0, 2.f);

        // Наконечник на 2 часа (конец дуги 1)
        auto drawArrow = [&](float angleDeg) {
            float ae  = angleDeg * PI / 180.f;
            float dir = ae + PI * 0.5f;
            float tx  = cx + r * cosf(ae);
            float tty = cy + r * sinf(ae);
            float len = r * 0.5f, hw = len * 0.45f;
            float bx  = tx - len * cosf(dir), by = tty - len * sinf(dir);
            dl->AddTriangleFilled({tx, tty},
                {bx + hw*cosf(dir+PI/2.f), by + hw*sinf(dir+PI/2.f)},
                {bx + hw*cosf(dir-PI/2.f), by + hw*sinf(dir-PI/2.f)}, col);
        };
        drawArrow(340.f);
        drawArrow(160.f);

        if (clicked) {
            long long aid = 0;
            { std::lock_guard<std::mutex> lk(g_player.mtx); aid = g_player.accountId; }
            if (aid > 0) startPhase1(aid);
        }
        refreshEndX = btnX + btnSz;
    }

    // Game phase dot + text
    ImVec4 dot2col = (phase == GamePhase::DRAFT   ? kAmber  :
                      phase == GamePhase::INGAME  ? kGreen  :
                      phase == GamePhase::POSTGAME? kMuted  : kMuted);
    float x2 = refreshEndX > 0.f
        ? refreshEndX + 12.f
        : playerEndX + 12.f;
    dl->AddCircleFilled({x2+4.f, dotCY}, dotR, C(dot2col));
    const char* phaseStr = (phase == GamePhase::IDLE)     ? "Waiting for game" :
                           (phase == GamePhase::DRAFT)    ? "Draft Phase" :
                           (phase == GamePhase::INGAME)   ? "In Game" : "Post Game";
    char p2buf[64];
    std::snprintf(p2buf, sizeof(p2buf), " Game: %s", phaseStr);
    dl->AddText({x2+12.f, ty}, C(kMuted), p2buf);

    // Match ID (right side)
    if (!matchId.empty()) {
        char mbuf[64];
        std::snprintf(mbuf, sizeof(mbuf), "match %s", matchId.c_str());
        ImVec2 mts = ImGui::CalcTextSize(mbuf);
        dl->AddText({sp.x+fullW-mts.x-8.f, ty}, Ca(kMuted,0.6f), mbuf);
    }

    // Зафиксировать курсор после статус-бара (Button внутри сдвигает layout)
    ImGui::SetCursorScreenPos({sp.x, sp.y + H});
    ImGui::Dummy({fullW, 0.f});
}

// --- Шапка: логотип [D] + заголовок + карточка игрока / ввод Friend ID --------
// Возвращает высоту шапки (фиксированная 60 - высота шапки не растёт; в
// режиме ввода Friend ID содержимое карточки сжимается по вертикали, чтобы
// уместиться в те же 60px, см. ниже).
float DrawHeader(float fullW) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      hs  = ImGui::GetCursorScreenPos();
    const float lh  = ImGui::GetTextLineHeight();
    const float H   = 60.f;

    // Снимок состояния player info (нужен уже здесь, чтобы посчитать ширину
    // карточки под режим ввода - см. ниже)
    long long  accountId;
    char       pname[128];
    bool       hasPlayer;
    bool       phase1Running;
    {
        std::lock_guard<std::mutex> lk(g_player.mtx);
        accountId    = g_player.accountId;
        hasPlayer    = g_player.hasPlayer;
        phase1Running= g_player.phase1Running;
        std::memcpy(pname, g_player.name, sizeof(pname));
    }
    const bool inputMode = !hasPlayer || s_editMode;

    // -- [D] logo box (на всю высоту шапки) -----------------------------
    const float LOGO_W = H;
    ImVec2 logoPos = {hs.x, hs.y};

    dl->AddRectFilled(logoPos, {logoPos.x+LOGO_W, logoPos.y+H}, C(kCard));
    dl->AddRect      (logoPos, {logoPos.x+LOGO_W, logoPos.y+H}, C(kBorder));
    ImVec2 sts = ImGui::CalcTextSize("[D]");
    dl->AddText({logoPos.x+(LOGO_W-sts.x)/2.f,
                 logoPos.y+(H-sts.y)/2.f - 1.f}, C(kText), "[D]");

    // Invisible button over the [D] box
    ImGui::SetCursorScreenPos(logoPos);
    if (ImGui::InvisibleButton("##logo_btn",{LOGO_W, H})) {
        if (g_Hwnd) {
            if (IsIconic(g_Hwnd)) ShowWindow(g_Hwnd, SW_RESTORE);
            SetForegroundWindow(g_Hwnd);
            BringWindowToTop(g_Hwnd);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Bring window to front");

    // -- Title (вертикально по центру шапки, две строки) -------------------
    float tx       = hs.x + LOGO_W + 10.f;
    float textH    = lh * 2.f + 2.f;
    float titleY   = hs.y + (H - textH) * 0.5f;
    dl->AddText({tx, titleY},            C(kText),  "Dota_Drafter");
    dl->AddText({tx, titleY + lh + 2.f}, C(kMuted), "Dota 2 Draft Analyzer - Live Overlay");

    // -- Player card (right side) ------------------------------------------
    const float INPUT_W = 120.f;
    float CW = 240.f;
    if (inputMode) {
        // В режиме ввода карточка должна вместить label + input + Set (+x) -
        // иначе кнопки вылезают за нарисованную рамку карточки (240px под
        // это не рассчитан, он только под аватар+имя в режиме показа).
        const ImGuiStyle& style   = ImGui::GetStyle();
        float setBtnW = ImGui::CalcTextSize("Set").x + style.FramePadding.x * 2.f;
        bool  showCancel = s_editMode && hasPlayer;
        float xBtnW   = showCancel ? (ImGui::CalcTextSize("x").x + style.FramePadding.x * 2.f) : 0.f;
        float neededW = 52.f + INPUT_W + 4.f + setBtnW + (showCancel ? (4.f + xBtnW) : 0.f) + 12.f;
        CW = (std::max)(CW, neededW);
    }
    // Не даём карточке залезть на заголовок, если окно сужено сильнее,
    // чем предполагает фиксированная раскладка (см. kMinClientW/H).
    float titleEndX = tx + ImGui::CalcTextSize("Dota_Drafter").x + 20.f;
    float cx = (std::max)(titleEndX, hs.x + fullW - CW);

    // Карточка - настоящее дочернее окно ImGui (как ##draft/##picks/##meta
    // ниже), а не просто нарисованный прямоугольник: любой виджет внутри
    // (InputText/Button) автоматически обрезается по его границе и не может
    // визуально вылезти наружу, даже если наш расчёт CW окажется неточным.
    ImGui::SetCursorScreenPos({cx, hs.y});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##playercard", {CW, H}, true, ImGuiWindowFlags_NoScrollbar);
    dl = ImGui::GetWindowDrawList();

    // Avatar box (36×36, вертикально по центру карточки - H всегда 60)
    const float AVS  = 36.f;
    const float avY  = hs.y + (H - AVS) * 0.5f;
    dl->AddRectFilled({cx+8, avY}, {cx+8+AVS, avY+AVS}, C(kCard2));
    dl->AddRect      ({cx+8, avY}, {cx+8+AVS, avY+AVS}, C(kBorder));

    if (inputMode) {
        // -- Input mode ----------------------------------------------------
        // Не увеличиваем шапку - вместо этого сжимаем отступы и высоту строки
        // input/Set/x (уменьшенный FramePadding), чтобы label + обе строки
        // гарантированно уместились в фиксированные 60px карточки.
        ImVec2 avts = ImGui::CalcTextSize("ID");
        dl->AddText({cx+8+(AVS-avts.x)/2.f, avY+(AVS-avts.y)/2.f},
                    C(kMuted), "ID");

        const float rowTopY = hs.y + 6.f;
        dl->AddText({cx+52.f, rowTopY}, C(kMuted), "Enter Friend ID:");

        // InputText + Set button (сжатый FramePadding.y, чтобы строка была ниже)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 1.f));
        ImGui::SetCursorScreenPos({cx+52.f, rowTopY+lh+2.f});
        ImGui::SetNextItemWidth(INPUT_W);
        bool commit = ImGui::InputText("##steamid", s_inputBuf, sizeof(s_inputBuf),
                                       ImGuiInputTextFlags_CharsNoBlank |
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, 4.f);
        bool setBtn = ImGui::Button("Set");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save Friend ID and load player data");

        if ((commit || setBtn) && s_inputBuf[0] != '\0') {
            long long newId = 0;
            try { newId = std::stoll(s_inputBuf); } catch (...) {}
            if (newId > 0) {
                // Update player state immediately
                {
                    std::lock_guard<std::mutex> lk(g_player.mtx);
                    g_player.accountId     = newId;
                    g_player.hasPlayer     = true;
                    g_player.idChanged     = true;   // сигнал оркестратору
                    g_player.phase1Done    = false;
                    g_player.phase1Error   = false;
                    std::snprintf(g_player.name, sizeof(g_player.name),
                                  "ID %lld", newId);
                }
                s_editMode    = false;
                s_inputBuf[0] = '\0';
                startPhase1(newId);
            }
        }

        // Cancel button (only in edit mode with existing player)
        if (s_editMode && hasPlayer) {
            ImGui::SameLine(0,4.f);
            if (ImGui::SmallButton("x")) s_editMode = false;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cancel");
        }
        ImGui::PopStyleVar(); // FramePadding (input/Set/x row)

    } else {
        // -- Display mode --------------------------------------------------
        if (g_avatarSRV) {
            dl->AddImage((ImTextureID)g_avatarSRV,
                         {cx+8.f, avY}, {cx+8.f+AVS, avY+AVS});
        } else {
            char av[3] = {pname[0] ? pname[0] : '?',
                          pname[1] ? pname[1] : '\0', '\0'};
            ImVec2 avts = ImGui::CalcTextSize(av);
            dl->AddText({cx+8+(AVS-avts.x)/2.f, avY+(AVS-avts.y)/2.f},
                        C(kMuted), av);
        }

        // Player name + ID (вертикально по центру карточки)
        float nameBlockH = lh * 2.f + 2.f;
        float nameY      = hs.y + (H - nameBlockH) * 0.5f;
        dl->AddText({cx+52.f, nameY},            C(kText), pname);
        char idStr[32];
        std::snprintf(idStr, sizeof(idStr), "ID %lld", accountId);
        dl->AddText({cx+52.f, nameY+lh+2.f}, C(kMuted), idStr);

        // Invisible button over name area to trigger edit
        ImGui::SetCursorScreenPos({cx+52.f, nameY});
        if (ImGui::InvisibleButton("##player_edit",{CW-60.f,lh*2.f+8.f})) {
            s_editMode = true;
            std::snprintf(s_inputBuf, sizeof(s_inputBuf), "%lld", accountId);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to change player ID");

        // Phase-1 spinner
        if (phase1Running) {
            static float spin = 0.f;
            spin += ImGui::GetIO().DeltaTime * 3.f;
            char sp[4] = {"|/-\\"[(int)(spin)%4]};
            dl->AddText({cx+CW-20.f, hs.y+H*0.5f-lh*0.5f},
                        C(kAmber), sp);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos({hs.x, hs.y + H});
    ImGui::Dummy({fullW, 0.f});
    return H;
}
