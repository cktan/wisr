# Copyright (c) 2026 CK Tan
# SPDX-License-Identifier: MIT

# wisr -- a stack of storage levels over RocksDB.
#
# Level 0 is sm0/, the error state, the log hook, the helpers
# and the system services every level uses; RocksDB is the store
# under the stack. Level 1 is sm1/, the MVCC storage manager.
# Level 2 is sm2/, types and records, and level 3 is sm3/, schema
# and index maintenance, being built.
#
# This file delegates to the levels, each of which has a Makefile
# of its own with the real targets, and builds the one thing that
# belongs to no level: the C wrapper, include/ and src/, into
# build/libsm.a, which bundles every level's objects so a C
# program links one archive (DESIGN.md section 2).
#
#   make -C sm1 test      a level's own tests alone
#
#   make              build every level, then build/libsm.a
#   make rel          build the rel test driver
#   make test         test every level, then the rel test suite
#   make clean        remove each level's build directory, and build/
#   make format       clang-format each level, rel, and src/
#
# LEVELS lists the levels that have a Makefile, in build order.
LEVELS := sm0 sm1 sm2 sm3

CC       ?= gcc
CXX      ?= g++
# As the levels': DEBUG=1, -O0 -g; otherwise -O3 -DNDEBUG.
ifeq ($(DEBUG),1)
OPTFLAGS ?= -O0 -g
else
OPTFLAGS ?= -O3 -DNDEBUG
endif
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -fno-rtti -Wall -Wextra -Iinclude -Ism0/include \
            -Ism1/include -Ism2/include -Ism3/include -MMD -MP
LDLIBS   += -lrocksdb -licui18n -licuuc -licudata -lpthread

BUILD     := build
LIB       := $(BUILD)/libsm.a
LEVEL_LIBS := sm0/$(BUILD)/libsm0.a sm1/$(BUILD)/libsm1.a \
              sm2/$(BUILD)/libsm2.a sm3/$(BUILD)/libsm3.a

.PHONY: all test clean format rel $(LEVELS)

all: $(LIB)

rel: $(LIB)
	$(MAKE) -C rel

$(LEVELS):
	$(MAKE) -C $@

# Each level links the ones below it, so they build first.
sm1: sm0
sm2: sm1
sm3: sm2

# The levels' archives are rebuilt by their own Makefiles; the
# levels are phony, so the bundle is redone after each build.
# The empty recipe matters: without one, make does not look at
# an archive's time again after its level rebuilt it, and the
# bundle, and rel with it, would keep the stale one.
# The levels' libraries: by the levels' own builds, or, with BUILD set
# elsewhere ("make BUILD=build/tsan OPTFLAGS=... build/tsan/libsm.a",
# as digres's tsan build does), each level's library alone, built under
# that directory with these flags.
ifeq ($(BUILD),build)
$(LEVEL_LIBS): $(LEVELS) ;
else
sm0/$(BUILD)/libsm0.a:
	$(MAKE) -C sm0 BUILD=$(BUILD) OPTFLAGS='$(OPTFLAGS)' $(BUILD)/libsm0.a
sm1/$(BUILD)/libsm1.a: sm0/$(BUILD)/libsm0.a
	$(MAKE) -C sm1 BUILD=$(BUILD) OPTFLAGS='$(OPTFLAGS)' $(BUILD)/libsm1.a
sm2/$(BUILD)/libsm2.a: sm1/$(BUILD)/libsm1.a
	$(MAKE) -C sm2 BUILD=$(BUILD) OPTFLAGS='$(OPTFLAGS)' $(BUILD)/libsm2.a
sm3/$(BUILD)/libsm3.a: sm2/$(BUILD)/libsm2.a
	$(MAKE) -C sm3 BUILD=$(BUILD) OPTFLAGS='$(OPTFLAGS)' $(BUILD)/libsm3.a
.PHONY: $(LEVEL_LIBS)
endif

# The compiler and flags the wrapper is built with, recorded as the
# levels record theirs, so a change of DEBUG rebuilds it.
FLAGS      := $(CXX) $(CXXFLAGS)
FLAGS_FILE := $(BUILD)/flags
$(shell mkdir -p $(BUILD); \
        [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = '$(FLAGS)' ] || \
        echo '$(FLAGS)' > $(FLAGS_FILE))

# The wrapper's flags, which sm_build_flags() reports with the levels'.
$(BUILD)/src/sm.o: src/sm.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -D'SM_BUILD_FLAGS="$(OPTFLAGS)"' -c -o $@ $<

# One archive of every level's objects and the wrapper's, by an
# ar script: ADDLIB keeps two levels' same-named objects apart.
$(LIB): $(BUILD)/src/sm.o $(LEVEL_LIBS)
	rm -f $@
	printf 'CREATE $@\nADDLIB sm0/$(BUILD)/libsm0.a\nADDLIB sm1/$(BUILD)/libsm1.a\nADDLIB sm2/$(BUILD)/libsm2.a\nADDLIB sm3/$(BUILD)/libsm3.a\nADDMOD $(BUILD)/src/sm.o\nSAVE\nEND\n' | ar -M

# Each level's own target, in order, stopping at the first level
# that fails; then the rel test driver.
test: rel
	@for d in $(LEVELS); do \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
	$(MAKE) -C rel test

clean:
	@for d in $(LEVELS); do \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
	$(MAKE) -C rel clean
	rm -rf $(BUILD)

format:
	@for d in $(LEVELS); do \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
	$(MAKE) -C rel format
	clang-format -i src/sm.cc

-include $(BUILD)/src/sm.d
