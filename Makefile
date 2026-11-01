# wisr -- four parts, each with a Makefile of its own (README.md): sm/,
# the storage stack; sq/, the SQL parser; aa/, roles and privileges; and
# dg/, digres, which links the other three. This file builds and tests
# them together.
#
#   DEBUG=1 make -j4    the debug build, dg/build/digres: what tests run
#   make -j4            the release build, dg/build/digres-release: what
#                       the benchmark runs
#   make -j4 test       every part's tests, on the debug build, in order:
#                       sm, sq, aa, then digres; make test-sm, test-sq,
#                       test-aa or test-dg, one part's
#   make -j4 tsan       digres under the thread sanitizer, and its tests
#   make -j4 bench      the release build, then bench.old/run.sh
#   make format         clang-format sm, sq and aa
#   make clean          remove every part's build directory
#
# dg is not formatted here: its tree is formatted by the lines a change
# touches (dg/CODE.md section 8).

.NOTPARALLEL:

DIRS := sm sq aa dg

TESTDIRS := $(DIRS:%=test-%)
CLEANDIRS := $(DIRS:%=clean-%)
FORMATDIRS := $(filter-out format-dg,$(DIRS:%=format-%))

# digres builds sq, sm and aa first, as it links them: -O0 -g with
# assertions for the tests, or optimized, with RocksDB built from source,
# for the benchmark (bench.old/CODE.md).
all:
ifeq ($(DEBUG),1)
	$(MAKE) -C dg
else
	$(MAKE) -C dg release
endif

# The tests run on the debug build, whatever DEBUG is: sm first, as aa's
# tests link its library, and digres last, as it runs on all three.
test: $(TESTDIRS)

$(TESTDIRS): export DEBUG := 1
$(TESTDIRS):
	$(MAKE) -C $(@:test-%=%) test

tsan:
	$(MAKE) -C dg tsan

bench:
	$(MAKE) -C dg release
	bench.old/run.sh

format: $(FORMATDIRS)

$(FORMATDIRS):
	$(MAKE) -C $(@:format-%=%) format

clean: $(CLEANDIRS)

$(CLEANDIRS):
	$(MAKE) -C $(@:clean-%=%) clean

.PHONY: $(TESTDIRS) $(CLEANDIRS) $(FORMATDIRS)
.PHONY: all test tsan bench format clean
