# sq -- the SQL parser: PostgreSQL 18's SQL in, a tree of nodes out.
#
#   make -j4         build build/libsq.a, its tests, and their tools
#   make test        build, and run the tests in test/
#   make corpus-test check sq over PostgreSQL's regression SQL, PG_SQL
#   make tsan        the unit tests under the thread sanitizer
#   make clean       remove build/
#   make format      clang-format src/ and test/
#   make nodes       rewrite include/sq_nodes.h from spec/
#
# The library is build/libsq.a, from src/ and include/. It is built
# from nothing outside this directory. include/sq.h and sq_error.h are
# aligned by hand and are not formatted; include/sq_nodes.h is
# generated, as are the files in build/gen.

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

.PHONY: all test corpus-test tsan clean format nodes
all: $(LIB) $(TEST_BIN) $(TOOLS)

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
# The functions over the nodes are built and not committed.
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

build/src/keywords.o build/rel/src/keywords.o: build/gen/keyword_hash.h

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

# The library again, optimized, its objects apart in build/rel: what
# digres's release build links (../bench/DESIGN.md section 2).
REL_OBJS := $(LIB_OBJS:build/%=build/rel/%)
REL_OPT := -O2 -DNDEBUG
REL_LIB_CXXFLAGS := $(filter-out -O0 -g,$(LIB_CXXFLAGS)) $(REL_OPT)
REL_GEN_CXXFLAGS := $(filter-out -O0 -g,$(GEN_CXXFLAGS)) $(REL_OPT)

# The flags each is built with, which sq_build_flags() reports.
build/src/error.o: LIB_CXXFLAGS += -D'SQ_BUILD_FLAGS="$(OPT)"'
build/rel/src/error.o: REL_LIB_CXXFLAGS += -D'SQ_BUILD_FLAGS="$(REL_OPT)"'

build/libsq-release.a: $(REL_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

build/rel/src/%.o: src/%.cc | build/gen/gram.hh
	@mkdir -p $(dir $@)
	$(CXX) $(REL_LIB_CXXFLAGS) -c -o $@ $<

build/rel/gen/print_nodes.o build/rel/gen/node_funcs.o: \
        build/rel/gen/%.o: build/gen/%.cc | build/gen/gram.hh
	@mkdir -p $(dir $@)
	$(CXX) $(REL_LIB_CXXFLAGS) -c -o $@ $<

build/rel/gen/scan.o build/rel/gen/gram.o: \
        build/rel/gen/%.o: build/gen/%.cc | build/gen/gram.hh
	@mkdir -p $(dir $@)
	$(CXX) $(REL_GEN_CXXFLAGS) -c -o $@ $<

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

test: all
	$(TEST_BIN)
	test/alone.sh
	test/nodes.sh
	test/run.sh

# sq over PostgreSQL 18.6's regression SQL, against the digests of what
# PostgreSQL's own scanner and parser made of it. It needs the scripts,
# which are not here, and takes minutes: so it is not part of `make
# test`.
PG_SQL ?= $(HOME)/p/postgres18/src/test/regress/sql
corpus-test: all
	test/corpus.py $(PG_SQL)

clean:
	rm -rf build

format:
	clang-format -i src/*.cc src/*.h test/*.cc test/tool/*.cc

-include $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(TOOLS:=.d) $(REL_OBJS:.o=.d)
