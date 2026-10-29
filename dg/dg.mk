# dg.mk -- the rules every Makefile under dg/ shares.
#
# A directory's Makefile sets top, the way back to dg/, and then
# any of these before including this file:
#
#   SUBDIRS    directories to run make in, in order
#   SKIP       .c files here that are not compiled, with the reason
#              beside them
#   INCLUDES   more -I flags
#
# OBJ is the suffix of an object, "o"; "make tsan" at dg/ makes it
# "tsan.o", so the thread sanitizer's objects, compiled for it, sit
# beside the others.
#
# Every .c file is compiled as C++ (sm/PART4.md section 2), and
# as strict C++: src/tools/cxx_keywords.py renamed the identifiers
# that are C++ keywords, and src/tools/cxx_casts.py wrote the casts
# C does not ask for. -w hides the warnings, which nobody has gone
# through yet.

CXX ?= g++
OBJ ?= o
ifeq ($(OBJ),tsan.o)
OPT := -O1 -g -fsanitize=thread
else
OPT ?= -O0 -g
endif
STD := -x c++ -std=gnu++20 -fno-rtti -w
# Assert() checks, on in every build here, which is a debug one; a
# release build turns them off with "make ASSERT=".
ASSERT ?= -DUSE_ASSERT_CHECKING
# Every directory may include sm_sys.h: the services dg is built on,
# in sm's level 0 (../sm/sm0); and aa.h, roles and privileges (../aa),
# which utils/pg_global.h includes.
CPPFLAGS := -I$(top)/src/include -I$(top)/../sm/sm0/include -I$(top)/../aa/include $(INCLUDES) -D_GNU_SOURCE $(ASSERT)

SRCS := $(filter-out $(SKIP),$(wildcard *.c))
OBJS := $(SRCS:.c=.$(OBJ))
# Objects here that no source compiles any more, of a .c deleted or
# skipped, plain, for the thread sanitizer or for the release build:
# the link takes every object under src/, so one left would still be
# linked.
STALE := $(filter-out $(SRCS:.c=.o) $(SRCS:.c=.tsan.o) $(SRCS:.c=.rel.o),\
                      $(wildcard *.o))

.PHONY: all clean format $(SUBDIRS)

all: $(SUBDIRS) $(OBJS)
ifneq ($(STALE),)
	rm -f $(STALE)
endif

# The same goal in each subdirectory: all, clean or format.
$(SUBDIRS):
	$(MAKE) -C $@ $(MAKECMDGOALS)

%.$(OBJ): %.c
	$(CXX) $(STD) $(OPT) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# Not *.d: utils/probes.d is a source file.
clean: $(SUBDIRS)
	rm -f $(SRCS:.c=.o) $(SRCS:.c=.d) $(SRCS:.c=.tsan.o) $(SRCS:.c=.tsan.d) \
	    $(SRCS:.c=.rel.o) $(SRCS:.c=.rel.d)

# .clang-format-ignore at dg/ keeps the generators' headers as they are.
format: $(SUBDIRS)
	$(if $(wildcard *.c *.h),clang-format -i $(wildcard *.c *.h))

# Only the objects' own: utils/probes.d is DTrace's, not make's.
-include $(OBJS:.o=.d)
