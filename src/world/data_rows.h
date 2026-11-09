#pragma once

// The game's mapped data rows (StateChange, StateTrigger, Action, ActionGroup...): a row found by its FName through the
// game's own accessor, and the mapped fields read out of it (Rebirth/Documentation/research/RE-statechange-row-layout.md).
// An array or string field is 16 bytes: a pointer, or a signed offset from the field when its low bit is set, then an
// i32 count (a string's count includes its terminator). Rows are static data: a row pointer is valid while its table
// is loaded, so callers read it within one game-thread call and keep only names.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lss::world::data_rows {

/// A row accessor's constructor, the game's (DataAcc_<Table>_Ctor): fills a 32-byte accessor, its row at +8.
using AccessorFn = void* (*)(void* accessor, std::uint64_t rowName);

/// A mapped field's data, or null for an empty field.
const std::byte* MappedData(const std::byte* field) noexcept;

/// A mapped array's element count, at most `max` (0 if unreadable).
std::int32_t MappedCount(const std::byte* field, std::int32_t max) noexcept;

/// A row name's longest text (the FName limit): a name read shorter would miss its row.
inline constexpr std::int32_t kMaxNameChars = 1024;

/// A mapped string (a 16-byte element or field): its text, or empty if it does not read as a row name (ASCII, at most
/// `cap` characters).
std::wstring MappedText(const std::byte* element, std::int32_t cap = 127);

/// A mapped array of FNames (8-byte elements): at most `max`, up to the first None (a list the game reads that way:
/// triggers, swaps, object lists).
std::vector<std::uint64_t> MappedNames(const std::byte* field, std::int32_t max);

/// The FName at `index` of a mapped array read by position (a row's string arguments), as the game's getters read it:
/// None (0) for a None element and for an index at or past the count.
std::uint64_t MappedNameAt(const std::byte* field, std::int32_t index) noexcept;

/// A row by FName through the game's accessor (its row pointer, or null).
const std::byte* Row(AccessorFn accessor, std::uint64_t name);

/// The FName of a name's text, looked up and never added (0 when the name table has no such name: a row that exists
/// has its name there).
std::uint64_t FindName(const std::wstring& text) noexcept;

/// A row by its name's text (next states are named by strings).
const std::byte* Row(AccessorFn accessor, const std::wstring& name);

}  // namespace lss::world::data_rows
