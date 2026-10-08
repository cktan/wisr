top := ../..
# One file a processor: the others are not this machine's.
ifeq ($(shell uname -m),aarch64)
SKIP := pg_crc32c_sse42.c pg_crc32c_sse42_choose.c pg_popcount_avx512.c
else
SKIP := pg_crc32c_armv8.c pg_crc32c_armv8_choose.c pg_popcount_aarch64.c
endif
include $(top)/dg.mk

# The CRC instructions are an extension the compiler must be told of.
pg_crc32c_armv8.o: OPT += -march=armv8-a+crc
pg_crc32c_sse42.o: OPT += -msse4.2

