# rel/Makefile -- build the rel test driver.

CXX      ?= g++
OPTFLAGS ?= -O1 -g
CXXFLAGS ?= $(OPTFLAGS)
CXXFLAGS += -std=c++20 -fno-rtti -Wall -Wextra -I../include -I/usr/local/include -Isrc -MMD -MP
LDLIBS   += -L/usr/local/lib -ljanet -lrocksdb -licui18n -licuuc -licudata -lpthread -ldl -lm

BUILD    ?= ../build
OBJDIR   := $(BUILD)/rel_obj
BIN      := $(BUILD)/rel
SRCS     := src/main.cc src/rel_catalog.cc src/rel_types.cc src/rel_ops.cc
OBJS     := $(SRCS:src/%.cc=$(OBJDIR)/%.o)
DEPS     := $(OBJS:.o=.d)

.PHONY: all clean test format

all: $(BIN)

$(OBJDIR)/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BIN): $(OBJS) $(BUILD)/libsm.a
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $(OBJS) $(BUILD)/libsm.a $(LDLIBS)

test: $(BIN)
	@mkdir -p test
	@rm -rf /tmp/wisr_rel_test_*
	@for t in test/*.rel; do \
		if [ -f "$$t" ]; then \
			echo "Running $$t..."; \
			$(BIN) "$$t" || exit 1; \
		fi \
	done
	@rm -rf /tmp/wisr_rel_test_*
	@echo "All rel tests passed."

format:
	clang-format -i src/*.cc src/*.h

clean:
	rm -rf $(OBJDIR) $(BIN)

-include $(DEPS)
