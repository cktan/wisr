/*
 * sm1_defer.h -- run a block when the enclosing scope exits.
 *
 *   defer { closedir(dir); };
 *
 * The block runs however the scope is left -- a return, a
 * break, or an exception -- and several defers in one scope
 * run in reverse order, as destructors do. The block captures
 * by reference, so it sees the variables as they are at exit.
 *
 * A cleanup that must happen on failure but not on success
 * uses a named guard and dismisses it once the work succeeds:
 *
 *   sm1::scope_guard_t undo([&] { remove_reservation(); });
 *   ...
 *   undo.dismiss(); // success: keep what was done
 *
 * `defer` is a macro, so it replaces every identifier of that
 * name in a file that includes this header.
 */
#ifndef SM1_DEFER_H
#define SM1_DEFER_H

#include <type_traits>
#include <utility>

namespace sm1 {

/* Runs a function when it goes out of scope, unless dismissed. */
template <typename F> class scope_guard_t {
public:
  /* Take the function to run at scope exit. */
  explicit scope_guard_t(F f) : m_func(std::move(f)) {}

  /* Run the function, unless the guard was dismissed. */
  ~scope_guard_t() {
    if (m_active) {
      m_func();
    }
  }

  /* A guard is never copied, so its function never runs twice. */
  scope_guard_t(const scope_guard_t &) = delete;
  scope_guard_t &operator=(const scope_guard_t &) = delete;

  /* Do not run the function at scope exit after all. */
  void dismiss() { m_active = false; }

private:
  F m_func;
  bool m_active = true;
};

/* The left operand of the + that turns a defer block into a guard. */
struct defer_tag_t {};

/* Build the guard for `defer { ... };` from its block. */
template <typename F>
scope_guard_t<std::decay_t<F>> operator+(defer_tag_t, F &&f) {
  return scope_guard_t<std::decay_t<F>>(std::forward<F>(f));
}

} // namespace sm1

#define SM1_DEFER_CAT2(a, b) a##b
#define SM1_DEFER_CAT(a, b) SM1_DEFER_CAT2(a, b)

/* defer { ... }; -- the block becomes a lambda run at scope exit. */
#define defer                                                                  \
  const auto SM1_DEFER_CAT(sm1_defer_, __COUNTER__) =                          \
      ::sm1::defer_tag_t{} + [&]()

#endif /* SM1_DEFER_H */
