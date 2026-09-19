# wisr -- build the storage manager library and its tests.
#
#   make -j4          libwisr.a, the test binary, and the sample
#   make test         build, then run every test
#   make format       clang-format src/ and test/ in place
#   make clean        remove build/
#
# include/sm.h is aligned by hand and is not formatted.

CXX      ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Iinclude -Isrc -MMD -MP
LDLIBS   += -lrocksdb -lpthread

BUILD := build

LIB_SRCS  := $(wildcard src/*.cc)
LIB_OBJS  := $(LIB_SRCS:src/%.cc=$(BUILD)/src/%.o)
TEST_SRCS := $(wildcard test/*.cc)
TEST_OBJS := $(TEST_SRCS:test/%.cc=$(BUILD)/test/%.o)

LIB      := $(BUILD)/libwisr.a
TEST_BIN := $(BUILD)/test_wisr
SAMPLE   := $(BUILD)/sample

FORMAT_FILES := $(wildcard src/*.cc src/*.h test/*.cc test/*.h) sample.cc
FORMAT_FILES := $(filter-out test/doctest.h,$(FORMAT_FILES))

.PHONY: all test format clean

all: $(LIB) $(TEST_BIN) $(SAMPLE)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(TEST_OBJS) $(LIB) $(LDLIBS)

$(SAMPLE): $(BUILD)/sample.o $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $(BUILD)/sample.o $(LIB) $(LDLIBS)

$(BUILD)/sample.o: sample.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/src/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/test/%.o: test/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: all
	$(TEST_BIN)

format:
	clang-format -i $(FORMAT_FILES)

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(BUILD)/sample.d
