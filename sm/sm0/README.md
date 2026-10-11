# sm0

Level 0 of the stack: what every level above needs that is
about no storage, and the system services sm and dg both build
on. sm0 includes nothing of the levels and links no RocksDB;
it includes only the root's `include/sm_error.h`, the stack's
error codes, besides the C++ standard library and POSIX.

| What | Header | Source |
| --- | --- | --- |
| the calling thread's error state: `errnum`, `errmsg`, `errname`, `retryable`, `seterr`, `reseterr` | `include/sm0.h` | `src/error.cc` |
| the process-wide log hook: `loglevel_t`, `log_fn`, `set_log` | `include/sm0.h` | `src/log.cc` |
| setting an error with a formatted message, from an errno, or for a bad name | `include/sm0_error.h` | `src/error.cc` |
| reporting through the log hook: `log_msg` | `include/sm0_log.h` | `src/log.cc` |
| `std::format` into a fixed buffer, allocating nothing | `include/sm0_format.h` | `src/format.cc` |
| defer blocks and scope guards | `include/sm0_defer.h` | — |
| fixed-width big-endian integers | `include/sm0_codec.h` | — |
| the vendored doctest test framework, used by every level | `include/doctest.h` | — |
| interrupting a thread, interruptible waits, and timers that interrupt (`INTERRUPT.md`) | `include/sm_sys.h` | `src/interrupt.cc` |

Everything is in `namespace sm0`. `sm1/include/sm1.h` brings
the error state and the log hook into `namespace sm1`, so the
stack and its callers call them `sm1::errnum()`,
`sm1::set_log()` and so on; `sm1/API.md` documents them there.
sm1's `src/error.h` and `src/log.h` likewise bring the setters
and `log_msg` into its `namespace wisr`, beside
`set_status_error`, which knows RocksDB and so stays in sm1.

## sm_sys.h

`sm_sys.h` is what was `sys/`, the system services. Every
name in it is public, for sm and its users alike: dg includes
it directly, through `-I../sm/sm0/include`, and gets its code
in `libsm.a`. Its interface is C in style: `sm_` names outside
any namespace, plain integers for times and counts, and a
function pointer with a `void *` where a callback is needed.
What is C++ is what must be: a handle that owns its object is
a `std::shared_ptr`, and a `std::mutex` a wait is made on is
passed by pointer.

## Building

```
make -j4          build/libsm0.a, and the test binary
make test         build, then run the tests
make tsan         the tests under the thread sanitizer
make asan         the tests under the address and UB sanitizers
make format       clang-format include/, src/ and test/ in place
make clean        remove build/
```
