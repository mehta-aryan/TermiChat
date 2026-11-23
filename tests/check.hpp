#pragma once
// tests/check.hpp – the tiny pass/fail helper shared by the unit tests.
// Plain checks, no external framework.

#include <cstdio>

inline int g_pass = 0;
inline int g_fail = 0;

inline void check(bool cond, const char* label) {
    std::printf("  %s: %s\n", cond ? "PASS" : "FAIL", label);
    cond ? ++g_pass : ++g_fail;
}

// Prints the totals; the return value is the process exit code.
inline int summary() {
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
