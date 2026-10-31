# rel/Makefile -- build the rel test driver.

CXX      ?= g++
# As the levels': DEBUG=1, -O0 -g; otherwise -O3 -DNDEBUG.
ifeq ($(DEBUG),1)
OPTFLAGS ?= -O0 -g
else
OPTFLAGS ?= -O3 -DNDEBUG
endif
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

# The compiler and flags, recorded as the levels record theirs, so a
# change of DEBUG rebuilds the driver.
FLAGS      := $(CXX) $(CXXFLAGS)
FLAGS_FILE := $(OBJDIR)/flags
$(shell mkdir -p $(OBJDIR); \
        [ "$$(cat $(FLAGS_FILE) 2>/dev/null)" = '$(FLAGS)' ] || \
        echo '$(FLAGS)' > $(FLAGS_FILE))

$(OBJDIR)/%.o: src/%.cc $(FLAGS_FILE)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BIN): $(OBJS) $(BUILD)/libsm.a
	@mkdir -p $(dir $@)
	$(CXX) -o $@ $(OBJS) $(BUILD)/libsm.a $(LDLIBS)

# Each run's databases are in a directory of its own, REL_TMP, which
# the tests make theirs under: runs side by side, by one user or two,
# do not meet. It is removed after a run that passes, kept after one
# that fails, for a look.
test: $(BIN)
	@mkdir -p test
	@REL_TMP=$$(mktemp -d "$${TMPDIR:-/tmp}/wisr_rel_test.XXXXXX") && \
	export REL_TMP && \
	for t in test/*.rel; do \
		if [ -f "$$t" ]; then \
			echo "Running $$t..."; \
			$(BIN) "$$t" || { echo "kept $$REL_TMP"; exit 1; }; \
		fi \
	done && rm -rf "$$REL_TMP"
	@echo "All rel tests passed."

format:
	clang-format -i src/*.cc src/*.h

clean:
	rm -rf $(OBJDIR) $(BIN)

-include $(DEPS)
