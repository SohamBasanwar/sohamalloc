/*
 * prefixed/producer_consumer -- tests/producer_consumer.c built with -DTEST_PREFIXED: every allocation
 * goes through the soham_* entry points (T_* macros in common/test_util.h),
 * linked against libsohamalloc-prefixed.a (optionally sanitizer-built) or
 * against prefixed/libc_shim.c (run_tests.sh --prefixed / --sanitize / --shim).
 */
#ifndef TEST_PREFIXED
#error "build with -DTEST_PREFIXED=1 (tests/run_tests.sh --prefixed)"
#endif
#include "../producer_consumer.c"
