# dg -- build digres.
#
#   make -j4           generate what is generated, compile, and link
#                      the server, build/digres, which is initdb too
#   make -k -j4        the same, going on past a file that fails
#   make gen           only generate
#   make test          check the parser, with sq's tests given this
#                      server, run the statements in test/, and check
#                      initdb and a server that psql connects to, run
#                      the sqllogictest files in test/slt but select5,
#                      and PostgreSQL 18's regression files as ported
#   make slt           run every sqllogictest file in test/slt
#   make slt-corpus    run a sample of SQLite's whole corpus, fetched once
#   make isolation     run PostgreSQL 18's isolation specs on digres
#   make regress       run PostgreSQL 18's regression suite on digres, and
#                      report what stops each file (test/regress/README.md)
#   make pgregress     run PostgreSQL 18's regression files as ported to
#                      digres, against expected output (test/pgregress)
#   make tsan          build build/digres-tsan, under the thread
#                      sanitizer, and run the statements in test/ with it
#   make clean         remove build/ and the generated files
#   make format        clang-format every .c and .h
#
# This Makefile builds the tree alone, as each of sm's levels does
# (../sm/sm3/Makefile): every object lies under build/, in a tree
# mirroring src/, and the link takes the list the sources give. So a
# source that goes away takes its object with it, and "make clean" is
# "rm -rf build" and the generated files.

I := src/include
B := src/backend
PERL := perl
# RocksDB, which sm links: the system's, or, for the thread sanitizer, one
# built for it with its compression libraries (test/rocksdb.sh).
LINK_ROCKSDB = -lrocksdb
ROCKSDB_TSAN := ../rocksdb/tsan/librocksdb.a
ROCKSDB_REL := ../rocksdb/release/librocksdb.a
LDLIBS = $(LINK_ROCKSDB) -licui18n -licuuc -licudata -lm -lpthread -ldl -lrt
# The parser, a library of its own (../sq).
SQ := ../sq
LIBSQ := $(SQ)/build/libsq.a
LIBSQ_REL := $(SQ)/build/libsq-release.a
# The storage stack, a library of its own too (../sm). Its level 0,
# sm0, holds the system services digres builds on (sm_sys.h), so this
# one library brings them.
SM := ../sm
LIBSM := $(SM)/build/libsm.a
# sm instrumented too: sm_sys.h's atomics hand off what digres's threads
# share, and a sanitizer that cannot see them reports races that are
# not.
LIBSM_TSAN := $(SM)/build/tsan/libsm.a
LIBSM_REL := $(SM)/build/release/libsm.a
# The sm library a link takes: the tsan build overrides it.
LINK_SM = $(LIBSM)
# Roles and privileges, a library of their own too (../aa), on sm; built
# the same three ways.
AA := ../aa
LIBAA := $(AA)/build/libaa.a
LIBAA_TSAN := $(AA)/build/tsan/libaa.a
LIBAA_REL := $(AA)/build/release/libaa.a
LINK_AA = $(LIBAA)
# Where digres is installed, which the server is built knowing.
PREFIX ?= /usr/local/dg

# FLAVOR is the build: the default one, debug; tsan, under the thread
# sanitizer; or rel, optimized and without assertions, which the
# benchmark runs. Each compiles to its own tree under build/ and links
# its own server, so the three never share an object.
FLAVOR ?= obj
OBJDIR := build/$(FLAVOR)

CXX ?= g++
# Every .c file is compiled as C++ (sm/PART4.md section 2), and as
# strict C++: src/tools/cxx_keywords.py renamed the identifiers that
# are C++ keywords, and src/tools/cxx_casts.py wrote the casts C does
# not ask for. -w hides the warnings, which nobody has gone through yet.
STD := -x c++ -std=gnu++20 -fno-rtti -w
# What the build says of assertions: Assert() and assert() on, as a
# debug build has them, or NDEBUG, which compiles both out.
ASSERT := -DUSE_ASSERT_CHECKING

ifeq ($(FLAVOR),obj)
OPT ?= -O0 -g
SERVER := build/digres
# A session is to be a thread: no writable global may be left that is
# not thread_local (PLAN.md section 3). The default build's objects are
# the ones checked; the others are of the same sources, and the
# sanitizer's have static initialisers of its own.
CHECK_GLOBALS := python3 -B src/tools/cxx_thread_local.py --check $(OBJDIR)
# The server is initdb too, under the name initdb looks for.
SERVER_LINKS := ln -sf digres build/initdb
else ifeq ($(FLAVOR),tsan)
OPT := -O1 -g -fsanitize=thread
SERVER := build/digres-tsan
LINK_SM := $(LIBSM_TSAN)
LINK_AA := $(LIBAA_TSAN)
LINK_ROCKSDB := $(ROCKSDB_TSAN) -lsnappy -llz4 -lzstd -lz -lbz2
ROCKSDB_BUILD := static, $(shell cat $(dir $(ROCKSDB_TSAN))flags 2>/dev/null)
else ifeq ($(FLAVOR),rel)
OPT := -O2
ASSERT := -DNDEBUG
SERVER := build/digres-release
LIBSQ := $(LIBSQ_REL)
LINK_SM := $(LIBSM_REL)
LINK_AA := $(LIBAA_REL)
LINK_ROCKSDB := $(ROCKSDB_REL) -lsnappy -llz4 -lzstd -lz -lbz2
ROCKSDB_BUILD := static, $(shell cat $(dir $(ROCKSDB_REL))flags 2>/dev/null)
else
$(error FLAVOR is obj, tsan or rel, not "$(FLAVOR)")
endif

# Every directory may include sm_sys.h: the services dg is built on, in
# sm's level 0 (../sm/sm0); and aa.h, roles and privileges (../aa),
# which utils/pg_global.h includes.
CPPFLAGS := -I$(I) -I$(SM)/sm0/include -I$(AA)/include -D_GNU_SOURCE $(ASSERT)

# The directories that see more: the bridges to the storage manager
# (sm.h) and to the parser (sq.h).
$(OBJDIR)/backend/access/sm/%.o:    CPPFLAGS += -I$(SM)/include
$(OBJDIR)/backend/storage/file/%.o: CPPFLAGS += -I$(SM)/include
$(OBJDIR)/backend/storage/ipc/%.o:  CPPFLAGS += -I$(SM)/include
$(OBJDIR)/backend/utils/init/%.o:   CPPFLAGS += -I$(SM)/include
$(OBJDIR)/backend/parser/%.o:       CPPFLAGS += -I$(SQ)/include
$(OBJDIR)/backend/main/%.o:         CPPFLAGS += -I$(SM)/include -I$(SQ)/include

# Not compiled, each with the reason: a file that another #includes;
# a variant for a library digres does not link; one for an
# architecture, which the build never chooses (CODE.md section 1); or
# a generator's output that nodes/*.c #includes.
SKIP := \
    $(B)/libpq/be-gssapi-common.c $(B)/libpq/be-secure-gssapi.c \
    $(B)/libpq/be-secure-openssl.c \
    src/common/cryptohash_openssl.c src/common/hmac_openssl.c \
    $(B)/regex/regc_color.c $(B)/regex/regc_cvec.c $(B)/regex/regc_lex.c \
    $(B)/regex/regc_locale.c $(B)/regex/regc_nfa.c \
    $(B)/regex/regc_pg_locale.c $(B)/regex/rege_dfa.c \
    $(B)/utils/adt/levenshtein.c $(B)/utils/adt/like_match.c \
    $(B)/utils/activity/pgstat_wait_event.c \
    $(B)/utils/activity/wait_event_funcs_data.c \
    src/port/pg_crc32c_armv8.c src/port/pg_crc32c_armv8_choose.c \
    src/port/pg_crc32c_sse42.c src/port/pg_crc32c_sse42_choose.c \
    src/port/pg_popcount_avx512.c \
    $(wildcard $(B)/nodes/*.funcs.c $(B)/nodes/*.switch.c)

# Every other .c under src/, sorted, so the link is the same twice; the
# generated ones are there because "all" generates in a make of its own
# before this list is read.
SRCS := $(filter-out $(SKIP),$(sort $(shell find src -name '*.c')))
OBJS := $(SRCS:src/%.c=$(OBJDIR)/%.o)

# The headers gen_node_support.pl reads, in the order it insists on.
NODE_HEADERS := nodes/nodes.h nodes/primnodes.h nodes/parsenodes.h \
    nodes/pathnodes.h nodes/plannodes.h nodes/execnodes.h access/amapi.h \
    access/cmptype.h access/sdir.h access/tableam.h access/tsmapi.h \
    commands/event_trigger.h commands/trigger.h executor/tuptable.h \
    foreign/fdwapi.h nodes/bitmapset.h nodes/extensible.h \
    nodes/lockoptions.h nodes/miscnodes.h \
    nodes/supportnodes.h nodes/value.h utils/rel.h

CATALOG_HEADERS := $(filter-out %_d.h,$(wildcard $(I)/catalog/pg_*.h))

# One stamp a generator, since each writes several files.
GEN := $(I)/catalog/.stamp $(B)/nodes/.stamp $(B)/utils/.fmgr-stamp \
    $(I)/utils/errcodes.h \
    $(B)/utils/activity/.stamp $(I)/utils/probes.h \
    $(I)/pg_config_paths.h src/common/kwlist_d.h \
    $(B)/parser/sq_nodes.c $(I)/catalog/cat_builtin.h \
    $(B)/catalog/cat_builtin.c $(B)/utils/adt/jsonpath_gram.c \
    $(B)/utils/adt/jsonpath_scan.c $(B)/utils/misc/guc-file.c \
    $(B)/utils/misc/tznames_builtin.c

# A generated source must exist before the source list above is read,
# so the generators run in a make of their own and the server is built
# by a second one. Every target below that builds a server does the
# same.
.PHONY: all
all:
	$(MAKE) gen
	$(MAKE) server

.PHONY: gen
gen: $(GEN)

# The server: every object, into one program.
.PHONY: server
server: $(SERVER)

$(SERVER): $(OBJS) $(LIBSQ) $(LINK_SM) $(LINK_AA)
	@mkdir -p build
	$(CHECK_GLOBALS)
	# What it says of the RocksDB it links, for "digres --build"
	# (src/backend/main/rocksdb_build.cc); compiled at each link, since
	# the libraries it reports are relinked at each.
	$(CXX) $(OPT) -DDG_ROCKSDB_BUILD="\"$(ROCKSDB_BUILD)\"" -c \
	    -o $(OBJDIR)/rocksdb_build.o src/backend/main/rocksdb_build.cc
	$(CXX) $(OPT) -o $@ $(OBJDIR)/rocksdb_build.o $(OBJS) \
	    $(LIBSQ) $(LINK_AA) $(LINK_SM) $(LDLIBS)
	$(SERVER_LINKS)

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CXX) $(STD) $(OPT) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# The RocksDB the default build links, as "digres --build" names it.
ROCKSDB_BUILD ?= shared, the system's (Ubuntu's build, with its assert()s)

# The server again, every object compiled once more for the thread
# sanitizer, and the statements in test/ run with it, alone and in
# sessions, and test/server.sh run on it. The sanitizer cannot map its
# memory with the address space randomised, hence setarch -R. Every
# library that runs threads is instrumented: sm, sm_sys.h's services
# among it, and RocksDB, built by test/rocksdb.sh. sq, the parser, is
# not.
.PHONY: tsan
tsan: $(LIBSQ) $(LIBSM_TSAN) $(LIBAA_TSAN) $(ROCKSDB_TSAN)
	$(MAKE) gen
	$(MAKE) server FLAVOR=tsan
	DIGRES="setarch $$(uname -m) -R build/digres-tsan" test/run.sh
	$(MAKE) build/numbin
	DIGRES="setarch $$(uname -m) -R build/digres-tsan" test/server.sh

# The parser is sq (src/backend/parser/parser.c), whose nodes are digres's. sq's
# tests keep what PostgreSQL's parser printed for each of their files;
# "digres --parse" prints the same for raw_parser(), so they check
# that digres still parses to PostgreSQL's nodes.
# The server is built by a make of its own, here and in each target
# below that runs tests, so that the generators run before the
# source list is read.
.PHONY: test
test:
	$(MAKE) all
	SQ_PARSE="$(CURDIR)/build/digres --parse" $(SQ)/test/run.sh
	test/run.sh
	# initdb.sh drops pg_global with sm's driver, to make a data directory
	# from before digres kept roles.
	$(MAKE) -C $(SM)/rel
	test/initdb.sh
	$(MAKE) build/numbin
	test/server.sh
	$(MAKE) build/slt
	test/slt.sh $(SLT_QUICK)
	test/pgregress/pgregress.py -j 4 test

# The sqllogictest files make test runs: all but select5.test, whose
# joins of up to 64 tables take three minutes in this build to plan.
SLT_QUICK = $(filter-out %/select5.test,$(sort $(wildcard test/slt/*.test \
    test/slt/sqlite/*.test)))

# The sqllogictest runner (test/slt/README.md), a client of the
# system's libpq, as psql is; it hashes a result with OpenSSL's MD5.
build/slt: test/slt/slt.cc
	@mkdir -p build
	$(CXX) -std=c++20 -O2 -Wall $(shell pkg-config --cflags libpq) \
	    -o $@ $< -lpq -lcrypto

# A client binding numerics in binary, as a driver does (test/numbin),
# which test/server.sh runs; a client of the system's libpq, as slt is.
build/numbin: test/numbin/numbin.cc
	@mkdir -p build
	$(CXX) -std=c++20 -O2 -Wall $(shell pkg-config --cflags libpq) \
	    -o $@ $< -lpq

# The sqllogictest files under test/slt, on digres.
.PHONY: slt
slt:
	$(MAKE) all
	$(MAKE) build/slt
	test/slt.sh

# PostgreSQL 18's isolation specs on digres, each against what
# test/isolation_specs.txt says it is to do.
.PHONY: isolation
isolation:
	$(MAKE) all
	test/isolation.sh

# PostgreSQL 18's regression suite on digres: a report of what fails and
# why (test/regress/README.md). It only measures, and never fails.
.PHONY: regress
regress:
	$(MAKE) all
	test/regress.sh

# PostgreSQL 18's regression files, ported: what digres does not run as
# PostgreSQL does commented out, and the rest must give PostgreSQL's output
# (test/pgregress/README.md).
.PHONY: pgregress
pgregress:
	$(MAKE) all
	test/pgregress/pgregress.py -j 4 test

# A sample of SQLite's whole corpus, fetched into build/slt-corpus, on
# PostgreSQL 18 and on digres: what digres alone fails.
.PHONY: slt-corpus
slt-corpus:
	$(MAKE) all
	$(MAKE) build/slt
	test/slt_corpus.sh

# The server again, optimized and without Assert() or assert(), on sq,
# sm and RocksDB built the same way: what bench/ measures
# (../bench/DESIGN.md section 2). "digres-release --init" makes a data
# directory.
.PHONY: release
release: $(LIBSQ_REL) $(LIBSM_REL) $(LIBAA_REL) $(ROCKSDB_REL)
	$(MAKE) gen
	$(MAKE) server FLAVOR=rel

# sq builds its own library; whether it is up to date is sq's to say.
.PHONY: $(LIBSQ) $(LIBSQ_REL)
$(LIBSQ):
	$(MAKE) -C $(SQ) build/libsq.a
$(LIBSQ_REL):
	$(MAKE) -C $(SQ) build/libsq-release.a

# So does sm, plain, for the thread sanitizer, and for the release build.
.PHONY: $(LIBSM) $(LIBSM_TSAN) $(LIBSM_REL)
$(LIBSM):
	$(MAKE) -C $(SM)
$(LIBSM_TSAN):
	$(MAKE) -C $(SM) BUILD=build/tsan OPTFLAGS="-O1 -g -fsanitize=thread" \
	    build/tsan/libsm.a
$(LIBSM_REL):
	$(MAKE) -C $(SM) BUILD=build/release OPTFLAGS="-O3 -DNDEBUG" build/release/libsm.a

# So does aa, each way sm is built, and sm under it.
.PHONY: $(LIBAA) $(LIBAA_TSAN) $(LIBAA_REL)
$(LIBAA): $(LIBSM)
	$(MAKE) -C $(AA) build/libaa.a
$(LIBAA_TSAN): $(LIBSM_TSAN)
	$(MAKE) -C $(AA) BUILD=build/tsan OPTFLAGS="-O1 -g -fsanitize=thread" \
	    build/tsan/libaa.a
$(LIBAA_REL): $(LIBSM_REL)
	$(MAKE) -C $(AA) BUILD=build/release OPTFLAGS="-O3 -DNDEBUG" \
	    build/release/libaa.a

# RocksDB for the thread sanitizer, and for the release build, each built
# once.
$(ROCKSDB_TSAN):
	test/rocksdb.sh tsan
$(ROCKSDB_REL):
	test/rocksdb.sh release

# The catalogs' #defines: pg_*_d.h and the syscache tables.
$(I)/catalog/.stamp: $(CATALOG_HEADERS) $(wildcard $(I)/catalog/*.dat) \
        $(B)/catalog/genbki.pl $(B)/catalog/Catalog.pm
	$(PERL) -I$(B)/catalog $(B)/catalog/genbki.pl --include-path=$(I)/ \
	    --set-version=18 --output=$(I)/catalog/ $(CATALOG_HEADERS)
	touch $@

# Node tags, and the copy, equal, out, read and jumble functions.
$(B)/nodes/.stamp: $(addprefix $(I)/,$(NODE_HEADERS)) \
        $(B)/nodes/gen_node_support.pl
	cd $(I) && $(PERL) ../backend/nodes/gen_node_support.pl \
	    --outdir ../backend/nodes $(NODE_HEADERS)
	cp $(B)/nodes/nodetags.h $(I)/nodes/nodetags.h
	touch $@

# The table of built-in functions, and their oids and prototypes.
$(B)/utils/.fmgr-stamp: $(I)/catalog/pg_proc.dat $(B)/utils/Gen_fmgrtab.pl
	$(PERL) -I$(B)/catalog $(B)/utils/Gen_fmgrtab.pl \
	    --include-path=$(I)/ --output=$(B)/utils/ $<
	cp $(B)/utils/fmgroids.h $(B)/utils/fmgrprotos.h $(I)/utils/
	touch $@

$(I)/utils/errcodes.h: $(B)/utils/errcodes.txt $(B)/utils/generate-errcodes.pl
	$(PERL) $(B)/utils/generate-errcodes.pl --outfile $@ $<

$(B)/utils/activity/.stamp: $(B)/utils/activity/wait_event_names.txt
	$(PERL) $(B)/utils/activity/generate-wait_event_types.pl --code \
	    --outdir $(B)/utils/activity $<
	cp $(B)/utils/activity/wait_event_types.h $(I)/utils/
	touch $@

# No DTrace: every probe is an empty macro.
$(I)/utils/probes.h: $(B)/utils/probes.d $(B)/utils/Gen_dummy_probes.pl
	$(PERL) -n $(B)/utils/Gen_dummy_probes.pl $< > $@

$(I)/pg_config_paths.h: Makefile
	printf '#define %s "$(PREFIX)/%s"\n' PGBINDIR bin PGSHAREDIR share \
	    SYSCONFDIR etc INCLUDEDIR include PKGINCLUDEDIR include \
	    INCLUDEDIRSERVER include/server LIBDIR lib PKGLIBDIR lib \
	    LOCALEDIR share/locale DOCDIR share/doc HTMLDIR share/doc \
	    MANDIR share/man > $@

src/common/kwlist_d.h: $(I)/parser/kwlist.h src/tools/gen_keywordlist.pl
	$(PERL) -Isrc/tools src/tools/gen_keywordlist.pl --extern \
	    -o src/common $<

# The built-in catalog rows, compiled in (sm/PART4.md section 10): from
# postgres.bki, where genbki.pl has resolved every reference to an oid;
# and the functions' ACLs from initdb's scripts, which digres runs none of.
$(I)/catalog/cat_builtin.h: $(I)/catalog/.stamp $(B)/catalog/gen_cat.py
	python3 $(B)/catalog/gen_cat.py $(I)/catalog/postgres.bki header > $@
$(B)/catalog/cat_builtin.c: $(I)/catalog/.stamp $(B)/catalog/gen_cat.py \
    $(B)/catalog/system_functions.sql $(B)/catalog/system_views.sql
	python3 $(B)/catalog/gen_cat.py $(I)/catalog/postgres.bki source > $@

# That sq's nodes are digres's, held at compile time, and the statements
# digres refuses: written by sq's generator from the node headers it has.
$(B)/parser/sq_nodes.c: $(SQ)/gen/gen_nodes.py $(SQ)/gen/nodespec.py \
        $(wildcard $(SQ)/spec/*)
	python3 $(SQ)/gen/gen_nodes.py checks > $@

# Grammars and scanners. SQL's own are sq's.
$(B)/utils/adt/jsonpath_gram.c: $(B)/utils/adt/jsonpath_gram.y
	bison -Wno-deprecated -d -o $@ $<
$(B)/utils/adt/jsonpath_scan.c: $(B)/utils/adt/jsonpath_scan.l
	flex -CF -p -p -o $@ $<
# flex leaves this table writable, and it is never written: const
# keeps it out of the variables each thread copies.
	sed -i 's/\*yy_start_state_list\[/*const yy_start_state_list[/' $@
$(B)/utils/misc/guc-file.c: $(B)/utils/misc/guc-file.l
	flex -o $@ $<

# The time zone abbreviation sets, compiled in: digres has no
# installation to read them from (src/tools/gen_tznames.py).
$(B)/utils/misc/tznames_builtin.c: src/tools/gen_tznames.py \
        $(wildcard src/timezone/tznames/*)
	python3 -B src/tools/gen_tznames.py src/timezone/tznames > $@
# Every .c and .h, src/include's among them; .clang-format-ignore at
# dg/ keeps the generators' headers as they are.
.PHONY: format
format:
	find src \( -name '*.c' -o -name '*.h' \) -print0 \
	    | xargs -0 clang-format -i

# Everything built, which is all under build/, and everything generated.
.PHONY: clean
clean:
	rm -rf build
	rm -f $(GEN) $(I)/catalog/pg_*_d.h $(I)/catalog/schemapg.h \
	    $(I)/catalog/syscache_ids.h $(I)/catalog/syscache_info.h \
	    $(I)/catalog/system_fk_info.h $(I)/catalog/postgres.bki \
	    $(I)/catalog/system_constraints.sql \
	    $(B)/nodes/*.funcs.c $(B)/nodes/*.switch.c $(B)/nodes/nodetags.h \
	    $(I)/nodes/nodetags.h $(B)/utils/fmgroids.h \
	    $(B)/utils/fmgrprotos.h $(B)/utils/fmgrtab.c \
	    $(I)/utils/fmgroids.h $(I)/utils/fmgrprotos.h \
	    $(B)/utils/activity/wait_event_types.h \
	    $(B)/utils/activity/pgstat_wait_event.c \
	    $(B)/utils/activity/wait_event_funcs_data.c \
	    $(I)/utils/wait_event_types.h $(I)/utils/pgstat_wait_event.c \
	    $(I)/utils/wait_event_funcs_data.c \
	    $(B)/utils/adt/jsonpath_gram.h

-include $(OBJS:.o=.d)
