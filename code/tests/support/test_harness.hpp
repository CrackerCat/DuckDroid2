#pragma once

#include <cstdint>
#include <cstdio>

namespace duckemu::test {

inline int g_failures = 0;

inline void checkTrue(bool condition, const char* file, int line,
                      const char* expression) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
        ++g_failures;
    }
}

inline void checkEqU64(uint64_t actual, uint64_t expected, const char* file,
                       int line, const char* expression) {
    if (actual != expected) {
        std::fprintf(stderr,
                     "FAIL %s:%d: %s == 0x%016llx, expected 0x%016llx\n", file,
                     line, expression, static_cast<unsigned long long>(actual),
                     static_cast<unsigned long long>(expected));
        ++g_failures;
    }
}

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::printf("%s: OK\n", name);
        return 0;
    }
    std::printf("%s: %d check(s) failed\n", name, g_failures);
    return 1;
}

} // namespace duckemu::test

#define CHECK(expression)                                                      \
    ::duckemu::test::checkTrue((expression), __FILE__, __LINE__, #expression)

#define CHECK_EQ(actual, expected)                                             \
    ::duckemu::test::checkEqU64(static_cast<uint64_t>(actual),                 \
                                static_cast<uint64_t>(expected), __FILE__,     \
                                __LINE__, #actual)

#define CHECK_RESULT(result)                                                   \
    do {                                                                       \
        const auto& _check_result = (result);                                  \
        if (!_check_result.ok()) {                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s -> %s\n", __FILE__, __LINE__, \
                         #result, _check_result.error().message.c_str());      \
            ++::duckemu::test::g_failures;                                     \
        }                                                                      \
    } while (0)
