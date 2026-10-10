top := ../..
# No processor's CRC or popcount instructions: pg_config.h chooses
# the plain C ones, so the build is the same on every machine. The
# exception is pg_popcount_aarch64.c: on aarch64 the compiler's
# __ARM_NEON, not pg_config.h, selects its Neon popcount
# (pg_bitutils.h), and elsewhere it compiles to nothing.
SKIP := pg_crc32c_armv8.c pg_crc32c_armv8_choose.c \
        pg_crc32c_sse42.c pg_crc32c_sse42_choose.c \
        pg_popcount_avx512.c
include $(top)/dg.mk
