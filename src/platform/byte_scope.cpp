#include "platform/byte_scope.h"

#include "platform/guarded.h"

#include <cstring>

namespace lss::platform {

void ByteContext::Attach(std::span<Range> ranges, std::span<std::uint8_t> values,
                         std::span<std::uint8_t> saved) noexcept
{
    ranges_ = ranges;
    values_ = values;
    saved_  = saved;
    count_  = 0;
    bytes_  = 0;
    open_   = false;
}

std::size_t ByteContext::OffsetOf(std::size_t index) const noexcept
{
    std::size_t at = 0;
    for (std::size_t i = 0; i < index; ++i)
    {
        at += ranges_[i].size;
    }
    return at;
}

bool ByteContext::Define(std::span<const Range> ranges) noexcept
{
    if (open_)
    {
        return false;
    }
    std::size_t bytes = 0;
    for (const Range& range : ranges)
    {
        bytes += range.size;
        if (range.live == 0 || range.size == 0)
        {
            Clear();
            return false;
        }
    }
    if (ranges.size() > ranges_.size() || bytes > values_.size())
    {
        Clear();
        return false;
    }
    for (std::size_t i = 0; i < ranges.size(); ++i)
    {
        ranges_[i] = ranges[i];
    }
    count_ = ranges.size();
    bytes_ = bytes;
    std::memset(values_.data(), 0, bytes_);
    std::memset(saved_.data(), 0, bytes_);
    return true;
}

bool ByteContext::Clear() noexcept
{
    if (open_)
    {
        return false;
    }
    count_ = 0;
    bytes_ = 0;
    return true;
}

bool ByteContext::Retarget(std::size_t index, std::uintptr_t live) noexcept
{
    if (open_ || index >= count_ || live == 0)
    {
        return false;
    }
    ranges_[index].live = live;
    return true;
}

std::span<std::uint8_t> ByteContext::Value(std::size_t index) noexcept
{
    return index < count_ ? values_.subspan(OffsetOf(index), ranges_[index].size) : std::span<std::uint8_t>();
}

std::span<const std::uint8_t> ByteContext::Value(std::size_t index) const noexcept
{
    return index < count_ ? values_.subspan(OffsetOf(index), ranges_[index].size) : std::span<const std::uint8_t>();
}

bool ByteContext::Open() noexcept
{
    if (open_ || count_ == 0)
    {
        return false;
    }
    std::size_t written = 0;
    std::size_t at      = 0;
    for (; written < count_; ++written)
    {
        const Range& range = ranges_[written];
        auto*        live  = reinterpret_cast<void*>(range.live);
        if (!CopyGuarded(saved_.data() + at, live, range.size) ||
            (range.kind != Kind::Keep && !WriteGuarded(live, values_.data() + at, range.size)))
        {
            break;
        }
        at += range.size;
    }
    if (written < count_)
    {
        // A range faulted: the ones already written go back, in reverse.
        for (std::size_t i = written; i-- > 0;)
        {
            at -= ranges_[i].size;
            if (ranges_[i].kind != Kind::Keep)
            {
                WriteGuarded(reinterpret_cast<void*>(ranges_[i].live), saved_.data() + at, ranges_[i].size);
            }
        }
        return false;
    }
    open_ = true;
    return true;
}

unsigned ByteContext::Close() noexcept
{
    if (!open_)
    {
        return 0;
    }
    unsigned    changed = 0;
    std::size_t at      = bytes_;
    for (std::size_t i = count_; i-- > 0;)
    {
        const Range& range = ranges_[i];
        at -= range.size;
        auto* live  = reinterpret_cast<void*>(range.live);
        auto* value = values_.data() + at;
        auto* saved = saved_.data() + at;
        if (range.kind == Kind::Exchange)
        {
            CopyGuarded(value, live, range.size);  // what the game left there is the player's now
            WriteGuarded(live, saved, range.size);
            continue;
        }
        std::uint8_t now[64];
        bool         same = true;
        for (std::size_t done = 0; done < range.size; done += sizeof(now))
        {
            const std::size_t part = range.size - done < sizeof(now) ? range.size - done : sizeof(now);
            const auto*       was  = range.kind == Kind::Set ? value + done : saved + done;
            if (!CopyGuarded(now, static_cast<const std::uint8_t*>(live) + done, part) ||
                std::memcmp(now, was, part) != 0)
            {
                same = false;
                break;
            }
        }
        if (same)
        {
            if (range.kind == Kind::Set)
            {
                WriteGuarded(live, saved, range.size);
            }
            continue;
        }
        ++changed;
        if (range.kind == Kind::Keep)
        {
            WriteGuarded(live, saved, range.size);  // the game wrote it inside: put back as it was found
        }
        // A Set range the game wrote inside keeps the game's newer value.
    }
    open_ = false;
    return changed;
}

}  // namespace lss::platform
