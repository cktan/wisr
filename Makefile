# wisr -- a stack of storage levels over RocksDB.
#
# Level 0 is RocksDB. Level 1 is sm1/, the MVCC storage manager.
# Level 2 is sm2/, types and records, and level 3 is sm3/, schema
# and index maintenance, which is not built yet.
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
#   make test         test every level, then the wrapper from C
#   make clean        remove each level's build directory, and build/
#   make format       clang-format each level, and src/ and test/
#
# LEVELS lists the levels that have a Makefile, in build order;
# sm3 joins it when it has one.
LEVELS := sm1 sm2

CC       ?= gcc
CXX      ?= g++
OPTFLAGS ?= -O1 -g
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -Wall -Wextra -Iinclude -Ism1/include -Ism2/include \
            -MMD -MP
# The test is C, held to the standard, so sm.h is proved pure C.
TEST_CFLAGS := $(OPTFLAGS) -std=c11 -pedantic -Wall -Wextra -Werror -Iinclude
LDLIBS   += -lrocksdb -licui18n -licuuc -licudata -lpthread

BUILD     := build
LIB       := $(BUILD)/libsm.a
LEVEL_LIBS := sm1/build/libwisr.a sm2/build/libsm2.a
TEST_BIN  := $(BUILD)/test_sm

.PHONY: all test clean format $(LEVELS)

all: $(LIB)

$(LEVELS):
	$(MAKE) -C $@

# sm2 links sm1's library, so sm1 builds first.
sm2: sm1

# The levels' archives are rebuilt by their own Makefiles; the
# levels are phony, so the bundle is redone after each build.
$(LEVEL_LIBS): $(LEVELS)

$(BUILD)/src/sm.o: src/sm.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# One archive of every level's objects and the wrapper's, by an
# ar script: ADDLIB keeps two levels' same-named objects apart.
$(LIB): $(BUILD)/src/sm.o $(LEVEL_LIBS)
	rm -f $@
	printf 'CREATE $@\nADDLIB sm1/build/libwisr.a\nADDLIB sm2/build/libsm2.a\nADDMOD $(BUILD)/src/sm.o\nSAVE\nEND\n' | ar -M

# Linked by the C++ driver, since the archive needs libstdc++.
$(TEST_BIN): test/test_sm.c include/sm.h include/sm_error.h \
             include/sm_options.h $(LIB)
	@mkdir -p $(BUILD)/test
	$(CC) $(TEST_CFLAGS) -c -o $(BUILD)/test/test_sm.o test/test_sm.c
	$(CXX) -o $@ $(BUILD)/test/test_sm.o $(LIB) $(LDLIBS)

# Each level's own target, in order, stopping at the first level
# that fails; then the wrapper's part.
test: $(TEST_BIN)
	@for d in $(LEVELS); do \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
	$(TEST_BIN)

clean:
	@for d in $(LEVELS); do \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
	rm -rf $(BUILD)

format:
	@for d in $(LEVELS); do \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
	clang-format -i src/sm.cc test/test_sm.c

-include $(BUILD)/src/sm.d
