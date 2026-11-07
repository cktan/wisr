# aa -- roles, authentication and authorization: what every database
# of a server shares about who may do what, kept in sm's database
# pg_global, which digres links as it links sm (README.md).
#
#   make -j4          libaa.a, and the test binary
#   make test         build, then run the tests
#   make tsan         the tests under the thread sanitizer
#   make asan         the tests under the address and UB sanitizers
#   make format       clang-format include/, src/ and test/ in place
#   make clean        remove build/
#
# Setting DEBUG=1 in the environment builds -O0 -g instead of
# -O3 -DNDEBUG, as sm's levels do. The flags are recorded in
# build/flags, so changing them rebuilds everything.
#
# BUILD names where the objects go; sm's library is taken from the
# same place in ../sm, built the same way: build/tsan/libaa.a links
# ../sm/build/tsan/libsm.a, as digres's tsan and release builds want.

ifeq ($(DEBUG),1)
OPTFLAGS := -O0 -g
else
OPTFLAGS := -O3 -DNDEBUG
endif

CXX      ?= g++
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -fno-rtti -Wall -Wextra -Iinclude -I../sm/include \
            -I../sm/sm0/include -Isrc -MMD -MP
LDLIBS   += -lrocksdb -licui18n -licuuc -licudata -lpthread

BUILD := build
SM    := ../sm
SM_LIB := $(SM)/$(BUILD)/libsm.a

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

# sm_begin() by its C++ symbol, which the tests wrap to fail a write
# (test_objects.cc).
WRAP_SM_BEGIN := _Z8sm_beginP12sm_session_t13sm_xctlevel_ti

LIB      := $(BUILD)/libaa.a
TEST_BIN := $(BUILD)/test_aa

FORMAT_FILES := $(wildcard src/*.cc src/*.h test/*.cc test/*.h include/*.h)

.PHONY: all test tsan asan format clean $(SM_LIB)

all: $(LIB) $(TEST_BIN)

# Rebuilt from scratch, so an object whose source is gone leaves it.
$(LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_OBJS) $(LIB) $(SM_LIB)
	$(CXX) $(CXXFLAGS) -Wl,--wrap=$(WRAP_SM_BEGIN) -o $@ $(TEST_OBJS) $(LIB) $(SM_LIB) $(LDLIBS)

# sm builds its own library; whether it is up to date is sm's to say.
ifeq ($(BUILD),build)
$(SM_LIB):
	$(MAKE) -C $(SM)
else
$(SM_LIB):
	$(MAKE) -C $(SM) BUILD=$(BUILD) OPTFLAGS='$(OPTFLAGS)' $(BUILD)/libsm.a
endif

$(BUILD)/src/%.o: src/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/test/%.o: test/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: all
	$(RUN) $(TEST_BIN)

# The tests again, built apart under a sanitizer, sm with them. The
# thread sanitizer cannot run with the address space randomised, hence
# setarch -R.
tsan:
	$(MAKE) BUILD=$(BUILD)/tsan OPTFLAGS="-O1 -g -fsanitize=thread" \
	    RUN="setarch $$(uname -m) -R" test

asan:
	$(MAKE) BUILD=$(BUILD)/asan \
	    OPTFLAGS="-O1 -g -fsanitize=address,undefined" test

format:
	clang-format -i $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)
