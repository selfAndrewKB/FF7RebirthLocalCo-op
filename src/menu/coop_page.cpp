#include "menu/coop_page.h"

#define RC_UNREAL_DISABLE_PROPERTY_DEPRECATION_WARNINGS
#define RC_UNREAL_DISABLE_CLASS_DEPRECATION_WARNINGS
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/UObjectGlobals.hpp>

#include "facts/menu.h"
#include "menu/game_ui.h"
#include "platform/build_value.h"
#include "platform/command_registry.h"
#include "platform/entry_redirect.h"
#include "platform/game_array.h"
#include "platform/guarded.h"
#include "platform/guarded_read.h"
#include "platform/image.h"
#include "platform/log.h"
#include "platform/settings.h"
#include "platform/verified_code.h"
#include "players/unreal_access.h"
#include "players/weak_object.h"

#include <Unreal/UObject.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lss::menu::coop_page {

namespace {

namespace rules    = coop_menu_rules;
namespace settings = platform::settings;
namespace weak     = players::weak_object;

using game_ui::At;
using platform::GameArray;
using platform::SourceString;

using GetValueFn   = std::int32_t (*)(void* page, std::uint8_t key, std::int32_t row);
using SetValueFn   = void* (*)(void* page, std::uint8_t category, std::int32_t value);
using CreateFn     = void* (*)(void* world, void* cls, std::uint8_t inGame);
using AddToRootFn  = bool (*)(void* widget, std::int32_t zOrder);
using InRootFn     = bool (*)(void* widget);
using FindOrAddFn  = void* (*)(void* map, const std::uint8_t* key);
using InfoCtorFn   = void* (*)(void* info);
using InfoDtorFn   = void (*)(void* info);
using InfoMoveFn   = void* (*)(void* to, void* from);
using GrowFn       = void (*)(void* array);
using MoveAssignFn = void* (*)(void* to, void* from);
using SetCountFn   = void (*)(void* box, std::int32_t count);
using RefreshFn    = void (*)(void* box);
using GetWorldFn   = void* (*)(void* object);
using RemoveFn     = void* (*)(void* widget);

/// The game's functions, found at install.
struct Game
{
    std::uintptr_t    getValueAt = 0, setValueAt = 0;  // where the redirects go
    game_ui::AssignFn assign     = nullptr;
    CreateFn          create     = nullptr;
    AddToRootFn       addToRoot  = nullptr;
    InRootFn          inRoot     = nullptr;
    FindOrAddFn       findOrAdd  = nullptr;
    InfoCtorFn        infoCtor   = nullptr;
    InfoDtorFn        infoDtor   = nullptr;
    InfoMoveFn        infoMove   = nullptr;
    GrowFn            growInfos  = nullptr;
    GrowFn            growTexts  = nullptr;
    MoveAssignFn      emptyInfos = nullptr;  // the rows' move-assign, from an empty array
    SetCountFn        setCount   = nullptr;
    RefreshFn         refresh    = nullptr;
    RemoveFn          remove     = nullptr;  // the page out of the overlay
};

// Set at install and on the first tick, then only read.
Game                         g;
platform::EntryRedirect      g_getValue, g_setValue, g_reset;
std::vector<rules::Category> g_page;  // the page's categories and rows

// Game thread.
std::uint64_t g_pageWeak = 0;     // the mod's page, kept while alive (a weak value: 0 none)
bool          g_filled = false;   // the page holds the mod's data (false across a re-add, whose construct puts the game's back,
                                  // and once the page is gone: its address may become the System window's own page)
bool          g_open   = false;   // the page is open
bool          g_failed = false;   // a fault while writing the page: the row opens nothing until the next start
std::uint64_t g_commits = 0;
/// A value the page committed, run by the next tick (an option's command may resize the window: never inside the
/// page's key event). The last commit per option wins.
std::array<std::optional<int>, settings::kOptions.size()> g_queued{};
/// The value the page last showed per option (a commit equal to it changes nothing when the current word is unknown).
std::array<int, settings::kOptions.size()> g_shown{};
bool g_sliders   = false;  // the long rows are the game's slider rows (menu/coop_slider placed)
bool g_resetting = false;  // the game's Reset All runs on the mod's page: the value read answers the defaults
/// The option and value the page's value read last answered, for a slider row's text (menu/coop_slider), taken once.
struct Answered
{
    int          option = -1;
    std::int32_t value  = 0;
};
std::optional<Answered> g_answered;

void* PageNow() noexcept
{
    return g_pageWeak != 0 ? weak::ObjectOf(g_pageWeak) : nullptr;
}

// --- Values: the settings table's options through their console commands ------------------------------------------

const control::Entry* EntryOf(int option) noexcept
{
    return control::Global().Find(settings::kOptions[static_cast<std::size_t>(option)]->key);
}

std::optional<std::wstring> CurrentWord(int option)
{
    const control::Entry* entry = EntryOf(option);
    return entry != nullptr && entry->get ? rules::WordOfReading(entry->get()) : std::nullopt;
}

/// The option a row of the page shows, by the screen's category key and row index.
std::optional<int> OptionAt(std::uint8_t key, std::int32_t row) noexcept
{
    for (const rules::Category& category : g_page)
    {
        if (category.key == key && row >= 0 && row < static_cast<std::int32_t>(category.options.size()))
        {
            return category.options[static_cast<std::size_t>(row)];
        }
    }
    return std::nullopt;
}

/// The game's value read: the mod's rows answer their option's word's place (a commit not yet run: its value).
std::int32_t GetValue(void* page, std::uint8_t key, std::int32_t row)
{
    g_answered.reset();  // every read, any page: an answer belongs only to the read just made
    if (g_filled && IsPage(page))
    {
        try
        {
            if (const auto option = OptionAt(key, row))
            {
                const auto              i     = static_cast<std::size_t>(*option);
                const settings::Option& shown = *settings::kOptions[i];
                const auto              word  = CurrentWord(*option);
                const int value = g_resetting ? rules::DefaultValue(shown)
                                  : g_queued[i] ? *g_queued[i]
                                                : (word ? rules::ValueOf(shown, *word).value_or(0) : 0);
                if (!g_resetting)
                {
                    g_shown[i] = value;  // a reset's default is not yet shown: its commit must not read as unchanged
                }
                g_answered = Answered{*option, value};
                if (platform::Enabled(platform::Level::Trace))
                {
                    platform::LogTrace(L"Co-op page: {} shows {} (the option reads {})", shown.key, value,
                                       word ? *word : std::wstring(L"nothing readable"));
                }
                return value;
            }
        }
        catch (...)
        {
            return 0;
        }
    }
    return reinterpret_cast<GetValueFn>(g_getValue.Original())(page, key, row);
}

/// The game's value commit: a mod row's value is queued for its option's command, run by the next tick (an unchanged
/// value does nothing: the game re-commits current values at its construct).
void* SetValue(void* page, std::uint8_t category, std::int32_t value)
{
    const auto option = rules::OptionOf(category);
    if (!option)
    {
        return reinterpret_cast<SetValueFn>(g_setValue.Original())(page, category, value);
    }
    try
    {
        const auto              i   = static_cast<std::size_t>(*option);
        const settings::Option& row = *settings::kOptions[i];
        if (!g_open || !IsPage(page))
        {
            platform::LogDebug(L"Co-op page: a commit of {} to {} outside the open page; nothing changed", row.key, value);
            return page;
        }
        const auto word = rules::WordOf(row, value);
        if (!word)
        {
            platform::LogWarning(L"Co-op page: {} has no value {}; nothing changed", row.key, value);
            return page;
        }
        const auto current = g_queued[i] ? rules::WordOf(row, *g_queued[i]) : CurrentWord(*option);
        if ((current && *current == *word) || (!current && value == g_shown[i]))
        {
            platform::LogDebug(L"Co-op page: {} stays {}", row.key, *word);
            return page;
        }
        g_queued[i] = value;
        platform::LogDebug(L"Co-op page: {} to {}, on the next tick", row.key, *word);
    }
    catch (...)
    {
        platform::LogWarning(L"Co-op page: a commit raised an exception; nothing changed");
    }
    return page;
}

using ResetFn = std::uint64_t (*)(void* page, std::uint64_t a2, std::uint64_t a3);

/// The game's Reset All after its Yes: on the mod's page the game's own row loop runs with the value read answering each
/// option's default, so its commits carry the defaults into the mod's (queued, run and saved as any change); then the
/// rows are drawn again (the reset writes a slider's label as a plain number). Elsewhere the game's alone.
std::uint64_t ResetConfirm(void* page, std::uint64_t a2, std::uint64_t a3)
{
    const auto original = reinterpret_cast<ResetFn>(g_reset.Original());
    if (!g_open || !g_filled || !IsPage(page))
    {
        return original(page, a2, a3);
    }
    struct Resetting
    {
        Resetting() { g_resetting = true; }
        ~Resetting() { g_resetting = false; }
    };
    std::uint64_t result = 0;
    {
        const Resetting resetting;
        result = original(page, a2, a3);
    }
    void* const rows = platform::ReadPointer(page, facts::kOptionsRowList);
    const bool  drawn = rows != nullptr && platform::CallGuarded(g.refresh, rows);
    platform::Log(L"Co-op page: Reset All: each option of the category back to its default{}",
                  drawn ? L"" : L" (the rows could not be drawn again)");
    return result;
}

/// The queued commits, each through its option's console command (as the console and the keys run them), then saved.
void RunQueued() noexcept
try
{
    for (std::size_t i = 0; i < g_queued.size(); ++i)
    {
        if (!g_queued[i])
        {
            continue;
        }
        const int value = *g_queued[i];
        g_queued[i].reset();
        const settings::Option& row   = *settings::kOptions[i];
        const auto              word  = rules::WordOf(row, value);
        const control::Entry*   entry = EntryOf(static_cast<int>(i));
        if (!word || entry == nullptr || !entry->set)
        {
            platform::LogWarning(L"Co-op page: {} has no command to take {}; nothing changed", row.key, value);
            continue;
        }
        const control::Outcome outcome = entry->set(rules::CommandValue(entry->shape, *word));
        ++g_commits;
        platform::Log(L"Co-op page: {} set to {} ({} {}){}{}", row.label, row.wordLabels[static_cast<std::size_t>(value)],
                      row.key, *word, outcome.ok ? L"" : L"; refused: ", outcome.ok ? std::wstring() : outcome.detail);
        if (outcome.ok)
        {
            settings::Save(row, *word);  // the next start reads the choice (logged either way)
        }
    }
}
catch (...)
{
    platform::LogWarning(L"Co-op page: running a change raised an exception; the rest waits for the next tick");
}

// --- Writing the page -------------------------------------------------------------------------------------------

/// Room for one more element: the game's growth reads the wanted count from Num, so Num is raised for the call and put
/// back whatever happens (an element is counted only once built). False if it faulted.
bool Grow(GrowFn grow, GameArray* array) noexcept
{
    ++array->num;
    __try
    {
        grow(array);
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        --array->num;
        return false;
    }
    --array->num;
    return true;
}

/// `text` appended to a TArray<FString> of the game's, grown first and counted once built. False if it did not grow.
bool AppendString(GameArray* strings, const wchar_t* text)
{
    if (strings->num + 1 > strings->max && !Grow(g.growTexts, strings))
    {
        return false;
    }
    if (strings->num >= strings->max || strings->data == nullptr)
    {
        return false;
    }
    std::uint8_t* element = static_cast<std::uint8_t*>(strings->data) + 16 * static_cast<std::size_t>(strings->num);
    std::memset(element, 0, 16);
    const GameArray source = SourceString(text);
    g.assign(element, &source);
    ++strings->num;
    return true;
}

/// One List row appended to `rows` (TArray<FOptionInfo>), built the way the game's construct builds one; each element is
/// built in room grown first and counted only once built, so a fault never leaves an unbuilt element counted. Raw
/// calls: run inside FillPage's guard. Null when done, else what failed.
const wchar_t* AppendRow(GameArray* rows, int option)
{
    const settings::Option&  row = *settings::kOptions[static_cast<std::size_t>(option)];
    alignas(16) std::uint8_t info[facts::kOptionInfoSize]{};
    g.infoCtor(info);
    // A row longer than the game's longest list is the game's slider over its values' places (menu/coop_slider).
    const bool slider                = rules::IsSlider(row) && g_sliders;
    info[facts::kOptionInfoKind]     = slider ? facts::kOptionKindRange : facts::kOptionKindList;
    info[facts::kOptionInfoCategory] = rules::RowByte(option);
    std::memset(info + facts::kOptionInfoMin, 0, 8);  // Min, Max (the constructor leaves them)
    if (slider)
    {
        const auto last = static_cast<std::int32_t>(row.wordLabels.size()) - 1;
        std::memcpy(info + facts::kOptionInfoMax, &last, sizeof last);
    }
    const GameArray text = SourceString(row.label);
    const GameArray help = SourceString(row.menuHelp);
    g.assign(info + facts::kOptionInfoText, &text);
    g.assign(info + facts::kOptionInfoHelp, &help);
    // Each value with the row's help: the value list's cells are reused, and a cell past ValueHelps' end keeps the help
    // it showed last (a game row's).
    auto* values = reinterpret_cast<GameArray*>(info + facts::kOptionInfoValues);
    auto* helps  = reinterpret_cast<GameArray*>(info + facts::kOptionInfoValueHelps);
    for (const wchar_t* name : row.wordLabels)
    {
        if (!AppendString(values, name) || !AppendString(helps, row.menuHelp))
        {
            g.infoDtor(info);
            return L"a value list did not grow";
        }
    }
    if ((rows->num + 1 > rows->max && !Grow(g.growInfos, rows)) || rows->num >= rows->max || rows->data == nullptr)
    {
        g.infoDtor(info);
        return L"a category's rows did not grow";
    }
    g.infoMove(static_cast<std::uint8_t*>(rows->data) + facts::kOptionInfoSize * static_cast<std::size_t>(rows->num), info);
    ++rows->num;
    g.infoDtor(info);  // frees nothing after the move
    return nullptr;
}

/// The page's categories, rows and left list replaced with the mod's, through the game's own helpers. Null when done,
/// else the step that failed.
const wchar_t* FillPageRaw(void* page)
{
    auto* labels = reinterpret_cast<GameArray*>(At(page, facts::kOptionsLabels));
    auto* order  = reinterpret_cast<GameArray*>(At(page, facts::kOptionsOrder));
    if (order->max < static_cast<std::int32_t>(g_page.size()) || order->data == nullptr)
    {
        return L"the category order has no room";
    }
    for (const rules::Category& category : g_page)
    {
        if (category.key >= labels->num || labels->data == nullptr)
        {
            return L"a category's label is missing";
        }
        const GameArray label = SourceString(category.text.label);
        g.assign(static_cast<std::uint8_t*>(labels->data) + 16 * static_cast<std::size_t>(category.key), &label);
        void* const infos = g.findOrAdd(At(page, facts::kOptionsItems), &category.key);
        if (infos == nullptr)
        {
            return L"a category's rows could not be found";
        }
        const GameArray help = SourceString(category.text.help);
        g.assign(At(infos, facts::kOptionInfosHelp), &help);
        // The game's construct only appends rows and finds categories by key (research/RE-coop-page-facts.md §2), so
        // after a re-add it adds its rows behind the mod's: every row goes, then the mod's are added.
        GameArray empty{};
        g.emptyInfos(At(infos, facts::kOptionInfosRows), &empty);
        auto* rows = reinterpret_cast<GameArray*>(At(infos, facts::kOptionInfosRows));
        for (const int option : category.options)
        {
            if (const wchar_t* failed = AppendRow(rows, option))
            {
                return failed;
            }
        }
    }
    for (std::size_t i = 0; i < g_page.size(); ++i)
    {
        static_cast<std::uint8_t*>(order->data)[i] = g_page[i].key;
    }
    order->num = static_cast<std::int32_t>(g_page.size());
    *reinterpret_cast<std::int32_t*>(At(page, facts::kOptionsCategoryIndex)) = 0;
    *reinterpret_cast<std::int32_t*>(At(page, facts::kOptionsRowIndex))      = 0;
    void* const box = *reinterpret_cast<void**>(At(page, facts::kOptionsCategoryList));
    if (box == nullptr)
    {
        return L"the category list is missing";
    }
    g.setCount(box, order->num);
    g.refresh(box);
    return nullptr;
}

const wchar_t* FillPage(void* page) noexcept
{
    __try
    {
        return FillPageRaw(page);
    }
    __except (platform::GuardFilter(GetExceptionCode()))
    {
        return L"a fault while writing it";
    }
}

/// The page's own title (its category title's text block, which its construct writes "Options" into) set to the mod's,
/// through the game's reflected SetText; the widgets found by their property names (their offsets differ between the
/// builds). Logged once if it cannot be: the page then reads Options.
void SetTitle(void* page) noexcept
try
{
    using RC::Unreal::UFunction;
    using RC::Unreal::UObject;
    // Found once (a native function: it lives as long as the game); looked for again until found.
    static UFunction*                  setText = nullptr;
    static std::optional<std::int32_t> inText;
    static bool                        logged = false;
    if (setText == nullptr || !inText)
    {
        setText = RC::Unreal::UObjectGlobals::StaticFindObject<UFunction*>(nullptr, nullptr,
                                                                           L"/Script/EndGame.EndTextBlock:SetText");
        inText  = setText != nullptr ? players::unreal::ParamOffset(setText, L"InText", sizeof(RC::Unreal::FString))
                                     : std::nullopt;
    }
    UObject* const  owner    = static_cast<UObject*>(page);
    UObject** const category = owner->GetValuePtrByPropertyNameInChain<UObject*>(STR("CategoryTitle"));
    UObject** const text = category != nullptr && *category != nullptr
                               ? (*category)->GetValuePtrByPropertyNameInChain<UObject*>(STR("Txt_MainTitle"))
                               : nullptr;
    const bool set = text != nullptr && *text != nullptr && setText != nullptr && inText &&
                     players::unreal::CallWithString(
                         *text, setText, *inText, rules::kTitle, [](std::uint8_t*) {}, [](const std::uint8_t*) {});
    if (!set && !logged)
    {
        logged = true;
        platform::LogWarning(L"Co-op page: its title could not be set (the title widget or SetText is not what the "
                             L"research read); it reads Options");
    }
}
catch (...)
{
}

/// The page's categories: the table's; without the slider rows, the rows longer than the game's longest list are left
/// out (its value list cannot scroll), and a category left empty by that.
std::vector<rules::Category> PageOf(bool sliders)
{
    std::vector<rules::Category> page = rules::BuildPage();
    if (sliders)
    {
        return page;
    }
    for (rules::Category& category : page)
    {
        std::erase_if(category.options, [](int option) {
            return rules::IsSlider(*settings::kOptions[static_cast<std::size_t>(option)]);
        });
    }
    std::erase_if(page, [](const rules::Category& category) { return category.options.empty(); });
    return page;
}

}  // namespace

bool Install(const platform::GameImage& image) noexcept
try
{
    namespace f = facts;
    const auto getValue   = platform::FindFunction(image, f::kOptionsGetValue);
    const auto setValue   = platform::FindFunction(image, f::kOptionsSetValue);
    const auto create     = platform::FindFunction(image, f::kOptionsCreate);
    const auto addToRoot  = platform::FindFunction(image, f::kAddToRootOverlay);
    const auto inRoot     = platform::FindFunction(image, f::kIsInRootOverlay);
    const auto findOrAdd  = platform::FindFunction(image, f::kOptionItemsFindOrAdd);
    const auto infoCtor   = platform::FindFunction(image, f::kOptionInfoCtor);
    const auto infoDtor   = platform::FindFunction(image, f::kOptionInfoDtor);
    const auto infoMove   = platform::FindFunction(image, f::kOptionInfoMove);
    const auto growInfos  = platform::FindFunction(image, f::kOptionInfosGrow);
    const auto growTexts  = platform::FindFunction(image, f::kStringArrayGrow);
    const auto emptyInfos = platform::FindFunction(image, f::kOptionInfosMoveAssign);
    const auto setCount   = platform::FindFunction(image, f::kScrollBoxSetCount);
    const auto refresh    = platform::FindFunction(image, f::kScrollBoxRefresh);
    const auto remove     = platform::FindFunction(image, f::kRemoveFromParent);
    g_page                = PageOf(false);  // the slider rows join once menu/coop_slider is placed (UseSliders)
    // game_ui is installed first (menu/coop_menu): the page writes its strings with the game's copy.
    if (!getValue || !setValue || !create || !addToRoot || !inRoot || !findOrAdd || !infoCtor || !infoDtor || !infoMove ||
        !growInfos || !growTexts || !emptyInfos || !setCount || !refresh || !remove || game_ui::Assign() == nullptr ||
        g_page.empty())
    {
        return false;
    }
    g.getValueAt = *getValue;
    g.setValueAt = *setValue;
    g.assign     = game_ui::Assign();
    g.create     = reinterpret_cast<CreateFn>(*create);
    g.addToRoot  = reinterpret_cast<AddToRootFn>(*addToRoot);
    g.inRoot     = reinterpret_cast<InRootFn>(*inRoot);
    g.findOrAdd  = reinterpret_cast<FindOrAddFn>(*findOrAdd);
    g.infoCtor   = reinterpret_cast<InfoCtorFn>(*infoCtor);
    g.infoDtor   = reinterpret_cast<InfoDtorFn>(*infoDtor);
    g.infoMove   = reinterpret_cast<InfoMoveFn>(*infoMove);
    g.growInfos  = reinterpret_cast<GrowFn>(*growInfos);
    g.growTexts  = reinterpret_cast<GrowFn>(*growTexts);
    g.emptyInfos = reinterpret_cast<MoveAssignFn>(*emptyInfos);
    g.setCount   = reinterpret_cast<SetCountFn>(*setCount);
    g.refresh    = reinterpret_cast<RefreshFn>(*refresh);
    g.remove     = reinterpret_cast<RemoveFn>(*remove);
    return true;
}
catch (...)
{
    return false;
}

bool Place(const platform::GameImage& image) noexcept
{
    const auto getFirst = image.EntryInstruction(facts::kOptionsGetValue.address,
                                                 facts::kOptionsGetValue.entryBytes.first(facts::kOptionsGetValueFirst));
    const auto setFirst = image.EntryInstruction(facts::kOptionsSetValue.address,
                                                 facts::kOptionsSetValue.entryBytes.first(facts::kOptionsSetValueFirst));
    const bool get = platform::RunningBuild() == platform::BuildIndex::V1005
                         ? g_getValue.InstallAtSecondInstruction(g.getValueAt, facts::kOptionsGetValueFirst1005,
                                                                 facts::kOptionsGetValueSecond1005,
                                                                 reinterpret_cast<std::uintptr_t>(&GetValue))
                         : g_getValue.Install(g.getValueAt, getFirst, reinterpret_cast<std::uintptr_t>(&GetValue));
    const bool placed = get && g_setValue.Install(g.setValueAt, setFirst, reinterpret_cast<std::uintptr_t>(&SetValue));
    // Reset All: 1.005 only (1.004's entry crosses a block and its second instruction is too short to take the jump; there
    // Reset All keeps every value of the page).
    if (placed && platform::RunningBuild() == platform::BuildIndex::V1005)
    {
        const auto reset    = platform::FindFunction(image, facts::kOptionsResetConfirm);
        bool       followed = false;
        if (reset)
        {
            const auto first = image.EntryInstruction(
                facts::kOptionsResetConfirm.address,
                facts::kOptionsResetConfirm.entryBytes.first(facts::kOptionsResetConfirmFirst));
            followed = g_reset.Install(*reset, first, reinterpret_cast<std::uintptr_t>(&ResetConfirm));
        }
        if (!followed)
        {
            platform::LogWarning(L"Co-op page: Reset All could not be followed; on the page it keeps every value");
        }
    }
    return placed;
}

void Unplace() noexcept
{
    g_reset.Restore();
    g_setValue.Restore();
    g_getValue.Restore();
    g_answered.reset();  // no read answers any more: a slider's value text redirect left in place finds none
}

const std::vector<coop_menu_rules::Category>& Categories() noexcept
{
    return g_page;
}

void* Prepare(void* host, std::uint8_t inGame, bool& inRoot) noexcept
{
    void*        page    = PageNow();
    std::uint8_t madeFor = inGame;
    if (page != nullptr && platform::Read(At(page, facts::kOptionsInGame), madeFor) && madeFor != inGame)
    {
        // Closed, so out of the overlay and held by nothing: the collector takes it.
        platform::LogDebug(L"Co-op page: the page made for the {} is left; a new one for the {}",
                           madeFor != 0 ? L"main menu" : L"title", inGame != 0 ? L"main menu" : L"title");
        g_pageWeak = 0;
        g_filled   = false;
        page       = nullptr;
    }
    if (page == nullptr)
    {
        // Looked up at each make (rare): a Blueprint class may be collected with a level.
        void* const cls = RC::Unreal::UObjectGlobals::StaticFindObject<RC::Unreal::UClass*>(nullptr, nullptr,
                                                                                            facts::kOptionsClassPath);
        void*      world    = nullptr;
        const auto getWorld = reinterpret_cast<GetWorldFn>(
            platform::ReadPointer(platform::ReadPointer(host, 0), static_cast<std::uint32_t>(facts::kObjectGetWorldSlot)));
        if (cls == nullptr || getWorld == nullptr || !platform::CallGuarded(getWorld, world, host) || world == nullptr ||
            !platform::CallGuarded(g.create, page, world, cls, inGame) || page == nullptr)
        {
            platform::LogWarning(L"Co-op page: the game's Options screen could not be made ({}); the row opens nothing",
                                 cls == nullptr ? L"its class is not loaded" : L"its creation failed");
            return nullptr;
        }
        g_pageWeak = weak::WeakOf(page);
        g_filled   = false;
        if (g_pageWeak == 0)
        {
            platform::LogWarning(L"Co-op page: the page made has no weak pointer; the row opens nothing");
            return nullptr;
        }
        platform::Log(L"Co-op page: made the mod's own Options screen (not the one the System window keeps)");
    }
    inRoot = false;
    if (!platform::CallGuarded(g.inRoot, inRoot, page))
    {
        return nullptr;
    }
    std::uint8_t state = 0;
    if (inRoot && (!platform::Read(At(page, facts::kOptionsState), state) || state != 0))
    {
        // Still playing its closing animation: its own tick takes it out of the overlay when that ends.
        platform::LogDebug(L"Co-op page: the page is still closing (state {}); not opened again yet", state);
        return nullptr;
    }
    if (!inRoot)
    {
        g_filled = false;  // its construct runs when it is added, over the game's data
    }
    return page;
}

bool Fill(void* page) noexcept
{
    if (const wchar_t* failed = FillPage(page))
    {
        g_failed = true;
        platform::LogWarning(L"Co-op page: writing the page failed ({}); the row opens nothing until the game restarts", failed);
        return false;
    }
    g_filled = true;
    SetTitle(page);
    return true;
}

void* Ready(void* host, std::int32_t z, std::uint8_t inGame) noexcept
{
    bool        inRoot = false;
    void* const page   = Prepare(host, inGame, inRoot);
    if (page == nullptr)
    {
        return nullptr;
    }
    bool added = false;
    if (!inRoot && !platform::CallGuarded(g.addToRoot, added, page, z))  // writes the page's order itself
    {
        platform::LogWarning(L"Co-op page: adding the page to the screen faulted; the row opens nothing");
        Abandon(page);
        return nullptr;
    }
    if (!Fill(page))
    {
        Abandon(page);
        return nullptr;
    }
    return page;
}

void UseSliders(bool sliders) noexcept
try
{
    g_sliders = sliders;
    g_page    = PageOf(sliders);
    g_filled  = false;  // the next open writes the page anew
}
catch (...)
{
}

std::optional<int> TakeAnswered(std::int32_t value) noexcept
{
    const auto answered = g_answered;
    g_answered.reset();
    return answered && answered->value == value ? std::optional<int>(answered->option) : std::nullopt;
}

void CommitEdit(void* page) noexcept
{
    std::int32_t category = -1, row = -1, value = 0;
    GameArray    order;
    std::uint8_t key = 0;
    if (!IsPage(page) || !platform::Read(At(page, facts::kOptionsCategoryIndex), category) ||
        !platform::Read(At(page, facts::kOptionsRowIndex), row) || !platform::Read(At(page, facts::kOptionsEditValue), value) ||
        !platform::Read(At(page, facts::kOptionsOrder), order) || category < 0 || category >= order.num || order.data == nullptr ||
        !platform::Read(static_cast<const std::uint8_t*>(order.data) + category, key))
    {
        return;
    }
    if (const auto option = OptionAt(key, row))
    {
        SetValue(page, rules::RowByte(*option), value);  // queued, as a list's commit is
    }
}

void Abandon(void* page) noexcept
{
    if (page == nullptr || g.remove == nullptr)
    {
        return;
    }
    void* removed = nullptr;
    platform::CallGuarded(g.remove, removed, page);
    const std::uint8_t none = 0;
    platform::WriteGuarded(At(page, facts::kOptionsState), &none, sizeof(none));
    g_filled = false;
}

void* Page() noexcept
{
    return PageNow();
}

bool IsPage(const void* object) noexcept
{
    return object != nullptr && g_pageWeak != 0 && weak::StillNames(g_pageWeak, object);
}

bool IsOpen() noexcept
{
    return g_open;
}

void SetOpen(bool open) noexcept
{
    g_open = open;
}

bool Failed() noexcept
{
    return g_failed;
}

std::uint64_t Commits() noexcept
{
    return g_commits;
}

void Tick() noexcept
{
    if (PageNow() == nullptr)
    {
        g_filled = false;  // gone: its address may become another page's
    }
    RunQueued();
}

}  // namespace lss::menu::coop_page
