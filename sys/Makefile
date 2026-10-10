# sys -- system services for sm and dg (README.md).
#
#   make -j4          build/libsys.a, and the test binary
#   make test         build, then run the tests
#   make tsan         the tests under the thread sanitizer
#   make asan         the tests under the address and UB sanitizers
#   make format       clang-format src/ and test/ in place
#   make clean        remove build/
#
# Setting DEBUG in the environment builds -O1 -g instead of -O3:
#
#   DEBUG=1 make -j4
#
# The flags are recorded in build/flags, so changing DEBUG (or
# CXX, or CXXFLAGS) rebuilds everything rather than linking
# objects compiled with the old ones.

# DEBUG set and non-empty: optimize lightly and keep the symbols.
ifdef DEBUG
OPTFLAGS := -O1 -g
else
OPTFLAGS := -O3
endif

CXX      ?= g++
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -Wall -Wextra -Iinclude -Isrc -MMD -MP
LDLIBS   += -lpthread

BUILD := build

# The compiler and flags every object is built with. Rewriting
# the file when they change makes it newer than the objects, so
# each one is rebuilt below.
FLAGS      := $(CXX) $(CXXFLAGS) $(LDLIBS)
FLAGS_FILE := $(BUILD)/flags
$(shell mkdir -p $(BUILD); \
        [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = '$(FLAGS)' ] || \
        echo '$(FLAGS)' > $(FLAGS_FILE))

LIB_SRCS  := $(wildcard src/*.cc)
LIB_OBJS  := $(LIB_SRCS:src/%.cc=$(BUILD)/src/%.o)
TEST_SRCS := $(wildcard test/*.cc)
TEST_OBJS := $(TEST_SRCS:test/%.cc=$(BUILD)/test/%.o)

LIB      := $(BUILD)/libsys.a
TEST_BIN := $(BUILD)/test_sys

FORMAT_FILES := $(wildcard src/*.cc src/*.h test/test_*.cc)

.PHONY: all test tsan asan format clean

all: $(LIB) $(TEST_BIN)

# Rebuilt from scratch, so an object whose source is gone leaves it.
$(LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(LDLIBS)

$(BUILD)/src/%.o: src/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/test/%.o: test/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: all
	$(RUN) $(TEST_BIN)

# The tests again, built apart under a sanitizer. The thread
# sanitizer cannot run with the address space randomised, hence
# setarch -R.
tsan:
	$(MAKE) BUILD=$(BUILD)/tsan OPTFLAGS="-O1 -g -fsanitize=thread" \
	    RUN="setarch $$(uname -m) -R" test

asan:
	$(MAKE) BUILD=$(BUILD)/asan \
	    OPTFLAGS="-O1 -g -fsanitize=address,undefined" test

format:
	$(if $(FORMAT_FILES),clang-format -i $(FORMAT_FILES))

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
