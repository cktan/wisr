/*
 * main.cc -- the doctest runner. Every other file in test/
 * contributes test cases; this one supplies main(), and the
 * binary's operator new, which fails on demand (alloc.h).
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "alloc.h"
#include "doctest.h"

#include <stdlib.h>

#include <new>

thread_local long t_alloc_budget = -1;

/*
 * The test binary's global operator new: plain malloc, except that
 * an allocation fails once the calling thread's t_alloc_budget is
 * spent. A failure does what the library's does: it calls the
 * new-handler if there is one, and throws std::bad_alloc if not.
 */
void *operator new(std::size_t n) {
  for (;;) {
    // Allocate while the budget lasts, spending from it.
    if (t_alloc_budget != 0) {
      if (t_alloc_budget > 0) {
        t_alloc_budget--;
      }
      if (void *p = malloc(n ? n : 1)) {
        return p;
      }
    }
    // Out of memory: the handler's policy, or bad_alloc.
    std::new_handler h = std::get_new_handler();
    if (!h) {
      throw std::bad_alloc();
    }
    h();
  }
}
