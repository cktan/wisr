/*
 * alloc.h -- making the test binary's allocations fail on demand
 * (main.cc).
 */
#ifndef SM2_TEST_ALLOC_H
#define SM2_TEST_ALLOC_H

/*
 * How many more allocations the calling thread may make before
 * each one fails; -1, the default, is unlimited.
 */
extern thread_local long t_alloc_budget;

#endif /* SM2_TEST_ALLOC_H */
