# wisr -- three parts, each with a Makefile of its own (README.md):
# sm/, the storage stack; sq/, the SQL parser; and dg/, digres, which
# links both. This file only delegates to them.
#
#   make          build everything: digres builds sq and sm first
#   make test     test sm, then sq, then digres, stopping at a failure
#   make format   clang-format sm and sq
#   make clean    remove each part's build directory
#
# dg is not formatted here: its whole tree is not clang-format clean,
# and is formatted by the lines a change touches.

.PHONY: all test format clean

all:
	$(MAKE) -C dg

# One part after another: each prints a summary of its own.
test:
	$(MAKE) -C sm test
	$(MAKE) -C sq test
	$(MAKE) -C dg test

format:
	$(MAKE) -C sm format
	$(MAKE) -C sq format

clean:
	$(MAKE) -C sm clean
	$(MAKE) -C sq clean
	$(MAKE) -C dg clean
