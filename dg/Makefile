# dg -- build digres.
#
#   make -j12          generate what is generated, then compile
#   make -k -j12       the same, going on past a file that fails
#   make gen           only generate
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
# Where digres is installed, which the server and psql are built knowing.
PREFIX ?= /usr/local/dg

# The headers gen_node_support.pl reads, in the order it insists on.
NODE_HEADERS := nodes/nodes.h nodes/primnodes.h nodes/parsenodes.h \
    nodes/pathnodes.h nodes/plannodes.h nodes/execnodes.h access/amapi.h \
    access/cmptype.h access/sdir.h access/tableam.h access/tsmapi.h \
    commands/event_trigger.h commands/trigger.h executor/tuptable.h \
    foreign/fdwapi.h nodes/bitmapset.h nodes/extensible.h \
    nodes/lockoptions.h nodes/miscnodes.h nodes/replnodes.h \
    nodes/supportnodes.h nodes/value.h utils/rel.h

CATALOG_HEADERS := $(filter-out %_d.h,$(wildcard $(I)/catalog/pg_*.h))

# One stamp a generator, since each writes several files.
GEN := $(I)/catalog/.stamp $(B)/nodes/.stamp $(B)/utils/.fmgr-stamp \
    $(I)/utils/errcodes.h $(I)/storage/lwlocknames.h \
    $(B)/utils/activity/.stamp $(I)/utils/probes.h \
    $(I)/pg_config_paths.h src/common/kwlist_d.h \
    $(B)/parser/gram.c $(B)/parser/scan.c $(B)/utils/adt/jsonpath_gram.c \
    $(B)/utils/adt/jsonpath_scan.c $(B)/utils/misc/guc-file.c \
    src/fe_utils/psqlscan.c src/bin/psql/psqlscanslash.c \
    src/bin/psql/tab-complete.c

.PHONY: gen
gen: $(GEN)
src: gen

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

$(I)/storage/lwlocknames.h: $(I)/storage/lwlocklist.h \
        $(B)/utils/activity/wait_event_names.txt
	$(PERL) $(B)/storage/lmgr/generate-lwlocknames.pl \
	    --outdir $(I)/storage $^

$(B)/utils/activity/.stamp: $(B)/utils/activity/wait_event_names.txt
	$(PERL) $(B)/utils/activity/generate-wait_event_types.pl --code \
	    --outdir $(B)/utils/activity $<
	cp $(B)/utils/activity/wait_event_types.h \
	    $(B)/utils/activity/pgstat_wait_event.c \
	    $(B)/utils/activity/wait_event_funcs_data.c $(I)/utils/
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

# Grammars and scanners.
$(B)/parser/gram.c: $(B)/parser/gram.y
	bison -Wno-deprecated -d -o $@ $<
	cp $(B)/parser/gram.h $(I)/parser/gram.h
$(B)/parser/scan.c: $(B)/parser/scan.l
	flex -CF -p -p -o $@ $<
$(B)/utils/adt/jsonpath_gram.c: $(B)/utils/adt/jsonpath_gram.y
	bison -Wno-deprecated -d -o $@ $<
$(B)/utils/adt/jsonpath_scan.c: $(B)/utils/adt/jsonpath_scan.l
	flex -CF -p -p -o $@ $<
$(B)/utils/misc/guc-file.c: $(B)/utils/misc/guc-file.l
	flex -o $@ $<
src/fe_utils/psqlscan.c: src/fe_utils/psqlscan.l
	flex -Cfe -p -p -o $@ $<
src/bin/psql/psqlscanslash.c: src/bin/psql/psqlscanslash.l
	flex -Cfe -p -p -o $@ $<
src/bin/psql/tab-complete.c: src/bin/psql/tab-complete.in.c
	$(PERL) src/bin/psql/gen_tabcomplete.pl $< --outfile $@

# Headers are formatted here: src/include has no Makefile.
format: format-include
.PHONY: format-include
format-include:
	find src/include -name '*.h' | xargs clang-format -i

clean: clean-gen
.PHONY: clean-gen
clean-gen:
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
	    $(I)/utils/wait_event_funcs_data.c $(B)/parser/gram.h \
	    $(I)/parser/gram.h $(B)/utils/adt/jsonpath_gram.h
