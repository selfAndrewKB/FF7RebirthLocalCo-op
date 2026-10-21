#pragma once

// The host tests' one check and one verdict.
//
// Every test is a plain executable: it prints each case's name, checks with EXPECT, and ends with
// `return lss::test::Report();`. A failed check prints its line and expression and is counted; the
// process exit code is 1 when any check failed, which is what ctest reads.

#include <cstdio>

namespace lss::test {

inline int g_failures = 0;

/// Prints the verdict and gives the process exit code: 0 when every EXPECT held.
inline int Report() noexcept
{
    std::printf(g_failures == 0 ? "all tests passed\n" : "%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace lss::test

#define EXPECT(condition)                                                                \
    do                                                                                   \
    {                                                                                    \
        if (!(condition))                                                                \
        {                                                                                \
            std::printf("  FAILED line %d: %s\n", __LINE__, #condition);                 \
            ++::lss::test::g_failures;                                                   \
        }                                                                                \
    } while (false)
