# wisr -- build the storage manager library and its tests.
#
#   make -j4          libsm1.a, the test binaries, and the examples
#   make test         build, then run every test, script, and example
#   make accept       take the scenario transcripts as expected
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
# include/sm1.h is aligned by hand and is not formatted.

# DEBUG set and non-empty: optimize lightly and keep the symbols.
ifdef DEBUG
OPTFLAGS := -O1 -g
else
OPTFLAGS := -O3
endif

CXX      ?= g++
CXXFLAGS ?= $(OPTFLAGS)
BUILD    := build
# sm0, level 0: the error state, the log hook, the helpers, and
# the system services (sm_sys.h).
SM0      := ../sm0
SM0_LIB  := $(SM0)/$(BUILD)/libsm0.a

CXXFLAGS += -std=c++20 -fno-rtti -Wall -Wextra -Iinclude -I../include \
            -I$(SM0)/include -Isrc -MMD -MP
LDLIBS   += $(SM0_LIB) -lrocksdb -lpthread

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
DRV_SRCS  := $(wildcard mvcc/driver/*.cc)
DRV_OBJS  := $(DRV_SRCS:mvcc/driver/%.cc=$(BUILD)/drvobj/%.o)
EX_SRCS   := $(wildcard example/*.cc)
EX_OBJS   := $(EX_SRCS:example/%.cc=$(BUILD)/example/%.o)
EX_BINS   := $(EX_SRCS:example/%.cc=$(BUILD)/example/%)

LIB       := $(BUILD)/libsm1.a
TEST_BIN  := $(BUILD)/test_sm1
MVCC_BIN  := $(BUILD)/mvcc_tests
DRIVER    := $(BUILD)/driver

# Where an example puts its home directory when make runs it.
EX_HOME := $(BUILD)/example-home

# The scenario scripts, their transcripts, and what is expected.
SCRIPTS     := $(wildcard mvcc/in/*.t)
SCRIPT_OUT  := mvcc/out
SCRIPT_GOOD := mvcc/good

FORMAT_FILES := $(wildcard src/*.cc src/*.h test/*.cc test/*.h mvcc/*.cc \
                          mvcc/*.h mvcc/driver/*.cc mvcc/driver/*.h \
                          example/*.cc include/sm1_*.h)

.PHONY: all test scenarios accept format clean $(SM0_LIB)

# Keep the example objects: make deletes chained intermediates.
.SECONDARY: $(EX_OBJS)

all: $(LIB) $(TEST_BIN) $(MVCC_BIN) $(DRIVER) $(EX_BINS)

# Rebuilt from scratch, so an object whose source is gone leaves it.
$(LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# sm0's Makefile decides whether its library is up to date.
$(SM0_LIB):
	$(MAKE) -C $(SM0) $(BUILD)/libsm0.a

$(TEST_BIN): $(TEST_OBJS) $(LIB) $(SM0_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(LDLIBS)

$(MVCC_BIN): $(MVCC_OBJS) $(LIB) $(SM0_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(MVCC_OBJS) $(LIB) $(LDLIBS)

$(DRIVER): $(DRV_OBJS) $(LIB) $(SM0_LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(DRV_OBJS) $(LIB) $(LDLIBS)

$(BUILD)/drvobj/%.o: mvcc/driver/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/mvcc/%.o: mvcc/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Imvcc -c -o $@ $<

$(BUILD)/example/%: $(BUILD)/example/%.o $(LIB) $(SM0_LIB)
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
test: all scenarios
	$(TEST_BIN)
	$(MVCC_BIN)
	@mkdir -p $(EX_HOME)
	@for ex in $(EX_BINS); do \
	  home=$(EX_HOME)/$$(basename $$ex); \
	  rm -rf $$home; \
	  echo "run $$ex"; \
	  $$ex $$home >/dev/null || exit 1; \
	done

# Run every scenario script and diff its transcript against the
# expected one. A difference is a failure: mvcc/good is the
# claim, mvcc/out is what the engine did.
scenarios: $(DRIVER)
	@mkdir -p $(SCRIPT_OUT)
	@fail=0; \
	for f in $(SCRIPTS); do \
	  b=$$(basename $$f .t); \
	  $(DRIVER) $$f > $(SCRIPT_OUT)/$$b.out || fail=1; \
	  if ! diff -u $(SCRIPT_GOOD)/$$b.out $(SCRIPT_OUT)/$$b.out; then \
	    fail=1; \
	  fi; \
	done; \
	if [ $$fail -ne 0 ]; then \
	  echo "scenario transcripts differ; see the diffs above"; \
	  exit 1; \
	fi; \
	echo "scenarios: $(words $(SCRIPTS)) scripts match"

# Take the current transcripts as expected. Never run by
# "make test": accepting a transcript is a deliberate act.
accept: $(DRIVER)
	@mkdir -p $(SCRIPT_OUT) $(SCRIPT_GOOD)
	@for f in $(SCRIPTS); do \
	  b=$$(basename $$f .t); \
	  $(DRIVER) $$f > $(SCRIPT_OUT)/$$b.out; \
	  cp $(SCRIPT_OUT)/$$b.out $(SCRIPT_GOOD)/$$b.out; \
	done
	@echo "accepted $(words $(SCRIPTS)) transcripts into $(SCRIPT_GOOD)"

format:
	clang-format -i $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(MVCC_OBJS:.o=.d) \
        $(DRV_OBJS:.o=.d) $(EX_OBJS:.o=.d)
