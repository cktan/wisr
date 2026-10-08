top := ../..
BOTH := 1
# No TLS library is chosen yet (to be decided): the OpenSSL
# versions wait for that.
SKIP := cryptohash_openssl.c hmac_openssl.c
# For a client only.
SKIP_BE := fe_memutils.c logging.c restricted_token.c sprompt.c
include $(top)/dg.mk
