# sm2 -- build the type and tuple library and its tests.
#
#   make -j4          libsm2.a and the test binary
#   make test         build, then run the tests
#   make tsan         the B+tree's tests under the thread sanitizer
#   make format       clang-format src/ and test/ in place
#   make clean        remove build/
#
# sm2 builds against sm1's headers and links sm1's library: sm1's
# error state reports sm2's failures too (SM2_DESIGN.md §5). The
# library is built through sm1's own Makefile when it is missing
# or out of date.
#
# Setting DEBUG=1 in the environment builds -O0 -g instead of
# -O3 -DNDEBUG,
# and the flags are recorded in build/flags, as in sm1.
#
# include/sm2.h is aligned by hand and is not formatted, nor is
# src/ryu/, PostgreSQL's float printers copied verbatim and built
# as C (src/ryu/postgres_fe.h).

# DEBUG=1: no optimization, the symbols, and assert(); otherwise -O3
# with NDEBUG, so assert() is compiled out.
ifeq ($(DEBUG),1)
OPTFLAGS := -O0 -g
else
OPTFLAGS := -O3 -DNDEBUG
endif

SM0 := ../sm0
SM1 := ../sm1

CC       ?= gcc
CFLAGS   ?= $(OPTFLAGS)
CFLAGS   += -std=c11 -Wall -DFRONTEND -Isrc/ryu -MMD -MP
CXX      ?= g++
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -fno-rtti -Wall -Wextra -Iinclude -Isrc/type -Isrc/ryu \
            -I$(SM1)/include -I$(SM0)/include -I../include \
            -MMD -MP
# RocksDB: the system's, or with make tsan the instrumented one
# ../dg/test/rocksdb.sh builds.
ROCKSDB  ?= -lrocksdb
LDLIBS   += $(ROCKSDB) -licui18n -licuuc -licudata -lpthread

BUILD := build

# The compiler and flags every object is built with. Rewriting
# the file when they change makes it newer than the objects, so
# each one is rebuilt below.
FLAGS      := $(CC) $(CFLAGS) $(CXX) $(CXXFLAGS) $(LDLIBS)
FLAGS_FILE := $(BUILD)/flags
$(shell mkdir -p $(BUILD); \
        [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = '$(FLAGS)' ] || \
        echo '$(FLAGS)' > $(FLAGS_FILE))

LIB_SRCS  := $(wildcard src/*/*.cc)
LIB_OBJS  := $(LIB_SRCS:src/%.cc=$(BUILD)/src/%.o)
RYU_SRCS  := $(wildcard src/ryu/*.c)
LIB_OBJS  += $(RYU_SRCS:src/%.c=$(BUILD)/src/%.o)
TEST_SRCS := $(wildcard test/*.cc)
TEST_OBJS := $(TEST_SRCS:test/%.cc=$(BUILD)/test/%.o)

LIB      := $(BUILD)/libsm2.a
SM0_LIB  := $(SM0)/$(BUILD)/libsm0.a
SM1_LIB  := $(SM1)/$(BUILD)/libsm1.a
TEST_BIN := $(BUILD)/test_sm2

# Everything but the generated fixture header and src/ryu/.
FORMAT_FILES := $(filter-out test/pg_fixtures.h src/ryu/%, \
    $(wildcard src/*/*.cc src/*/*.h test/*.cc test/*.h))

.PHONY: all test tsan format clean $(SM0_LIB) $(SM1_LIB)

all: $(LIB) $(TEST_BIN)

# Rebuilt from scratch, so an object whose source is gone leaves it.
$(LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# sm0's Makefile decides whether its library is up to date.
$(SM0_LIB):
	$(MAKE) -C $(SM0) BUILD=$(BUILD) OPTFLAGS='$(OPTFLAGS)' $(BUILD)/libsm0.a

# sm1's Makefile decides whether its library is up to date.
$(SM1_LIB):
	$(MAKE) -C $(SM1) BUILD=$(BUILD) OPTFLAGS='$(OPTFLAGS)' $(BUILD)/libsm1.a

$(TEST_BIN): $(TEST_OBJS) $(LIB) $(SM1_LIB) $(SM0_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(SM1_LIB) $(SM0_LIB) \
	    $(LDLIBS)

# The flags it is built with, which build_flags() reports.
$(BUILD)/src/tuple/tuple.o: CXXFLAGS += -D'SM2_BUILD_FLAGS="$(OPTFLAGS)"'

$(BUILD)/src/%.o: src/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/src/%.o: src/%.c $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/test/%.o: test/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: all
	$(RUN) $(TEST_BIN) $(TEST_ARGS)

# The B+tree's tests, whose threads share pages, latches and the
# block cache, built apart under the thread sanitizer, against the
# RocksDB ../../dg/test/rocksdb.sh builds instrumented: the system's
# is not, and its handovers would read as races. The sanitizer cannot
# run with the address space randomised, hence setarch -R.
TSAN_ROCKSDB := $(abspath ../../rocksdb/tsan/librocksdb.a) \
    -lsnappy -llz4 -lzstd -lz -lbz2
tsan:
	../../dg/test/rocksdb.sh tsan
	$(MAKE) BUILD=$(BUILD)/tsan OPTFLAGS="-O1 -g -fsanitize=thread" \
	    ROCKSDB="$(TSAN_ROCKSDB)" RUN="setarch $$(uname -m) -R" \
	    TEST_ARGS='-tc="tree*"' test

format:
	clang-format -i $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
