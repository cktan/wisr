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
#   make clean         remove objects and generated files
#   make format        clang-format every .c and .h
#
# Each directory under src/ has a Makefile of three lines or so
# that includes dg.mk; this one adds the generated sources, which
# must exist before anything is compiled.

top := .
SUBDIRS := src
include $(top)/dg.mk

I := src/include
B := src/backend
PERL := perl
# RocksDB, which sm links: the system's, or, for the thread sanitizer, one
# built for it with its compression libraries (test/rocksdb.sh).
LINK_ROCKSDB = -lrocksdb
ROCKSDB_TSAN := build/rocksdb-tsan/librocksdb.a
ROCKSDB_REL := build/rocksdb-release/librocksdb.a
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
# Where digres is installed, which the server is built knowing.
PREFIX ?= /usr/local/dg

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
    $(B)/parser/sq_convert.c $(I)/catalog/cat_builtin.h \
    $(B)/catalog/cat_builtin.c $(B)/utils/adt/jsonpath_gram.c \
    $(B)/utils/adt/jsonpath_scan.c $(B)/utils/misc/guc-file.c \
    $(B)/utils/misc/tznames_builtin.c

.PHONY: gen
gen: $(GEN)
src: gen

# The server: every object under src/, into one program. It is
# relinked every time, since the objects are built by the Makefiles
# below and this one cannot tell which of them changed.
all: build/digres
.PHONY: build/digres
build/digres: src $(LIBSQ) $(LIBSM)
	@mkdir -p build
	# A session is to be a thread: no writable global may be left
	# that is not thread_local (PLAN.md section 3).
	python3 -B src/tools/cxx_thread_local.py --check src
	$(call link_server,$@,.o,$(OPT))
	ln -sf digres build/initdb

# Link $(1) from the objects under src/ named *$(2), compiling with
# $(3), and with what it says of the RocksDB it links, ROCKSDB_BUILD,
# for "digres --build" (src/backend/main/rocksdb_build.cc).
define link_server
	$(CXX) $(3) -DDG_ROCKSDB_BUILD="\"$(ROCKSDB_BUILD)\"" -c \
	    -o build/rocksdb_build$(2) src/backend/main/rocksdb_build.cc
	$(CXX) $(3) -o $(1) build/rocksdb_build$(2) \
	    $$(find src -name '*$(2)' \
	        $(if $(filter .o,$(2)),! -name '*.tsan.o' ! -name '*.rel.o')) \
	    $(LIBSQ) $(LINK_SM) $(LDLIBS)
endef

# The RocksDB each build links, as "digres --build" names it.
ROCKSDB_BUILD = shared, the system's (Ubuntu's build, with its assert()s)

# The server again, each object compiled once more as NAME.tsan.o for
# the thread sanitizer, and the statements in test/ run with it, alone
# and in sessions, and test/server.sh run on it. The sanitizer cannot map its memory with the address
# space randomised, hence setarch -R. Every library that runs threads
# is instrumented: sm, sm_sys.h's services among it, and RocksDB, built by
# test/rocksdb.sh. sq, the parser, is not.
.PHONY: tsan
tsan: LINK_SM = $(LIBSM_TSAN)
tsan: LINK_ROCKSDB = $(ROCKSDB_TSAN) -lsnappy -llz4 -lzstd -lz -lbz2
tsan: ROCKSDB_BUILD = static, $$(cat $(dir $(ROCKSDB_TSAN))flags)
tsan: gen $(LIBSQ) $(LIBSM_TSAN) $(ROCKSDB_TSAN)
	$(MAKE) -C src OBJ=tsan.o
	@mkdir -p build
	$(call link_server,build/digres-tsan,.tsan.o,-O1 -g -fsanitize=thread)
	DIGRES="setarch $$(uname -m) -R build/digres-tsan" test/run.sh
	$(MAKE) build/numbin
	DIGRES="setarch $$(uname -m) -R build/digres-tsan" test/server.sh

# The parser is sq and a converter (src/backend/parser/parser.c). sq's
# tests keep what PostgreSQL's parser printed for each of their files;
# "digres --parse" prints the same for raw_parser(), so they check
# that digres still parses to PostgreSQL's nodes.
# The server is built by a make of its own, here and in each target below
# that runs tests: a goal named here is passed down to every directory,
# and none of them has a "test", or any of the others.
.PHONY: test
test:
	$(MAKE) all
	SQ_PARSE="$(CURDIR)/build/digres --parse" $(SQ)/test/run.sh
	test/run.sh
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

# The server again, each object compiled once more as NAME.rel.o,
# optimized and without Assert() or assert(), on sq, sm and RocksDB built
# the same way: what bench/ measures (../bench/DESIGN.md section 2).
# "digres-release --init" makes a data directory.
.PHONY: release
release: LIBSQ = $(LIBSQ_REL)
release: LINK_SM = $(LIBSM_REL)
release: LINK_ROCKSDB = $(ROCKSDB_REL) -lsnappy -llz4 -lzstd -lz -lbz2
release: ROCKSDB_BUILD = static, $$(cat $(dir $(ROCKSDB_REL))flags)
release: gen $(LIBSQ_REL) $(LIBSM_REL) $(ROCKSDB_REL)
	$(MAKE) -C src OBJ=rel.o OPT="-O2 -DNDEBUG" ASSERT=
	@mkdir -p build
	$(call link_server,build/digres-release,.rel.o,-O2)

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
# postgres.bki, where genbki.pl has resolved every reference to an oid.
$(I)/catalog/cat_builtin.h: $(I)/catalog/.stamp $(B)/catalog/gen_cat.py
	python3 $(B)/catalog/gen_cat.py $(I)/catalog/postgres.bki header > $@
$(B)/catalog/cat_builtin.c: $(I)/catalog/.stamp $(B)/catalog/gen_cat.py
	python3 $(B)/catalog/gen_cat.py $(I)/catalog/postgres.bki source > $@

# sq's tree as PostgreSQL's nodes, written by sq's generator from the
# node headers it has: one function a node, as its other output is.
$(B)/parser/sq_convert.c: $(SQ)/gen/gen_nodes.py $(SQ)/gen/nodespec.py \
        $(wildcard $(SQ)/spec/*)
	python3 $(SQ)/gen/gen_nodes.py converter > $@

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

# Headers are formatted here: src/include has no Makefile.
format: format-include
.PHONY: format-include
format-include:
	find src/include -name '*.h' | xargs clang-format -i

clean: clean-gen
.PHONY: clean-gen
clean-gen:
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
