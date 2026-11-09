#pragma once

// The Co-op page's facts: the game's UI system, the pause menu's first page and its row tables, the Options screen the
// mod opens as its page, and the helpers that write its data (menu/*). Research addresses (1.004); 1.005 through
// tools/version_match/out-1.005/replaced-coop-page.json, except where a PerBuild value says otherwise.
//
// Research: research/RE-coop-rows-facts.md (the rows, the UI system, the main menu manager, the windows),
// research/RE-coop-page-facts.md (the page, its data, its helpers), research/RE-coop-page-menus.md and
// research/RE-coop-page-options-screen.md (the mechanisms).

#include "facts/engine.h"
#include "facts/hud.h"
#include "platform/build_value.h"
#include "platform/verified_code.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace lss::facts {

using platform::FunctionFact;
using platform::PerBuild;
using platform::SlotFact;

// --- The UI system and the main menu manager ------------------------------------------------------------------------

// The UI system itself: facts/hud.h kEndUISystem (a static object whose qwords are the managers).
inline constexpr std::uint32_t  kEndUISystemSlots   = 64;
inline constexpr std::uint32_t  kUiInputSlot        = 2;   // Square's input object (the focus stack, the stick mode)
inline constexpr std::uint32_t  kUiMainMenuSlot     = 28;  // the main menu manager
inline constexpr std::uint32_t  kUiMenuTutorialSlot = 58;  // the menu tutorial
inline constexpr std::uint32_t  kMenuTutorialState  = 0x28;  // u8
/// The tutorial states in which the window opener opens nothing (it returns after its prelude).
inline constexpr std::array<std::uint8_t, 4> kMenuTutorialBusy{33, 36, 37, 38};

inline constexpr std::uint32_t kMainMenuMgrMenu       = 0x20;   // weak: the UEndMainMenu frame
inline constexpr std::uint32_t kMainMenuMgrCurrent    = 0x30;   // weak: the current window
inline constexpr std::uint32_t kMainMenuMgrOpen       = 0x180;  // u8: 1 open, 2 closing, 0 closed
inline constexpr std::uint8_t  kMainMenuOpen          = 1;
inline constexpr std::uint32_t kMainMenuMgrTopCurrent = 0x4A7;  // u8: the first page is the current window

// --- The main menu's windows ----------------------------------------------------------------------------------------

/// UEndMainMenuTopWindow's vtable: the live first page is told apart by it.
inline constexpr std::uintptr_t kMainMenuTopWindowVTable = 0x1457B1918;
inline constexpr PerBuild<std::uint32_t> kWindowHideSlot = {0x4B8, 0x4C0};  // UEndMainMenuWindow: Hide
// GetWorld: facts/engine.h kObjectGetWorldSlot.
inline constexpr std::uint32_t kWindowVisible = 0x408;  // u8
inline constexpr std::uint32_t kWindowTitle   = 0x3F8;  // FString the manager's tick shows as the title
inline constexpr std::uint32_t kWindowZOrder  = 0x2A8;  // i32: the order the widget was added to the overlay at
inline constexpr std::uint32_t kTopRows       = 0x4A0;  // TArray<u8>: the kinds the first page built

/// The key guide the main menu's tick shows comes from the current window's guide getter (a vtable slot): the first
/// page's answers its own text field, every other window's (the base window's) the focused widget's guide, so their
/// guide hides over an open gauge or list and comes back at a cancel (research/RE-coop-page-guide.md §2.3, §4).
inline constexpr std::uint8_t kTopGuideGetterBytes[] = {0x48, 0x8D, 0x81, 0x88, 0x04, 0x00, 0x00, 0xC3};
inline constexpr SlotFact     kTopGuideSlot{L"the first page's guide getter", 0x1457B1DF8,
                                        {L"UEndMainMenuTopWindow_GetGuideText_Field488", 0x1418EF9A0, kTopGuideGetterBytes}};
inline constexpr std::uint8_t kWindowGuideGetterBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57,
                                                           0xB8, 0x40, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kWindowGuideGetter{L"UEndMainMenuWindow_GetGuideText_FocusedMeta", 0x140BE3ED8,
                                                 kWindowGuideGetterBytes};

/// EndUI28_MainMenu_OpenWindowByKind(mgr, u8 kind): its prelude (the tutorial, a flag), then the kind's window.
inline constexpr std::uint8_t kOpenWindowByKindBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                          0x24, 0x20, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56};
inline constexpr FunctionFact kOpenWindowByKind{L"EndUI28_MainMenu_OpenWindowByKind", 0x1414CB7A4, kOpenWindowByKindBytes};
inline constexpr std::size_t  kOpenWindowByKindFirst  = 5;  // mov [rsp+10h], rbx (both builds)
inline constexpr std::uint8_t kOpenWindowPreludeOnly = 0xFF;  // a kind past the last: the prelude, then nothing

/// EndUI28_MainMenu_ShowTopWindow(mgr, bool restoreCursor): the first page back as after any window.
inline constexpr std::uint8_t kShowTopWindowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
                                                       0x41, 0x56, 0x41, 0x57, 0xB8, 0x60, 0x00, 0x00};
inline constexpr FunctionFact kShowTopWindow{L"EndUI28_MainMenu_ShowTopWindow", 0x1414CC430, kShowTopWindowBytes};

// --- The pause menu's row: the trial's "Buy Now", kind 13 ----------------------------------------------------------

inline constexpr std::uint8_t   kTopRowKind     = 13;
inline constexpr std::uintptr_t kTopRowTextSlot = 0x14615BD48;  // g_MainMenuTopWindow_ItemTextIds[13]
inline constexpr std::uintptr_t kTopRowTextWas  = 0x14615B910;  // $menu_TrialVersion_MainMenu_BuyNow
inline constexpr std::uintptr_t kTopRowHelpSlot = 0x14615BCD8;  // g_MainMenuTopWindow_ItemHelpIds[13]
inline constexpr std::uintptr_t kTopRowHelpWas  = 0x14615BBD0;  // ..._BuyNow_help
/// The order of kinds 8..13 (one aligned word, read only by the row builder): 13 moved before 12 (System).
inline constexpr std::uintptr_t kTopRowOrderWord = 0x145D20860;
inline constexpr std::uint64_t  kTopRowOrderWas  = 0x00000D0C0B0A0908;
inline constexpr std::uint64_t  kTopRowOrderCoop = 0x00000C0D0B0A0908;
/// What the builder appends kind 13 by, per build. 1.004: a name global it reads only there, 0 appends the row (the
/// global is written after its static init). 1.005: the builder's one call of the shared demo-mode test, answered yes
/// there only; the call's bytes and its target are this build's own (no research address).
inline constexpr std::uintptr_t kTopRowDemoName = 0x1491C0BE8;        // 1.004 only
inline constexpr std::uintptr_t kTopRowDemoCall = 0x1414A605F;        // 1.005 only: call EndGame_IsDemoMode
inline constexpr std::uintptr_t kTopRowDemoTest = 0x1414A51C4;        // 1.005 only: its target
inline constexpr std::uint8_t   kTopRowDemoCallBytes[] = {0xE8, 0x60, 0xF1, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0xB4};

// --- The title menu's row: the demo's "Buy Now", kind 9 (research/RE-coop-rows-facts.md §1.1, §1.2) ---------------

/// UEndStartMenu's construct (vtable 0x1467579B8 +0x320; 1.005 +0x328): it builds the rows' kinds, then the list.
inline constexpr std::uint8_t kStartMenuConstructBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                                            0x24, 0x18, 0x56, 0x57, 0x41, 0x57, 0xB8, 0x30};
inline constexpr SlotFact kStartMenuConstruct{L"the title menu's construct", 0x146757CD8,
                                              {L"UEndStartMenu_NativeConstruct", 0x14395D98C, kStartMenuConstructBytes}};
inline constexpr std::uint32_t kWidgetSlot           = 0x28;   // UWidget::Slot (stock 4.26)
inline constexpr std::uint32_t kStartMenuKinds       = 0x438;  // TArray<int32>
inline constexpr std::int32_t  kStartMenuKindOptions = 3;
inline constexpr std::int32_t  kStartMenuKindCoop    = 9;      // built only under the demo check
inline constexpr std::uint32_t kStartMenuAction      = 0x400;  // i32: a flow of its own runs (0 idle)
inline constexpr std::uint32_t kStartMenuPage        = 0x4A8;  // the game's Options page (the open reads it from here)
inline constexpr std::uint32_t kStartMenuListWidth   = 0x478;  // float: the list slot's width
inline constexpr std::uint32_t kStartMenuListBase    = 0x47C;  // float: its height at the base row count
inline constexpr std::int32_t  kStartMenuBaseRows    = 4;
inline constexpr float         kStartMenuRowHeight   = 62.0f;
inline constexpr std::uintptr_t kStartMenuRowTextSlot = 0x146757F28;  // g_StartMenu_RowTextIds[9]
inline constexpr std::uintptr_t kStartMenuRowTextWas  = 0x146756D48;  // $demo_titlemenu_BuyNow
/// Kind 9's handler, void(start), and its one call, in the row press's switch: the call redirected for good (1.005's
/// handler entry crosses a block; the call fits one in both builds).
inline constexpr std::uint8_t kStartMenuKindCoopBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                           0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41};
inline constexpr FunctionFact   kStartMenuKindCoopPress{L"UEndStartMenu_Kind9_BuyNowTrial", 0x143960498,
                                                        kStartMenuKindCoopBytes};
inline constexpr std::uintptr_t kStartMenuKindCoopCall = 0x1439645C8;
/// The list: UUserWidget's find by name (on the start menu itself) with the game's own FName global, then the game's
/// class tests (the Cast_* functions answer a bool in al, read live 2026-11-07: the low byte 1, the rest of rax left over).
inline constexpr std::uint8_t kFindWidgetBytes[] = {0xB8, 0x48, 0x00, 0x00, 0x00, 0xE8, 0x3A, 0xFB,
                                                    0xE7, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x89};
inline constexpr FunctionFact   kFindWidget{L"UUserWidget_FindWidgetByName", 0x14122F3EC, kFindWidgetBytes};
inline constexpr std::uintptr_t kNameListMenu = 0x1491C5D70;  // g_FName_List_Menu
inline constexpr std::uint8_t kCastScrollBoxBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                       0x94, 0xA5, 0x97, 0x00, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr FunctionFact kCastScrollBox{L"Cast_UEndVirtualScrollBox", 0x141734990, kCastScrollBoxBytes};
// The list's height: the canvas slot's stock SetSize, reflected (menu/title_row; the 1.005 pairing of the native setter
// and of the slot's class test read live as other functions).
/// TArray<int32>'s growth after Num is raised.
inline constexpr std::uint8_t kIntArrayGrowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C,
                                                      0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41};
inline constexpr FunctionFact kIntArrayGrow{L"TArrayInt32_ResizeGrow", 0x1420FCA7C, kIntArrayGrowBytes};
/// UEndStartMenu::OnOptions(start): the frame and a second widget made and added, then the page read from +0x4A8 (its
/// close delegate bound, added at 1990 if not in the overlay, opened).
inline constexpr std::uint8_t kStartMenuOpenOptionsBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74,
                                                              0x24, 0x20, 0x55, 0x57, 0x41, 0x56, 0x48, 0x8B};
inline constexpr FunctionFact kStartMenuOpenOptions{L"UEndStartMenu__OnOptions_OpenOptionsPage", 0x1439666E4,
                                                    kStartMenuOpenOptionsBytes};
/// The page's close function OnOptions binds, void(const capture*: {start, its weak}): the frame taken down, the
/// filters and the stick mode let go, the start menu's focus back.
inline constexpr std::uint8_t kStartMenuOptionsClosedBytes[] = {0x40, 0x56, 0xB8, 0xD0, 0x00, 0x00, 0x00, 0xE8,
                                                                0x14, 0x94, 0xD6, 0xFF, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr FunctionFact kStartMenuOptionsClosed{L"UEndStartMenu_OnOptionsClosed_Lambda", 0x142345B10,
                                                      kStartMenuOptionsClosedBytes};
/// SquareInput_SetStickDPadRequestBit1(input, u8 on): the stick as the d-pad, request bit 1 (the title's open sets it).
inline constexpr std::uint8_t kSetStickDPadModeBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x0E, 0x07,
                                                          0x57, 0xFF, 0x48, 0x2B, 0xE0, 0x44, 0x8A, 0xC2};
inline constexpr FunctionFact kSetStickDPadMode{L"SquareInput_SetStickDPadRequestBit1", 0x142B3E818,
                                                kSetStickDPadModeBytes};

// --- The Options screen as the mod's page ---------------------------------------------------------------------------

inline constexpr const wchar_t* kOptionsClassPath =
    L"/Game/Menu/Resident/MainMenu/MainMenu_OptionsTop2.MainMenu_OptionsTop2_C";

inline constexpr std::uint32_t kOptionsState          = 0x3C8;  // u8
inline constexpr std::uint8_t  kOptionsStateClosing   = 8;
inline constexpr std::uint8_t  kOptionsStateRangeEdit = 5;
inline constexpr std::uint32_t kOptionsCategoryList   = 0x3D0;  // UEndVirtualScrollBox*
inline constexpr std::uint32_t kOptionsRowList        = 0x3D8;  // UEndVirtualScrollBox*: the category's rows
inline constexpr std::uint32_t kOptionsCategoryIndex  = 0x410;  // i32
inline constexpr std::uint32_t kOptionsRowIndex       = 0x414;  // i32
inline constexpr std::uint32_t kOptionsEditValue      = 0x418;  // i32: a Range row's pending value
inline constexpr std::uint32_t kOptionsInGame         = 0x428;  // u8 written by Create
inline constexpr std::uint32_t kOptionsOrder          = 0x498;  // TArray<u8>: the category keys shown
inline constexpr std::uint32_t kOptionsLabels         = 0x4D8;  // TArray<FString>: _CategoryItems, by key
inline constexpr std::uint32_t kOptionsItems          = 0x4E8;  // TMap<u8, FOptionInfos>: _OptionItems

inline constexpr std::uint32_t kOptionInfosHelp     = 0x00;  // FOptionInfos.HelpText
inline constexpr std::uint32_t kOptionInfosRows     = 0x10;  // FOptionInfos.Infos (TArray<FOptionInfo>)
inline constexpr std::size_t   kOptionInfoSize      = 0x68;
inline constexpr std::size_t   kOptionInfoKind      = 0x00;  // u8: 0 List, 1 Range
inline constexpr std::size_t   kOptionInfoCategory  = 0x01;  // u8: the byte the value read and commit take
inline constexpr std::size_t   kOptionInfoText      = 0x08;  // FString
inline constexpr std::size_t   kOptionInfoHelp      = 0x18;  // FString
inline constexpr std::size_t   kOptionInfoValues    = 0x28;  // TArray<FString>
inline constexpr std::size_t   kOptionInfoValueHelps = 0x38;  // TArray<FString>
inline constexpr std::size_t   kOptionInfoMin       = 0x58;  // i32
inline constexpr std::size_t   kOptionInfoMax       = 0x5C;  // i32
inline constexpr std::uint8_t  kOptionKindList      = 0;
inline constexpr std::uint8_t  kOptionKindRange     = 1;

/// The heading id per category key, read by the category press into the page's title (7 entries in 1.004, 8 in 1.005:
/// a key past them shows no heading).
inline constexpr std::uintptr_t kOptionsHeadings     = 0x14615DE98;
inline constexpr PerBuild<std::uint32_t> kOptionsHeadingCount = {7, 8};

inline constexpr std::uint8_t kOptionsCreateBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0xB8, 0x20,
                                                       0x00, 0x00, 0x00, 0xE8, 0x68, 0xCE, 0x4C, 0x01};
inline constexpr FunctionFact kOptionsCreate{L"UEndMainOptionsMenu_Create", 0x140BE20B8, kOptionsCreateBytes};
inline constexpr std::uint8_t kOptionsOpenBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                     0x24, 0x10, 0x57, 0xB8, 0x60, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kOptionsOpen{L"UEndMainOptionsMenu_Open", 0x141808144, kOptionsOpenBytes};
inline constexpr std::uint8_t kAddToRootOverlayBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xC2, 0x63,
                                                          0x24, 0x01, 0x48, 0x2B, 0xE0, 0x48, 0xC7, 0x44};
inline constexpr FunctionFact kAddToRootOverlay{L"UEndUserWidget__AddToRootOverlay_SetZOrder", 0x140E68B64,
                                                kAddToRootOverlayBytes};
inline constexpr std::uint8_t kIsInRootOverlayBytes[] = {0x33, 0xC0, 0x48, 0x39, 0x81, 0x90, 0x02, 0x00,
                                                         0x00, 0x74, 0x14, 0x48, 0x8B, 0x91, 0x98, 0x02};
inline constexpr FunctionFact kIsInRootOverlay{L"UEndUserWidget_IsInRootOverlay", 0x1410E5A9C, kIsInRootOverlayBytes};

/// The value read, i32(page, u8 key, i32 row): redirected for the mod's rows. 1.004 enters with `mov [rsp+18h], rbx`
/// (5 bytes). 1.005 enters with `mov rax, rsp; mov [rax+8], rbx` (7) across a block: redirected at the next 8 bytes
/// (`mov [rax+18h], rsi; mov [rax+20h], rdi`), the first 7 run before the mod's function.
inline constexpr std::uint8_t kOptionsGetValueBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
                                                         0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr FunctionFact kOptionsGetValue{L"OptionsMenu_FillValueArray", 0x140BE4838, kOptionsGetValueBytes};
inline constexpr std::size_t  kOptionsGetValueFirst = 5;
inline constexpr std::uint8_t kOptionsGetValueFirst1005[]  = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08};
inline constexpr std::uint8_t kOptionsGetValueSecond1005[] = {0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20};
/// The value commit, void(page, u8 byte, i32 value): redirected for the mod's rows (first instruction 5 bytes).
inline constexpr std::uint8_t kOptionsSetValueBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57,
                                                         0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
inline constexpr FunctionFact kOptionsSetValue{L"OptionsMenu_SetOptionValue_CopyApplyCommit", 0x141808A00,
                                               kOptionsSetValueBytes};
inline constexpr std::size_t  kOptionsSetValueFirst = 5;
/// Reset All's Yes, (page, …): for the current category (keys 0..4) a copy of the option block reset from the default
/// profile (bytes to 0x54 only), committed, then each row read through the value read and set through the value commit
/// (research/RE-coop-page-guide.md, Reset All). 1.005's first instruction (`mov [rsp+8], rcx`) lies in one block;
/// 1.004's crosses one and its second (`push rbp`) is too short to take the jump, so 1.004 keeps no Reset All.
inline constexpr std::uint8_t kOptionsResetConfirmBytes[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56,
                                                             0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};
inline constexpr FunctionFact kOptionsResetConfirm{L"OptionsMenu_ResetCategoryToDefaults_OnConfirm", 0x1438E66BC,
                                                   kOptionsResetConfirmBytes};
inline constexpr std::size_t  kOptionsResetConfirmFirst = 5;

/// The page's key handler (pass 2: Back, Accept, the options button), a slot of MainMenu_OptionsTop2_C's vtable
/// (0x14590C758 +0x3A8; 1.005 +0x3B0).
inline constexpr std::uint8_t kOptionsKeyDownBytes[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                                        0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC};
inline constexpr SlotFact kOptionsKeyDown{L"the Options page's key handler", 0x14590CB00,
                                          {L"UEndMainOptionsMenu_KeyDown", 0x141808C00, kOptionsKeyDownBytes}};

// --- The page's slider rows (research/RE-coop-page-facts.md §1.2, §1.3, §3) -----------------------------------------

/// OptionsMenu_UpdateGaugeMarker(page, gauge, const FOptionInfo*): placed after at its two calls, the Range row's press
/// and each Left/Right step (1.004's both cross a block: changed from the game thread).
inline constexpr std::uint8_t kGaugeMarkerBytes[] = {0x48, 0x85, 0xD2, 0x0F, 0x84, 0xB7, 0x01, 0x00,
                                                     0x00, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89};
inline constexpr FunctionFact   kGaugeMarker{L"OptionsMenu_UpdateGaugeMarker", 0x1417C71CC, kGaugeMarkerBytes};
inline constexpr std::uintptr_t kGaugeMarkerOpenCall = 0x14148F39F;
inline constexpr std::uintptr_t kGaugeMarkerStepCall = 0x1438E5F1D;
/// FString::AppendInt(FString* out, i32): the row's value text, appended into a zeroed FString, at its one call in the
/// row setup (right after the value read).
inline constexpr std::uint8_t   kAppendIntBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                                     0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0xB8, 0x50};
inline constexpr FunctionFact   kAppendInt{L"FString_AppendInt", 0x140A38D24, kAppendIntBytes};
inline constexpr std::uintptr_t kRowValueTextCall = 0x140BE6C25;
/// The page's action handler (pass 1: the d-pad; state 5 Left/Right steps unless the event repeats), a slot of
/// MainMenu_OptionsTop2_C's vtable (+0x3A0; 1.005 +0x3A8).
inline constexpr std::uint8_t kOptionsActionDownBytes[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
                                                           0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C};
inline constexpr SlotFact kOptionsActionDown{L"the Options page's action handler", 0x14590CAF8,
                                             {L"UEndMainOptionsMenu_ActionDown", 0x1413FB608, kOptionsActionDownBytes}};
/// The key event both handlers take (an FKeyEvent): its repeat byte and its key's FName.
inline constexpr std::uint32_t  kActionEventRepeat = 0x0A;
inline constexpr std::uint32_t  kActionEventKey    = 0x18;
/// The keys the handlers compare with (EKeys FKey globals: the FName first).
inline constexpr std::uintptr_t kKeyDPadLeft  = 0x149245DA0;
inline constexpr std::uintptr_t kKeyDPadRight = 0x149245D88;
inline constexpr std::uintptr_t kKeyAccept    = 0x149247148;  // Virtual_Accept
/// The gauge: on the page, the gauge widget and its strip (a scroll box); on the gauge, the left arrow (in one
/// horizontal box with the strip and the right arrow), the marker image and the label.
inline constexpr std::uint32_t kOptionsGauge      = 0x3F0;
inline constexpr std::uint32_t kOptionsGaugeStrip = 0x400;
inline constexpr std::uint32_t kGaugeArrowLeft    = 0x3D0;  // Arrow_Left
inline constexpr std::uint32_t kGaugeHead         = 0x3F0;  // Img_Gauge_Head
inline constexpr std::uint32_t kGaugeText         = 0x400;  // Txt_Setting
inline constexpr float         kGaugePitch        = 28.0f;  // a cell's width
inline constexpr float         kGaugeHeadX        = 80.5f;  // the marker's x at cell 0
inline constexpr std::uint32_t kPanelSlotParent   = 0x28;   // UPanelSlot::Parent (stock 4.26)
inline constexpr std::uint32_t kCanvasSlotLeft    = 0x38;   // Square's canvas slot: LayoutData.Offsets.Left
inline constexpr std::uint32_t kCanvasSlotTop     = 0x3C;   // and .Top

// --- Taking the page down the way its own no-animation Back does (research/RE-coop-page-facts.md §5) ---------------

/// UEndUserWidget::RemoveFromParent (the overlay add's counterpart).
inline constexpr std::uint8_t kRemoveFromParentBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                                          0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
inline constexpr FunctionFact kRemoveFromParent{L"UEndUserWidget__RemoveFromParent", 0x140F5C0E8, kRemoveFromParentBytes};
/// UWidget::TakeWidget(widget, TSharedRef* out16) -> out: the reference the focus layer is pushed and popped with.
inline constexpr std::uint8_t kTakeWidgetBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                    0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
inline constexpr FunctionFact kTakeWidget{L"UWidget__TakeWidget", 0x140E6827C, kTakeWidgetBytes};
/// EndUISystem_PopFocusLayer(the UI system, TSharedRef* taken): pops the widget's layer, always releases `taken`.
inline constexpr std::uint8_t kPopFocusLayerBytes[] = {0x40, 0x53, 0xB8, 0x30, 0x00, 0x00, 0x00, 0xE8,
                                                       0xFC, 0x66, 0x42, 0x01, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr FunctionFact kPopFocusLayer{L"EndUISystem_PopFocusLayer", 0x140C88828, kPopFocusLayerBytes};

// --- The helpers the page's data is written with -------------------------------------------------------------------

/// FString_AssignCopy(dst, const FString* src): reads only src's Data and Num (a view over the mod's text will do).
inline constexpr std::uint8_t kFStringAssignBytes[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                                       0x89, 0x68, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48};
inline constexpr FunctionFact kFStringAssign{L"FString_AssignCopy", 0x141081984, kFStringAssignBytes};
/// OptionItemsMap_FindOrAdd(map, const u8* key) -> FOptionInfos*.
inline constexpr std::uint8_t kOptionItemsFindOrAddBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                              0x68, 0x3A, 0xE4, 0x00, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr FunctionFact kOptionItemsFindOrAdd{L"OptionItemsMap_FindOrAdd", 0x14126B4BC, kOptionItemsFindOrAddBytes};
inline constexpr std::uint8_t kOptionInfoCtorBytes[] = {0x33, 0xC0, 0x48, 0x89, 0x41, 0x08, 0x48, 0x89,
                                                        0x41, 0x10, 0x48, 0x89, 0x41, 0x18, 0x48, 0x89};
inline constexpr FunctionFact kOptionInfoCtor{L"FOptionInfo_DefaultCtor", 0x1415BD460, kOptionInfoCtorBytes};
inline constexpr std::uint8_t kOptionInfoDtorBytes[] = {0x40, 0x53, 0xB8, 0x20, 0x00, 0x00, 0x00, 0xE8,
                                                        0x90, 0x1A, 0xAF, 0x00, 0x48, 0x2B, 0xE0, 0x48};
inline constexpr FunctionFact kOptionInfoDtor{L"FOptionInfo_Dtor", 0x1415BD494, kOptionInfoDtorBytes};
inline constexpr std::uint8_t kOptionInfoMoveBytes[] = {0x8A, 0x02, 0x45, 0x33, 0xC0, 0x88, 0x01, 0x8A,
                                                        0x42, 0x01, 0x88, 0x41, 0x01, 0x4C, 0x89, 0x41};
inline constexpr FunctionFact kOptionInfoMove{L"FOptionInfo_MoveCtor", 0x1415BD314, kOptionInfoMoveBytes};
inline constexpr std::uint8_t kGrowBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                              0x24, 0x10, 0x57, 0xB8, 0x20, 0x00, 0x00, 0x00};
inline constexpr FunctionFact kOptionInfosGrow{L"TArrayFOptionInfo_ResizeGrow", 0x141009F1C, kGrowBytes};
inline constexpr FunctionFact kStringArrayGrow{L"TArray16_ResizeGrow_FString", 0x1407E2988, kGrowBytes};
/// TArrayFOptionInfo_MoveAssign(dst, src): with an empty source, every row destroyed and the buffer freed.
inline constexpr std::uint8_t kOptionInfosMoveAssignBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                               0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
inline constexpr FunctionFact kOptionInfosMoveAssign{L"TArrayFOptionInfo_MoveAssign", 0x14126B430,
                                                     kOptionInfosMoveAssignBytes};
inline constexpr std::uint8_t kScrollBoxSetCountBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0xD2, 0x4D,
                                                           0xFC, 0x00, 0x48, 0x2B, 0xE0, 0x89, 0x91, 0x38};
inline constexpr FunctionFact kScrollBoxSetCount{L"UEndVirtualScrollBox_SetItemCount", 0x1410EA154,
                                                 kScrollBoxSetCountBytes};
inline constexpr std::uint8_t kScrollBoxRefreshBytes[] = {0xB8, 0x28, 0x00, 0x00, 0x00, 0xE8, 0x16, 0x95,
                                                          0xFC, 0x00, 0x48, 0x2B, 0xE0, 0x48, 0x8B, 0x89};
inline constexpr FunctionFact kScrollBoxRefresh{L"UEndVirtualScrollBox_RefreshItems", 0x1410E5A10,
                                                kScrollBoxRefreshBytes};

}  // namespace lss::facts
