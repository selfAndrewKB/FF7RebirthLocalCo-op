#include "platform/image.h"

#include "platform/log.h"
#include "platform/rip_relative.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstring>

namespace lss::platform {

namespace {

constexpr std::uintptr_t kIdaImageBase = 0x140000000;

/// True if every byte of [address, address + length) is committed and readable. Lying inside the
/// image is not enough: a page can still be inaccessible, and reading it would crash the game
/// instead of letting a startup check fail cleanly.
bool IsReadable(std::uintptr_t address, std::size_t length) noexcept
{
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const std::uintptr_t end = address + length;
    while (address < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & kReadable) == 0 || (info.Protect & PAGE_GUARD) != 0)
        {
            return false;
        }
        address = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

}  // namespace

std::optional<GameImage> GameImage::Locate() noexcept
{
    const HMODULE module = GetModuleHandleW(kGameExe);
    if (module == nullptr)
    {
        return std::nullopt;
    }

    // The loader has already validated these headers; SizeOfImage bounds every address we use. The build is
    // recognised by the three header values the linker writes (timestamp, image size, checksum).
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const GameBuild* build =
        FindBuild(nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum);
    static bool warned = false;  // once: the image is located again by the control commands
    if (build == nullptr && !warned)
    {
        warned = true;
        LogWarning(L"unknown game build (timestamp 0x{:08X}, image size 0x{:X}, checksum 0x{:08X}): addresses are used "
                   L"as the research build's, and every check decides",
                   nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum);
    }
    SetRunningBuild(build != nullptr ? build->index : BuildIndex::Research);
    return GameImage(base, nt->OptionalHeader.SizeOfImage, build);
}

std::optional<std::uintptr_t> GameImage::Rebase(std::uintptr_t idaAddress, std::size_t length) const noexcept
{
    if (idaAddress < kIdaImageBase || length > size_)
    {
        return std::nullopt;
    }
    std::uintptr_t offset = idaAddress - kIdaImageBase;
    if (Translating())
    {
        const auto translated = offset <= 0xFFFFFFFFu ? TranslateRva(*build_, static_cast<std::uint32_t>(offset),
                                                                     static_cast<std::uint32_t>(length))
                                                      : std::nullopt;
        if (!translated)
        {
            LogDebug(L"no {} address for IDA 0x{:X} (+{} bytes)", build_->name, idaAddress, length);
            return std::nullopt;
        }
        offset = *translated;
    }
    if (offset > size_ || length > size_ - offset)
    {
        return std::nullopt;
    }
    return base_ + offset;
}

bool GameImage::BytesMatch(std::uintptr_t idaAddress, std::span<const std::uint8_t> expected) const noexcept
{
    const auto live = Rebase(idaAddress, expected.size());
    if (!live || !IsReadable(*live, expected.size()))
    {
        return false;
    }
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(*live), expected.size());
    if (Translating())
    {
        const bool matches =
            MatchesTranslated(*build_, static_cast<std::uint32_t>(idaAddress - kIdaImageBase), expected, bytes);
        if (!matches && Enabled(Level::Debug))
        {
            // Which byte first differs from the research build's (moved fields included), for re-finding it in IDA.
            std::size_t at = 0;
            while (at < bytes.size() && bytes[at] == expected[at])
            {
                ++at;
            }
            LogDebug(L"{}: the {} bytes at IDA 0x{:X} (live 0x{:X}) are not the research build's; first difference at "
                     L"+{} (live 0x{:02X}, research 0x{:02X})",
                     build_->name, expected.size(), idaAddress, *live - base_ + kIdaImageBase, at,
                     at < bytes.size() ? bytes[at] : 0, at < expected.size() ? expected[at] : 0);
        }
        return matches;
    }
    return std::memcmp(bytes.data(), expected.data(), expected.size()) == 0;
}

std::optional<std::uintptr_t> GameImage::RipTarget(std::uintptr_t                idaSite,
                                                   std::span<const std::uint8_t> opcode) const noexcept
{
    const std::size_t length = opcode.size() + sizeof(std::int32_t);
    const auto        live   = Rebase(idaSite, length);
    if (!live || !BytesMatch(idaSite, opcode) || !IsReadable(*live + opcode.size(), sizeof(std::int32_t)))
    {
        return std::nullopt;
    }
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(*live + opcode.size()), sizeof(displacement));
    return platform::RipTarget(*live, length, displacement);
}

std::span<const std::uint8_t> GameImage::EntryInstruction(std::uintptr_t idaAddress,
                                                          std::span<const std::uint8_t> researchFirst) const noexcept
{
    if (!Translating() || idaAddress < kIdaImageBase || idaAddress - kIdaImageBase > 0xFFFFFFFFu)
    {
        return researchFirst;
    }
    const ReplacedAddress* replaced =
        FindReplacedAddress(*build_, static_cast<std::uint32_t>(idaAddress - kIdaImageBase));
    if (replaced == nullptr)
    {
        return researchFirst;  // matched through a range: the instruction's bytes are the research build's
    }
    if (replaced->firstInstruction != researchFirst.size() || replaced->bytes.size() < researchFirst.size())
    {
        Log(L"{}: the first instruction at IDA 0x{:X} is not as long as the research build's ({} bytes); no entry "
            L"redirect there",
            build_->name, idaAddress, researchFirst.size());
        return {};
    }
    return replaced->bytes.first(researchFirst.size());
}

std::uintptr_t GameImage::ResearchAddress(std::uintptr_t live) const noexcept
{
    if (live < base_ || live - base_ >= size_)
    {
        return live;
    }
    const auto rva = static_cast<std::uint32_t>(live - base_);
    if (Translating())
    {
        if (const auto research = ResearchRva(*build_, rva))
        {
            return kIdaImageBase + *research;
        }
    }
    return kIdaImageBase + rva;
}

std::optional<std::uintptr_t> GameImage::ReadPointer(std::uintptr_t idaAddress) const noexcept
{
    const auto live = Rebase(idaAddress, sizeof(std::uintptr_t));
    if (!live || !IsReadable(*live, sizeof(std::uintptr_t)))
    {
        return std::nullopt;
    }
    return *reinterpret_cast<const std::uintptr_t*>(*live);
}

}  // namespace lss::platform
