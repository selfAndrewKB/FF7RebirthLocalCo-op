#pragma once

// Capture-and-replay of the game's placement calls, written once (hud/battle/popups_code.h records battle popups,
// hud/battle/enemy_gauges_code.h the untargeted enemy gauges). A record is a fixed array of placements with a count
// and a `replaying` byte; machine code the mod writes near the game appends one placement per call of the game's own
// placement function, unless the record is full or the mod is replaying (its own calls are not the game's); each HUD
// pass then places the recorded placements again for its player, and the frame's end clears the record.
//
// The code's head is the same for every record: `mov rax, &record; cmp byte [rax+4], 0; jne done; mov r10d, [rax];
// cmp r10d, capacity; jae done; lea r11d, [r10+1]; mov [rax], r11d` (32 bytes), then the element scale (r10 *=
// sizeof the placement), `add rax, r10` (the placements start at +10h), the payload that copies the call's
// arguments into [rax+10h..], and the replaced instruction at `done`. RecordAppendCode builds it from the payload
// and the replaced instruction, which stay hand-written and reviewed in each code header; the host tests execute
// the composed bytes. Only rax, r10, r11 and the flags change in the head.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::hud {

/// A record of up to `N` placements of `Placement` (a trivially copyable struct the code writes from +10h).
template <class Placement, std::uint32_t N>
struct alignas(16) PlacementRecord
{
    static constexpr std::uint32_t kCapacity = N;

    std::uint32_t count     = 0;
    std::uint8_t  replaying = 0;
    std::uint8_t  pad[11]{};
    Placement     placements[N];

    /// The last placement the game made this frame whose `keyOf(placement)` is `wanted`, or null.
    template <class KeyOf>
    const Placement* Last(std::uint64_t wanted, KeyOf keyOf) const noexcept
    {
        const std::uint32_t n = count < N ? count : N;
        for (std::uint32_t i = n; i-- > 0;)
        {
            if (keyOf(placements[i]) == wanted)
            {
                return &placements[i];
            }
        }
        return nullptr;
    }

    /// The frame's end: the record starts empty.
    void EndFrame() noexcept { count = 0; }
};

/// While it lives the mod is replaying: the code records nothing.
template <class Record>
class Replaying
{
public:
    explicit Replaying(Record& record) noexcept : record_(record) { record_.replaying = 1; }
    ~Replaying() { record_.replaying = 0; }
    Replaying(const Replaying&)            = delete;
    Replaying& operator=(const Replaying&) = delete;

private:
    Record& record_;
};

inline constexpr std::size_t kRecordHeadSize  = 32;  // the shared head
inline constexpr std::size_t kRecordScaleSize = 4;   // the element scale (imul r10, r10, imm8 or shl r10, imm8)
inline constexpr std::size_t kRecordAddSize   = 3;   // add rax, r10

/// The capture code for `record`: the head with `capacity`, `scale`, `add rax, r10`, `payload`, then `replaced` at
/// `done` (the jump back to the next instruction is appended by the installer). `capacity` fits an imm8.
template <std::size_t PayloadN, std::size_t ReplacedN>
std::array<std::uint8_t, kRecordHeadSize + kRecordScaleSize + kRecordAddSize + PayloadN + ReplacedN>
RecordAppendCode(const void* record, std::uint32_t capacity, const std::array<std::uint8_t, kRecordScaleSize>& scale,
                 const std::uint8_t (&payload)[PayloadN], const std::uint8_t (&replaced)[ReplacedN]) noexcept
{
    constexpr std::size_t kDone = kRecordHeadSize + kRecordScaleSize + kRecordAddSize + PayloadN;
    static_assert(kDone - 16 <= 0x7F && kDone - 25 <= 0x7F, "the done label is within a short jump");
    std::array<std::uint8_t, kDone + ReplacedN> code = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,                         //  0: mov  rax, &record
        0x80, 0x78, 0x04, 0x00,                                     // 10: cmp  byte [rax+4], 0   replaying
        0x75, static_cast<std::uint8_t>(kDone - 16),                // 14: jne  done
        0x44, 0x8B, 0x10,                                           // 16: mov  r10d, [rax]       count
        0x41, 0x83, 0xFA, static_cast<std::uint8_t>(capacity & 0x7F),  // 19: cmp  r10d, capacity
        0x73, static_cast<std::uint8_t>(kDone - 25),                // 23: jae  done
        0x45, 0x8D, 0x5A, 0x01,                                     // 25: lea  r11d, [r10+1]
        0x44, 0x89, 0x18,                                           // 29: mov  [rax], r11d       count + 1
        scale[0], scale[1], scale[2], scale[3],                     // 32: r10 *= sizeof(Placement)
        0x4C, 0x01, 0xD0,                                           // 36: add  rax, r10
    };
    std::memcpy(&code[kRecordHeadSize + kRecordScaleSize + kRecordAddSize], payload, PayloadN);
    std::memcpy(&code[kDone], replaced, ReplacedN);
    const auto address = reinterpret_cast<std::uint64_t>(record);
    std::memcpy(&code[2], &address, sizeof(address));
    return code;
}

}  // namespace lss::hud
