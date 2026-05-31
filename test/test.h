/* test/test.h -- tiny zero-dep test harness. */
#ifndef JERBOA_TEST_H
#define JERBOA_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int  test_failed = 0;
static int  test_total  = 0;
static int  test_passed = 0;
static const char *test_current = "";

#define TEST(name) \
    static void name(void); \
    static void run_##name(void) { \
        test_current = #name; \
        test_failed = 0; \
        test_total++; \
        name(); \
        if (test_failed == 0) { \
            test_passed++; \
            printf("  ok   %s\n", #name); \
        } else { \
            printf("  FAIL %s\n", #name); \
        } \
    } \
    static void name(void)

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "    %s:%d: %s: CHECK(%s) failed\n", \
                __FILE__, __LINE__, test_current, #cond); \
        test_failed = 1; \
    } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "    %s:%d: %s: %s (%ld) != %s (%ld)\n", \
                __FILE__, __LINE__, test_current, #a, _a, #b, _b); \
        test_failed = 1; \
    } \
} while (0)

#define CHECK_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        fprintf(stderr, "    %s:%d: %s: \"%s\" != \"%s\"\n", \
                __FILE__, __LINE__, test_current, _a, _b); \
        test_failed = 1; \
    } \
} while (0)

#define TEST_SUMMARY() do { \
    printf("\n%d/%d passed\n", test_passed, test_total); \
    return (test_passed == test_total) ? 0 : 1; \
} while (0)

#endif
