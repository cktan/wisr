# dg.mk -- the rules every Makefile under dg/ shares.
#
# A directory's Makefile sets top, the way back to dg/, and then
# any of these before including this file:
#
#   SUBDIRS    directories to run make in, in order
#   SKIP       .c files here that are not compiled, with the reason
#              beside them
#   FRONTEND   1 to compile for a client: -DFRONTEND
#   BOTH       1 to compile every file twice, x.o for the server
#              and x_fe.o for a client, as src/common and src/port
#              need; SKIP_BE and SKIP_FE leave a file out of one
#   INCLUDES   more -I flags
#
# Every .c file is compiled as C++ (sm/PART4.md section 2), and
# as strict C++: src/tools/cxx_keywords.py renamed the identifiers
# that are C++ keywords, and src/tools/cxx_casts.py wrote the casts
# C does not ask for. -w hides the warnings, which nobody has gone
# through yet.

CXX ?= g++
OPT ?= -O0 -g
STD := -x c++ -std=gnu++20 -w
CPPFLAGS := -I$(top)/src/include $(INCLUDES) -D_GNU_SOURCE

SRCS := $(filter-out $(SKIP),$(wildcard *.c))
ifeq ($(BOTH),1)
OBJS := $(patsubst %.c,%.o,$(filter-out $(SKIP_BE),$(SRCS))) \
        $(patsubst %.c,%_fe.o,$(filter-out $(SKIP_FE),$(SRCS)))
else
OBJS := $(SRCS:.c=.o)
endif
ifeq ($(FRONTEND),1)
CPPFLAGS += -DFRONTEND
endif

.PHONY: all clean format $(SUBDIRS)

all: $(SUBDIRS) $(OBJS)

# The same goal in each subdirectory: all, clean or format.
$(SUBDIRS):
	$(MAKE) -C $@ $(MAKECMDGOALS)

%.o: %.c
	$(CXX) $(STD) $(OPT) $(CPPFLAGS) -MMD -MP -c -o $@ $<

%_fe.o: %.c
	$(CXX) $(STD) $(OPT) $(CPPFLAGS) -DFRONTEND -MMD -MP -c -o $@ $<

clean: $(SUBDIRS)
	rm -f *.o *.d

# .clang-format-ignore at dg/ keeps the generators' headers as they are.
format: $(SUBDIRS)
	$(if $(wildcard *.c *.h),clang-format -i $(wildcard *.c *.h))

# Only the objects' own: utils/probes.d is DTrace's, not make's.
-include $(OBJS:.o=.d)
