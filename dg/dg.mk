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
# Every directory may include sm_sys.h: the services dg is built on,
# in sm's level 0 (../sm/sm0).
CPPFLAGS := -I$(top)/src/include -I$(top)/../sm/sm0/include $(INCLUDES) -D_GNU_SOURCE

SRCS := $(filter-out $(SKIP),$(wildcard *.c))
OBJS := $(SRCS:.c=.$(OBJ))

.PHONY: all clean format $(SUBDIRS)

all: $(SUBDIRS) $(OBJS)

# The same goal in each subdirectory: all, clean or format.
$(SUBDIRS):
	$(MAKE) -C $@ $(MAKECMDGOALS)

%.$(OBJ): %.c
	$(CXX) $(STD) $(OPT) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# Not *.d: utils/probes.d is a source file.
clean: $(SUBDIRS)
	rm -f $(SRCS:.c=.o) $(SRCS:.c=.d) $(SRCS:.c=.tsan.o) $(SRCS:.c=.tsan.d)

# .clang-format-ignore at dg/ keeps the generators' headers as they are.
format: $(SUBDIRS)
	$(if $(wildcard *.c *.h),clang-format -i $(wildcard *.c *.h))

# Only the objects' own: utils/probes.d is DTrace's, not make's.
-include $(OBJS:.o=.d)
