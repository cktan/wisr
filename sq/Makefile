# sq -- the SQL front end: text in, parse tree out.
#
#   make -j4         build build/libsq.a, its tests, and build/oracle
#   make test        build, and run the tests in test/
#   make corpus      split PG_SQL's files into build/corpus
#   make corpus-test compare with the oracle over all of PG_SQL
#   make tsan        the unit tests under the thread sanitizer
#   make clean       remove build/
#   make format      clang-format src/ and test/
#   make nodes       rewrite include/sq_nodes.h from spec/
#
# Two things are built here (PLAN.md).
#
# The library, build/libsq.a, from src/*.cc and include/. It uses
# nothing of digres's and nothing of sm's. include/sq.h and sq_error.h are aligned by
# hand and are not formatted.
#
# The oracle, build/oracle, from src/*.c and PostgreSQL's own parser,
# whose sources are still in ../dg/src and are compiled from there
# into build/dg. It is what the library is checked against, and goes
# when the library can do without it.

# ---- the library ----

CXX ?= g++
OPT ?= -O0 -g
LIB_CXXFLAGS := -std=c++20 -Wall -Wextra $(OPT) -Iinclude -Isrc -Ibuild/gen \
                -Ispec -MMD -MP
# What flex and bison write is not held to -Wall.
GEN_CXXFLAGS := $(filter-out -Wall -Wextra,$(LIB_CXXFLAGS)) -w

LIB_SRCS  := $(wildcard src/*.cc)
LIB_OBJS  := $(LIB_SRCS:src/%.cc=build/src/%.o) build/gen/print_nodes.o \
             build/gen/node_funcs.o build/gen/scan.o build/gen/gram.o
TOOL_SRCS := $(wildcard test/tool/*.cc)
TOOLS     := $(TOOL_SRCS:test/tool/%.cc=build/%)
TEST_SRCS := $(wildcard test/*.cc)
TEST_OBJS := $(TEST_SRCS:test/%.cc=build/test/%.o)
LIB       := build/libsq.a
TEST_BIN  := build/test_sq

# Rebuilt from scratch, so an object whose source is gone leaves it.
$(LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# Linked with the address sanitizer's runtime for its leak checker: a
# node or a string that a tree fails to free fails the tests.
$(TEST_BIN): $(TEST_OBJS) $(LIB)
	$(CXX) $(LIB_CXXFLAGS) -fsanitize=address -o $@ $(TEST_OBJS) $(LIB) -lpthread

# The nodes are PostgreSQL's, generated from its headers in spec/.
# include/sq_nodes.h is committed, so that the API can be read: `make
# nodes` rewrites it, and test/nodes.sh fails when it is out of date.
# The printer over the nodes is built and not committed.
GEN_NODES := gen/gen_nodes.py gen/nodespec.py $(wildcard spec/*)

nodes:
	python3 gen/gen_nodes.py header > include/sq_nodes.h

build/gen/print_nodes.cc: $(GEN_NODES)
	@mkdir -p $(dir $@)
	python3 gen/gen_nodes.py printer > $@

build/gen/node_funcs.cc: $(GEN_NODES)
	@mkdir -p $(dir $@)
	python3 gen/gen_nodes.py funcs > $@

# The perfect hash the scanner finds a keyword with.
build/gen/keyword_hash.h: gen/gen_keywords.py spec/kwlist.h
	@mkdir -p $(dir $@)
	python3 gen/gen_keywords.py > $@

build/src/keywords.o: build/gen/keyword_hash.h

# The grammar. Its header has the tokens the scanner returns. bison
# writes both files at once; the header is the one make follows.
build/gen/gram.hh: src/gram.y
	@mkdir -p $(dir $@)
	bison -Wno-deprecated -d -o build/gen/gram.cc $<

build/gen/gram.cc: build/gen/gram.hh ;

build/gen/gram.o: build/gen/gram.cc
	$(CXX) $(GEN_CXXFLAGS) -c -o $@ $<

# The flags are PostgreSQL's for its scanner: a full table, for speed.
build/gen/scan.cc: src/scan.l
	@mkdir -p $(dir $@)
	flex -CF -p -p -o $@ $<

build/gen/print_nodes.o: build/gen/print_nodes.cc
	$(CXX) $(LIB_CXXFLAGS) -c -o $@ $<

build/gen/node_funcs.o: build/gen/node_funcs.cc | build/gen/gram.hh
	$(CXX) $(LIB_CXXFLAGS) -c -o $@ $<

build/gen/scan.o: build/gen/scan.cc build/gen/gram.hh
	$(CXX) $(GEN_CXXFLAGS) -c -o $@ $<

build/src/%.o: src/%.cc | build/gen/gram.hh
	@mkdir -p $(dir $@)
	$(CXX) $(LIB_CXXFLAGS) -c -o $@ $<

build/test/%.o: test/%.cc | build/gen/gram.hh
	@mkdir -p $(dir $@)
	$(CXX) $(LIB_CXXFLAGS) -c -o $@ $<

# The tests again, with the library built into them under the thread
# sanitizer: slow to build, so it is a target of its own. What it is
# for is the test that parses on eight threads at once.
build/test_sq_tsan: $(LIB_SRCS) $(TEST_SRCS) $(LIB) | build/gen/gram.hh
	$(CXX) $(GEN_CXXFLAGS) -MF build/test_sq_tsan.d -O1 -fsanitize=thread \
	    -o $@ $(LIB_SRCS) $(TEST_SRCS) build/gen/print_nodes.cc \
	    build/gen/node_funcs.cc build/gen/scan.cc build/gen/gram.cc -lpthread

# Run with address randomisation off: this compiler's sanitizer cannot
# place its shadow memory under the kernel's randomisation, and stops
# with "unexpected memory mapping" before a test has run.
tsan: build/test_sq_tsan
	setarch $$(uname -m) -R build/test_sq_tsan

# Programs the shell tests run, each one source file over the library.
build/%: test/tool/%.cc $(LIB) | build/gen/gram.hh
	$(CXX) $(LIB_CXXFLAGS) -o $@ $< $(LIB)

# ---- the oracle ----

DG  := ../dg
SRC := $(DG)/src
B   := $(SRC)/backend

# One section a function, so that the link keeps only what the
# parser reaches, and reports only what that needs.
CXXFLAGS = -x c++ -std=gnu++20 -w $(OPT) -ffunction-sections \
            -fdata-sections -I$(SRC)/include -I$(B)/parser -D_GNU_SOURCE \
            -MMD -MP
LDFLAGS  := -Wl,--gc-sections
LDLIBS   := -licui18n -licuuc -licudata -lm -lpthread

# What belongs to sq. Processor-specific files are chosen as
# ../dg/src/port/Makefile chooses them.
ifeq ($(shell uname -m),aarch64)
PORT_SKIP := pg_crc32c_sse42.c pg_crc32c_sse42_choose.c pg_popcount_avx512.c
else
PORT_SKIP := pg_crc32c_armv8.c pg_crc32c_armv8_choose.c pg_popcount_aarch64.c
endif
COMMON_SKIP := cryptohash_openssl.c hmac_openssl.c

DG_SRCS := \
    $(filter-out $(addprefix $(SRC)/port/,$(PORT_SKIP)),$(wildcard $(SRC)/port/*.c)) \
    $(filter-out $(addprefix $(SRC)/common/,$(COMMON_SKIP)),$(wildcard $(SRC)/common/*.c)) \
    $(filter-out %.funcs.c %.switch.c,$(wildcard $(B)/nodes/*.c)) \
    $(wildcard $(B)/utils/mmgr/*.c) $(wildcard $(B)/utils/error/*.c) \
    $(wildcard $(B)/utils/mb/*.c) $(wildcard $(B)/utils/hash/*.c) \
    $(wildcard $(B)/lib/*.c) \
    $(addprefix $(B)/parser/,gram.c scan.c parser.c scansup.c) \
    $(B)/utils/init/globals.c $(B)/utils/adt/datum.c \
    $(B)/utils/adt/expandeddatum.c \
    $(B)/utils/adt/numutils.c $(B)/utils/misc/stack_depth.c \
    $(B)/commands/define.c $(B)/storage/lmgr/s_lock.c \
    $(B)/libpq/pqformat.c $(SRC)/timezone/localtime.c \
    $(SRC)/timezone/strftime.c

DG_OBJS := $(patsubst $(SRC)/%.c,build/dg/%.o,$(DG_SRCS))
OBJS    := build/oracle.o build/host.o $(DG_OBJS)

.PHONY: all test corpus corpus-test tsan clean format gen nodes
all: $(LIB) $(TEST_BIN) $(TOOLS) build/oracle

# The generated sources and headers are digres's to make.
gen:
	$(MAKE) -C $(DG) gen

build/dg/%.o: $(SRC)/%.c | gen
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/dg/port/pg_crc32c_armv8.o: OPT += -march=armv8-a+crc
build/dg/port/pg_crc32c_sse42.o: OPT += -msse4.2

build/%.o: src/%.c | gen
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/oracle: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

test: all
	$(TEST_BIN)
	test/alone.sh
	test/nodes.sh
	test/spec.sh
	test/tokens.sh test
	test/tokens.sh test/scan
	test/trees.sh test
	test/trees.sh test/scan
	test/trees.sh test/errors
	test/trees.sh test/types --type-name
	test/run.sh
	test/split.sh

# PostgreSQL's regression SQL, one statement a file with its tree.
PG_SQL ?= $(HOME)/p/postgres18/src/test/regress/sql
corpus: build/oracle
	test/corpus.sh $(PG_SQL)

# The checks that run over all of it, and are too large for `make test`:
# the scanner's tokens in every file, the parser's tree for every
# statement that can be split off, and what the parser says of every
# piece of every file, the wrong ones among them.
corpus-test: all corpus
	test/tokens.sh $(PG_SQL)
	test/trees.sh --corpus
	rm -rf build/chunks
	test/chunks.py $(PG_SQL) build/chunks
	test/trees.sh --chunks

clean:
	rm -rf build

format:
	clang-format -i src/*.c src/*.cc src/*.h test/*.cc test/tool/*.cc

-include $(OBJS:.o=.d) $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(TOOLS:=.d)
