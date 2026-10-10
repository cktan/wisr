# sys

System services for the rest of the repository: what `sm/` and
`dg/` both need from the operating system and the C++ runtime,
built once, below both of them.

sys depends on nothing else in the repository: only the C++
standard library and POSIX. sm and dg may depend on sys; sys
never includes a header of theirs.

## What is here

| Service | Interface | Design | State |
| --- | --- | --- | --- |
| Interrupts and timers | `include/sys/interrupt.h` | `INTERRUPT.md` | built and tested |

An interface is C in style: `sys_` names outside any namespace,
opaque handles, plain integers for times and counts, and a
function pointer with a `void *` where a callback is needed.
What must be C++ -- a `std::mutex` a wait is made on, say -- is
passed by pointer.

A header is included by its path under `include/`, as
`"sys/interrupt.h"`, so its name cannot collide with a header of
sm or dg.

## Building

```
make -j4          build/libsys.a, and the test binary
make test         build, then run the tests
make tsan         the tests under the thread sanitizer
make asan         the tests under the address and UB sanitizers
make format       clang-format src/ and test/ in place
make clean        remove build/
```

Setting DEBUG in the environment builds -O1 -g instead of -O3.
