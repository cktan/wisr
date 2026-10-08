# sq -- the SQL front end, cut out of digres: text in, parse tree out.
#
#   make -j4         build build/sqparse
#   make test        build, and run the tests in test/
#   make clean       remove build/
#   make format      clang-format src/
#
# The sources are still in ../dg/src, and are compiled from there
# into build/: README.md says why they have not moved yet. What is
# here is the boundary: the list of what belongs to sq, src/host.c
# for what that list still needs from the server, and the tests.

DG  := ../dg
SRC := $(DG)/src
B   := $(SRC)/backend

CXX ?= g++
OPT ?= -O0 -g
# One section a function, so that the link keeps only what the
# parser reaches, and reports only what that needs.
CXXFLAGS = -x c++ -std=gnu++20 -w $(OPT) -ffunction-sections \
            -fdata-sections -I$(SRC)/include -D_GNU_SOURCE -MMD -MP
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
OBJS    := build/sqparse.o build/host.o $(DG_OBJS)

.PHONY: all test clean format gen
all: build/sqparse

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

build/sqparse: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

test: build/sqparse
	test/run.sh

clean:
	rm -rf build

format:
	clang-format -i src/*.c

-include $(OBJS:.o=.d)
