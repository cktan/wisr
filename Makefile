# wisr -- a stack of storage levels over RocksDB.
#
# Level 0 is RocksDB. Level 1 is sm1/, the MVCC storage manager.
# Level 2 is sm2/, types and records, and level 3 is sm3/, schema
# and indexes, which is not built yet.
#
# This file only delegates; each level has a Makefile of its own
# with the real targets, and any of them can be run directly:
#
#   make -C sm1 test      the same as "make test" here
#
#   make              build every level
#   make test         build and test every level
#   make clean        remove each level's build directory
#   make format       clang-format each level in place
#
# LEVELS lists the levels that have a Makefile, in build order;
# sm3 joins it when it has one.
LEVELS := sm1 sm2

.PHONY: all test clean format $(LEVELS)

all: $(LEVELS)

$(LEVELS):
	$(MAKE) -C $@

# sm2 links sm1's library, so sm1 builds first.
sm2: sm1

# Each of these runs the level's own target, in order, and stops
# at the first level that fails.
test clean format:
	@for d in $(LEVELS); do \
	  $(MAKE) -C $$d $@ || exit 1; \
	done
