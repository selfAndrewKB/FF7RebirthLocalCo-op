#pragma once

// A game setting held at the mod's value for a while, then put back: the console settings two-player rendering
// needs (split_screen_settings.h), the two-player layout (split_layout.h) and the three-player layout (mod.cpp, while
// three local players are listed). One algorithm for all of them, written once:
//
//   - Hold: the current value is read and remembered; the mod's is written and read back to confirm. A value that
//     is already the mod's is left as it is. Once a write has RUN the previous value is remembered whatever the
//     read-back says: the setting may have changed even when confirming it failed. The previous value is kept
//     across the mod's own later writes (a layout toggled while it is held still goes back to the game's own),
//     so what is remembered is the last value the mod did not write; a write back to the previous value ends the
//     hold, and the next write starts a new one from whatever is in the slot then.
//   - Release: the remembered value is written back if the mod's is still in place. A value that cannot be read is
//     never overwritten, and a value someone else set meanwhile is left alone: both may be someone's choice. A
//     failed release keeps the remembered value and is tried again at the next release.
//
// The engine is reached through SettingAccess, so the policy has no UE4SS dependency and is tested on the host
// (tests/held_setting_test.cpp). Every change is reported for the caller to log (Describe), none is logged here.

#include <cstdint>
#include <optional>
#include <string>

namespace lss::players {

/// The setting's read and write, both by its name (a console variable's, or ignored by a fixed field's).
template <class T>
struct SettingAccess
{
    std::optional<T> (*read)(const wchar_t* name) noexcept;
    bool (*write)(const wchar_t* name, T value) noexcept;
};

/// What a Hold, Release or Update did, for the caller to log. `held` is the mod's value, `value` the other one the
/// line needs (the previous value, or the one found).
struct HeldChange
{
    enum class Kind
    {
        None,           // nothing to do
        Chosen,         // not held now: the value is remembered for the next hold (split_layout)
        Unreadable,     // the current value was needed to choose, and could not be read (split_layout)
        Applied,        // the mod's value is in place (`value` = the previous one)
        AlreadySet,     // the setting already had the mod's value; nothing was written
        ApplyFailed,    // it could not be read, written or confirmed (`value` = the one found, if read)
        Restored,       // the previous value (`value`) was put back
        LeftAlone,      // something else changed it meanwhile (`value` = now); kept
        RestoreFailed,  // it could not be read or put back; tried again at the next release
    };
    Kind         kind  = Kind::None;
    std::int32_t value = 0;
    std::int32_t held  = 0;
};

/// A value's text in the log; null prints the number.
using ValueText = std::wstring (*)(std::int32_t value);

/// The change for the log (empty for None): "<label>: <name> <from> -> <to> until the leave", and so on.
std::wstring Describe(const HeldChange& change, const wchar_t* label, const wchar_t* name, ValueText text = nullptr);

template <class T>
class HeldSetting
{
public:
    /// `name` must outlive the object.
    HeldSetting(SettingAccess<T> access, const wchar_t* name) noexcept : access_(access), name_(name) {}

    /// Writes `value`, remembering the previous one for the release.
    HeldChange Hold(T value) noexcept
    {
        const auto current = access_.read(name_);
        if (!current)
        {
            return {HeldChange::Kind::ApplyFailed, 0, value};  // nothing ran
        }
        HeldChange::Kind kind = HeldChange::Kind::AlreadySet;
        if (*current != value)
        {
            if (!access_.write(name_, value))
            {
                return {HeldChange::Kind::ApplyFailed, *current, value};  // nothing changed
            }
            if (!held_ || *current != held_->written)
            {
                held_ = Held{*current, value};  // the previous value: the last one the mod did not write
            }
            else
            {
                held_->written = value;  // over the mod's own earlier write: the previous value stays
            }
            const auto after = access_.read(name_);
            kind = after && *after == value ? HeldChange::Kind::Applied : HeldChange::Kind::ApplyFailed;
        }
        if (held_ && held_->previous == value)
        {
            held_.reset();  // back at the previous value: nothing to put back
        }
        return {kind, *current, value};
    }

    /// Puts the previous value back if the mod's is still in place. None if nothing is held.
    HeldChange Release() noexcept
    {
        if (!held_)
        {
            return {};
        }
        const T    previous = held_->previous;
        const T    written  = held_->written;
        const auto current  = access_.read(name_);
        if (!current)
        {
            return {HeldChange::Kind::RestoreFailed, previous, written};  // unknown: kept, tried again
        }
        if (*current == previous)
        {
            held_.reset();  // the write did not take, or it was already put back
            return {};
        }
        if (*current != written)
        {
            held_.reset();  // someone chose another value after the mod
            return {HeldChange::Kind::LeftAlone, *current, written};
        }
        if (!access_.write(name_, previous))
        {
            return {HeldChange::Kind::RestoreFailed, previous, written};
        }
        const auto after = access_.read(name_);
        if (!after || *after != previous)
        {
            return {HeldChange::Kind::RestoreFailed, previous, written};  // kept: the next release checks again
        }
        held_.reset();
        return {HeldChange::Kind::Restored, previous, written};
    }

    /// Holds `value` while `in`, releases when it ends; acts only when `in` changes. An empty value holds nothing
    /// (the setting stays the game's) but still counts as in.
    HeldChange Update(bool in, std::optional<T> value) noexcept
    {
        if (in == in_)
        {
            return {};
        }
        in_ = in;
        if (!in)
        {
            return Release();
        }
        return value ? Hold(*value) : HeldChange{};
    }

    /// The setting's current value, whoever set it.
    std::optional<T> Read() const noexcept { return access_.read(name_); }

    bool           In() const noexcept { return in_; }
    const wchar_t* Name() const noexcept { return name_; }

private:
    /// While the mod's value is in place: what to put back, and what the mod wrote (its own later writes keep the
    /// previous value; anything else in the slot is someone's choice).
    struct Held
    {
        T previous;
        T written;
    };

    SettingAccess<T>    access_;
    const wchar_t*      name_;
    bool                in_ = false;
    std::optional<Held> held_;
};

}  // namespace lss::players
