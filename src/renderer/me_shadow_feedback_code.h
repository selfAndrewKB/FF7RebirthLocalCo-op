#pragma once

// The machine code that lets player 2's family skip the ME shadow pass's streaming-feedback writes
// (RE-perf-me-shadows.md, Phase 3, option D; performance work).
//
// ME_ChunkDraw_FullPipeline_cand culls and draws one shadow batch. Between reserving a per-chunk transition array and
// freeing it, it records the pre-transitions of every chunk's two feedback UAVs, two WriteStreamingDataFromBatchedCS
// runs over the chunks, and the post-transitions: about 70 dispatches and 12 barrier points per frame and family. Only
// the main pass's feedback copy reads those buffers, and player 2's family does not run it (me_feedback_once.h), so its
// writes are discarded, except for chunks of shadow groups wholly outside player 1's view, where player 2's shadow
// request merges into player 1's next copy (player 1's own request decides there instead, as in solo).
//
// One whole instruction is replaced through platform/code_redirect.h: the `jbe` (count 0) after
// `cmp dword [r14+18h],<zero register>` that enters the pre-transition loop (research 0x140F0DC3A, 1.005 0x140C882B7;
// one 16-byte block in both). While the switch byte is set and the view's family is an additional one (FViewInfo+0 is
// the renderer's family copy, +0x74 its bAdditionalViewFamily, the byte me_feedback_once reads at renderer+0x84), the
// code jumps to the free of the transition array (research 0x140F0E345, 1.005 0x140C88A01), past the whole region. The
// research build's code after it needs r12 = 0 and r13 = the view, both already true, and r15d = 0x30 (the command
// list's allocator offset, set inside the region at 0x140F0DE14 and read by `mov [rsi+r15],rax` at 0x140F0EA02; before
// it r15 is rsi+0x30); 1.005's frees rbx (the array, [rbp-68h]) and needs edi = 0. Otherwise it redoes the compare
// (the zero register reads 0) and takes the `jbe` or resumes. rax and the flags are dead at the site. Each path counts
// itself in the fields (lock inc).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lss::renderer {

/// The fields the code reads and counts into, at fixed offsets (the code addresses them from one base).
struct ShadowFeedbackFields
{
    alignas(8) std::uint64_t skip = 0;  // +0x00: nonzero while player 2's family skips (stream-feedback p1, mask bit 16)
    std::uint64_t skipped       = 0;    // +0x08: player 2's family, region skipped (per shadow batch)
    std::uint64_t passedOne     = 0;    // +0x10: player 1's family (or any non-additional one), the game's code
    std::uint64_t passedTwo     = 0;    // +0x18: player 2's family with the switch off
};
static_assert(offsetof(ShadowFeedbackFields, skipped) == 0x08 && offsetof(ShadowFeedbackFields, passedOne) == 0x10 &&
              offsetof(ShadowFeedbackFields, passedTwo) == 0x18);

/// The instruction the site replaces (`jbe rel32`, +0x11C in both builds), and the compare just before it.
inline constexpr std::uint8_t kShadowFeedbackSiteBytes[]        = {0x0F, 0x86, 0x1C, 0x01, 0x00, 0x00};
inline constexpr std::uint8_t kShadowFeedbackCompareBytes[]     = {0x45, 0x39, 0x66, 0x18};  // cmp [r14+18h],r12d
inline constexpr std::uint8_t kShadowFeedbackCompareBytes1005[] = {0x45, 0x39, 0x6E, 0x18};  // cmp [r14+18h],r13d

inline constexpr std::size_t kShadowFeedbackCodeSize     = 83;
inline constexpr std::size_t kShadowFeedbackCodeSize1005 = 83;

namespace detail {
inline void PutQword(std::uint8_t* at, std::uint64_t value) noexcept
{
    std::memcpy(at, &value, sizeof(value));
}
}  // namespace detail

/// Research build. `skipTo` and `emptyTo` are the live addresses of the array's free and of the `jbe` target.
inline std::array<std::uint8_t, kShadowFeedbackCodeSize> ShadowFeedbackCode(const ShadowFeedbackFields& fields,
                                                                            std::uintptr_t skipTo,
                                                                            std::uintptr_t emptyTo) noexcept
{
    std::array<std::uint8_t, kShadowFeedbackCodeSize> code = {
        0x49, 0x8B, 0x45, 0x00,                    //  0: mov  rax, [r13]          the view's family copy
        0x80, 0x78, 0x74, 0x00,                    //  4: cmp  byte [rax+74h], 0   bAdditionalViewFamily
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  8: mov  rax, &fields        (flags kept)
        0x74, 0x25,                                // 18: je   one (57)
        0x80, 0x38, 0x00,                          // 20: cmp  byte [rax], 0       the switch
        0x74, 0x19,                                // 23: je   two (50)
        0xF0, 0x48, 0xFF, 0x40, 0x08,              // 25: lock inc qword [rax+8]   skipped
        0x41, 0xBF, 0x30, 0x00, 0x00, 0x00,        // 30: mov  r15d, 30h           the allocator offset, set there
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // 36: jmp  [rip+0]             the array's free
        0, 0, 0, 0, 0, 0, 0, 0,                    // 42: skipTo
        0xF0, 0x48, 0xFF, 0x40, 0x18,              // 50: two: lock inc qword [rax+18h]  passedTwo
        0xEB, 0x05,                                // 55: jmp  compare (62)
        0xF0, 0x48, 0xFF, 0x40, 0x10,              // 57: one: lock inc qword [rax+10h]  passedOne
        0x41, 0x83, 0x7E, 0x18, 0x00,              // 62: compare: cmp dword [r14+18h], 0  (the replaced test)
        0x77, 0x0E,                                // 67: ja   resume (83)
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // 69: jmp  [rip+0]             the jbe target
        0, 0, 0, 0, 0, 0, 0, 0,                    // 75: emptyTo
    };                                             // 83: resume (the jump back is appended here)
    detail::PutQword(&code[10], reinterpret_cast<std::uint64_t>(&fields));
    detail::PutQword(&code[42], skipTo);
    detail::PutQword(&code[75], emptyTo);
    return code;
}

/// Game version 1.005: the view is in r12, and the code at the free needs rbx = the array and edi = 0.
inline std::array<std::uint8_t, kShadowFeedbackCodeSize1005> ShadowFeedbackCode1005(const ShadowFeedbackFields& fields,
                                                                                    std::uintptr_t skipTo,
                                                                                    std::uintptr_t emptyTo) noexcept
{
    std::array<std::uint8_t, kShadowFeedbackCodeSize1005> code = {
        0x49, 0x8B, 0x04, 0x24,                    //  0: mov  rax, [r12]          the view's family copy
        0x80, 0x78, 0x74, 0x00,                    //  4: cmp  byte [rax+74h], 0   bAdditionalViewFamily
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,        //  8: mov  rax, &fields        (flags kept)
        0x74, 0x25,                                // 18: je   one (57)
        0x80, 0x38, 0x00,                          // 20: cmp  byte [rax], 0       the switch
        0x74, 0x19,                                // 23: je   two (50)
        0xF0, 0x48, 0xFF, 0x40, 0x08,              // 25: lock inc qword [rax+8]   skipped
        0x48, 0x8B, 0x5D, 0x98,                    // 30: mov  rbx, [rbp-68h]      the array, freed there
        0x31, 0xFF,                                // 34: xor  edi, edi
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // 36: jmp  [rip+0]             the array's free
        0, 0, 0, 0, 0, 0, 0, 0,                    // 42: skipTo
        0xF0, 0x48, 0xFF, 0x40, 0x18,              // 50: two: lock inc qword [rax+18h]  passedTwo
        0xEB, 0x05,                                // 55: jmp  compare (62)
        0xF0, 0x48, 0xFF, 0x40, 0x10,              // 57: one: lock inc qword [rax+10h]  passedOne
        0x41, 0x83, 0x7E, 0x18, 0x00,              // 62: compare: cmp dword [r14+18h], 0  (the replaced test)
        0x77, 0x0E,                                // 67: ja   resume (83)
        0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,        // 69: jmp  [rip+0]             the jbe target
        0, 0, 0, 0, 0, 0, 0, 0,                    // 75: emptyTo
    };                                             // 83: resume (the jump back is appended here)
    detail::PutQword(&code[10], reinterpret_cast<std::uint64_t>(&fields));
    detail::PutQword(&code[42], skipTo);
    detail::PutQword(&code[75], emptyTo);
    return code;
}

}  // namespace lss::renderer
