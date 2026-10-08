top := ../../..
FRONTEND := 1
INCLUDES := -I$(top)/src/port
# No TLS or GSSAPI library is chosen yet (to be decided).
SKIP := fe-gssapi-common.c fe-secure-common.c fe-secure-gssapi.c \
        fe-secure-openssl.c
include $(top)/dg.mk
