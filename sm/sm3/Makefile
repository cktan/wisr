# sm3 -- build the schema and index maintenance library and its
# tests.
#
#   make -j4          libsm3.a and the test binary
#   make test         build, then run the tests
#   make format       clang-format src/ and test/ in place
#   make clean        remove build/
#
# sm3 builds against sm2's and sm1's headers and links their
# libraries, each built through its own Makefile when it is
# missing or out of date.
#
# Setting DEBUG=1 in the environment builds -O0 -g instead of
# -O3 -DNDEBUG,
# and the flags are recorded in build/flags, as in sm1 and sm2.

# DEBUG=1: no optimization, the symbols, and assert(); otherwise -O3
# with NDEBUG, so assert() is compiled out.
ifeq ($(DEBUG),1)
OPTFLAGS := -O0 -g
else
OPTFLAGS := -O3 -DNDEBUG
endif

SM0 := ../sm0
SM1 := ../sm1
SM2 := ../sm2

CXX      ?= g++
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -fno-rtti -Wall -Wextra -Iinclude -Isrc/catalog \
            -Isrc/analyze -I$(SM2)/include -I$(SM1)/include \
            -I$(SM0)/include -I../include -MMD -MP
LDLIBS   += -lrocksdb -licui18n -licuuc -licudata -lpthread

BUILD := build

# The compiler and flags every object is built with. Rewriting
# the file when they change makes it newer than the objects, so
# each one is rebuilt below.
FLAGS      := $(CXX) $(CXXFLAGS) $(LDLIBS)
FLAGS_FILE := $(BUILD)/flags
$(shell mkdir -p $(BUILD); \
        [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = '$(FLAGS)' ] || \
        echo '$(FLAGS)' > $(FLAGS_FILE))

LIB_SRCS  := $(wildcard src/*/*.cc)
LIB_OBJS  := $(LIB_SRCS:src/%.cc=$(BUILD)/src/%.o)
TEST_SRCS := $(wildcard test/*.cc)
TEST_OBJS := $(TEST_SRCS:test/%.cc=$(BUILD)/test/%.o)

LIB      := $(BUILD)/libsm3.a
SM0_LIB  := $(SM0)/build/libsm0.a
SM1_LIB  := $(SM1)/build/libsm1.a
SM2_LIB  := $(SM2)/build/libsm2.a
TEST_BIN := $(BUILD)/test_sm3

FORMAT_FILES := $(wildcard src/*/*.cc src/*/*.h test/*.cc test/*.h)

.PHONY: all test format clean $(SM0_LIB) $(SM1_LIB) $(SM2_LIB)

all: $(LIB) $(TEST_BIN)

# Rebuilt from scratch, so an object whose source is gone leaves it.
$(LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# The lower levels' Makefiles decide whether their libraries are
# up to date.
$(SM0_LIB):
	$(MAKE) -C $(SM0) build/libsm0.a

$(SM1_LIB):
	$(MAKE) -C $(SM1) build/libsm1.a

$(SM2_LIB):
	$(MAKE) -C $(SM2) build/libsm2.a

$(TEST_BIN): $(TEST_OBJS) $(LIB) $(SM2_LIB) $(SM1_LIB) $(SM0_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(SM2_LIB) $(SM1_LIB) \
	    $(SM0_LIB) $(LDLIBS)

# The flags it is built with, which build_flags() reports.
$(BUILD)/src/table/table.o: CXXFLAGS += -D'SM3_BUILD_FLAGS="$(OPTFLAGS)"'

$(BUILD)/src/%.o: src/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/test/%.o: test/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: all
	$(TEST_BIN)

format:
	clang-format -i $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
