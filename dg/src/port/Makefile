top := ../..
# No processor's CRC or popcount instructions: pg_config.h chooses
# the plain C ones, so the build is the same on every machine.
SKIP := pg_crc32c_armv8.c pg_crc32c_armv8_choose.c \
        pg_crc32c_sse42.c pg_crc32c_sse42_choose.c \
        pg_popcount_aarch64.c pg_popcount_avx512.c
include $(top)/dg.mk
