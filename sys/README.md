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
| Interrupts and timers | `include/sys.h` | `INTERRUPT.md` | built and tested |

An interface is C in style: `sys_` names outside any namespace,
plain integers for times and counts, and a function pointer with
a `void *` where a callback is needed. What is C++ is what must
be: a handle that owns its object is a `std::shared_ptr`, and a
`std::mutex` a wait is made on is passed by pointer.

sys has one header, `include/sys.h`, included as `"sys.h"`: a
name no header of sm or dg has.

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
