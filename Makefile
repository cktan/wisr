# wisr -- build the storage manager library and its tests.
#
#   make -j4          libwisr.a, the test binaries, and the examples
#   make test         build, then run every test and every example
#   make dist         install libwisr.a and sm.h under PREFIX
#   make format       clang-format src/, test/, and example/ in place
#   make clean        remove build/
#
# Setting DEBUG in the environment builds -O1 -g instead of -O3:
#
#   DEBUG=1 make -j4
#
# The flags are recorded in build/flags, so changing DEBUG (or
# CXX, or CXXFLAGS) rebuilds everything rather than linking
# objects compiled with the old ones.
#
# include/sm.h is aligned by hand and is not formatted.

# DEBUG set and non-empty: optimize lightly and keep the symbols.
ifdef DEBUG
OPTFLAGS := -O1 -g
else
OPTFLAGS := -O3
endif

CXX      ?= g++
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -Wall -Wextra -Iinclude -Isrc -MMD -MP
LDLIBS   += -lrocksdb -lpthread

BUILD := build

# Where make dist installs the library and its header.
PREFIX ?= $(HOME)/.local

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
MVCC_SRCS := $(wildcard mvcc/*.cc)
MVCC_OBJS := $(MVCC_SRCS:mvcc/%.cc=$(BUILD)/mvcc/%.o)
EX_SRCS   := $(wildcard example/*.cc)
EX_OBJS   := $(EX_SRCS:example/%.cc=$(BUILD)/example/%.o)
EX_BINS   := $(EX_SRCS:example/%.cc=$(BUILD)/example/%)

LIB       := $(BUILD)/libwisr.a
TEST_BIN  := $(BUILD)/test_wisr
MVCC_BIN  := $(BUILD)/mvcc_tests

# Where an example puts its home directory when make runs it.
EX_HOME := $(BUILD)/example-home

FORMAT_FILES := $(wildcard src/*.cc src/*.h test/*.cc test/*.h mvcc/*.cc \
                          mvcc/*.h example/*.cc)
FORMAT_FILES := $(filter-out test/doctest.h,$(FORMAT_FILES))

.PHONY: all test dist format clean

# Keep the example objects: make deletes chained intermediates.
.SECONDARY: $(EX_OBJS)

all: $(LIB) $(TEST_BIN) $(MVCC_BIN) $(EX_BINS)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(LDLIBS)

$(MVCC_BIN): $(MVCC_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(MVCC_OBJS) $(LIB) $(LDLIBS)

# The scenario files take doctest.h from test/.
$(BUILD)/mvcc/%.o: mvcc/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Itest -Imvcc -c -o $@ $<

$(BUILD)/example/%: $(BUILD)/example/%.o $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIB) $(LDLIBS)

$(BUILD)/example/%.o: example/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/src/%.o: src/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/test/%.o: test/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Each example runs on a home directory of its own, made fresh
# so that a run never inherits an older format version.
test: all
	$(TEST_BIN)
	$(MVCC_BIN)
	@mkdir -p $(EX_HOME)
	@for ex in $(EX_BINS); do \
	  home=$(EX_HOME)/$$(basename $$ex); \
	  rm -rf $$home; \
	  echo "run $$ex"; \
	  $$ex $$home >/dev/null || exit 1; \
	done

# Install what is built now: a DEBUG build installs that one.
dist: $(LIB)
	@mkdir -p $(PREFIX)/lib $(PREFIX)/include
	install -m 644 $(LIB) $(PREFIX)/lib/
	install -m 644 include/sm.h $(PREFIX)/include/

format:
	clang-format -i $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(MVCC_OBJS:.o=.d) \
        $(EX_OBJS:.o=.d)
