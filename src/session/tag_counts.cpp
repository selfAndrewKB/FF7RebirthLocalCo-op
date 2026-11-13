#include "session/tag_counts.h"

#include "facts/engine.h"
#include "facts/party.h"
#include "platform/image.h"
#include "platform/verified_code.h"
#include "players/actor_fields.h"
#include "players/object_fields.h"
#include "players/weak_object.h"

namespace lss::session::tag_counts {

namespace {

/// TSet<TPair<FName, int32>>::FindId(this, &outIndex, key): the index, or -1.
using FindIdFn = std::int32_t* (*)(const void* set, std::int32_t* outIndex, std::uint64_t key);

// Set once at install.
FindIdFn             g_findId        = nullptr;
const std::uint64_t* g_registryWeak  = nullptr;
const std::uint64_t* g_triggerTag    = nullptr;

}  // namespace

bool Install(const platform::GameImage& image) noexcept
{
    const auto findId   = platform::FindFunction(image, facts::kTagSetFindId);
    const auto registry = image.Rebase(facts::kTagRegistryWeak, sizeof(std::uint64_t));
    const auto trigger  = image.Rebase(facts::kTriggerTagName, sizeof(std::uint64_t));
    if (!players::weak_object::Known() || !findId || !registry || !trigger)
    {
        return false;
    }
    g_findId       = reinterpret_cast<FindIdFn>(*findId);
    g_registryWeak = reinterpret_cast<const std::uint64_t*>(*registry);
    g_triggerTag   = reinterpret_cast<const std::uint64_t*>(*trigger);
    return true;
}

bool Installed() noexcept
{
    return g_findId != nullptr;
}

std::optional<std::uint64_t> FirstTagOf(const void* actor) noexcept
{
    if (actor == nullptr || ReadField<std::int32_t>(actor, facts::kActorTagsCount) <= 0)
    {
        return std::nullopt;
    }
    const void* tags = players::actor_fields::Tags(actor);
    return tags == nullptr ? std::nullopt : std::optional<std::uint64_t>(ReadField<std::uint64_t>(tags, 0));
}

std::optional<std::uint64_t> TriggerTag() noexcept
{
    return g_triggerTag == nullptr ? std::nullopt : std::optional<std::uint64_t>(*g_triggerTag);
}

std::optional<std::int32_t> CountOf(std::uint64_t tag) noexcept
{
    if (g_findId == nullptr)
    {
        return std::nullopt;
    }
    const void* registry = players::weak_object::ObjectAt(g_registryWeak);
    if (registry == nullptr)
    {
        return std::nullopt;
    }
    const auto*  set   = static_cast<const std::byte*>(registry) + facts::kTagRegistryMap;
    std::int32_t index = -1;
    g_findId(set, &index, tag);
    if (index < 0)
    {
        return 0;
    }
    const void* elements = ReadPointer(set, 0);
    return elements == nullptr ? std::nullopt
                               : std::optional<std::int32_t>(ReadField<std::int32_t>(
                                     elements, static_cast<std::uint32_t>(index) * facts::kTagRegistryElementSize +
                                                   facts::kTagRegistryCount));
}

Counts Read(const void* member) noexcept
{
    Counts counts;
    if (const auto tag = FirstTagOf(member))
    {
        counts.member = CountOf(*tag);
    }
    if (const auto tag = TriggerTag())
    {
        counts.trigger = CountOf(*tag);
    }
    return counts;
}

}  // namespace lss::session::tag_counts
