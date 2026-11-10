/*-------------------------------------------------------------------------
 *
 * crypt.c
 *	  Functions for dealing with encrypted passwords stored in
 *	  pg_authid.rolpassword.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/backend/libpq/crypt.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "catalog/pg_authid.h"
#include "common/md5.h"
#include "common/scram-common.h"
#include "libpq/crypt.h"
#include "libpq/scram.h"
#include "utils/builtins.h"
#include "utils/pg_global.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

/*
 * Fetch stored password for a user, for authentication.
 *
 * On error, returns NULL, and stores a palloc'd string describing the reason,
 * for the postmaster log, in *logdetail.  The error reason should *not* be
 * sent to the client, to avoid giving away user information!
 */
char *get_role_password(const char *role, const char **logdetail) {
  /* The roles are pg_global's, and which secret serves is aa's. */
  std::string secret, why;

  if (!aa_role_secret(pg_global_env(), role, GetCurrentTimestamp(), &secret,
                      &why)) {
    *logdetail = pstrdup(why.c_str());
    return NULL;
  }
  return pstrdup(secret.c_str());
}

/*
 * What kind of a password type is 'shadow_pass'?
 */
PasswordType get_password_type(const char *shadow_pass) {
  char *encoded_salt;
  int iterations;
  int key_length = 0;
  pg_cryptohash_type hash_type;
  uint8 stored_key[SCRAM_MAX_KEY_LEN];
  uint8 server_key[SCRAM_MAX_KEY_LEN];

  if (strncmp(shadow_pass, "md5", 3) == 0 &&
      strlen(shadow_pass) == MD5_PASSWD_LEN &&
      strspn(shadow_pass + 3, MD5_PASSWD_CHARSET) == MD5_PASSWD_LEN - 3)
    return PASSWORD_TYPE_MD5;
  if (parse_scram_secret(shadow_pass, &iterations, &hash_type, &key_length,
                         &encoded_salt, stored_key, server_key))
    return PASSWORD_TYPE_SCRAM_SHA_256;
  return PASSWORD_TYPE_PLAINTEXT;
}

/*
 * Given a user-supplied password, convert it into a secret of
 * 'target_type' kind.
 *
 * If the password is already in encrypted form, we cannot reverse the
 * hash, so it is stored as it is regardless of the requested type.
 */
char *encrypt_password(PasswordType target_type, const char *role,
                       const char *password) {
  PasswordType guessed_type = get_password_type(password);
  char *encrypted_password = NULL;

  if (guessed_type != PASSWORD_TYPE_PLAINTEXT) {
    /*
     * Cannot convert an already-encrypted password from one format to
     * another, so return it as it is.
     */
    encrypted_password = pstrdup(password);
  } else {
    switch (target_type) {
    case PASSWORD_TYPE_MD5:
      /* password_encryption has no md5: no role gets an MD5 password. */
      elog(ERROR, "cannot encrypt password with 'md5'");
      break;

    case PASSWORD_TYPE_SCRAM_SHA_256:
      encrypted_password = pg_be_scram_build_secret(password);
      break;

    case PASSWORD_TYPE_PLAINTEXT:
      elog(ERROR, "cannot encrypt password with 'plaintext'");
      break;
    }
  }

  Assert(encrypted_password);

  /*
   * Valid password hashes may be very long, but we don't want to store
   * anything that might need out-of-line storage, since de-TOASTing won't
   * work during authentication because we haven't selected a database yet
   * and cannot read pg_class. 512 bytes should be more than enough for all
   * practical use, so fail for anything longer.
   */
  if (encrypted_password && /* keep compiler quiet */
      strlen(encrypted_password) > MAX_ENCRYPTED_PASSWORD_LEN) {
    /*
     * We don't expect any of our own hashing routines to produce hashes
     * that are too long.
     */
    Assert(guessed_type != PASSWORD_TYPE_PLAINTEXT);

    ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("encrypted password is too long"),
             errdetail("Encrypted passwords must be no longer than %d bytes.",
                       MAX_ENCRYPTED_PASSWORD_LEN)));
  }

  return encrypted_password;
}

/*
 * Check given password for given user, and return STATUS_OK or STATUS_ERROR.
 *
 * 'shadow_pass' is the user's correct password hash, as stored in
 * pg_authid.rolpassword.
 * 'client_pass' is the password given by the remote user.
 *
 * In the error case, store a string at *logdetail that will be sent to the
 * postmaster log (but not the client).
 */
int plain_crypt_verify(const char *role, const char *shadow_pass,
                       const char *client_pass, const char **logdetail) {
  /*
   * Client sent password in plaintext: check it against the SCRAM secret
   * stored.  No role has an MD5 password (../aa/ROLE.md section 5.1).
   */
  switch (get_password_type(shadow_pass)) {
  case PASSWORD_TYPE_SCRAM_SHA_256:
    if (scram_verify_plain_password(role, client_pass, shadow_pass)) {
      return STATUS_OK;
    } else {
      *logdetail =
          psprintf(_("Password does not match for user \"%s\"."), role);
      return STATUS_ERROR;
    }
    break;

  case PASSWORD_TYPE_MD5:
  case PASSWORD_TYPE_PLAINTEXT:

    /*
     * We never store passwords in plaintext, nor an MD5 hash, so this
     * shouldn't happen.
     */
    break;
  }

  /*
   * This shouldn't happen.  Plain "password" authentication is possible
   * with any kind of stored password hash.
   */
  *logdetail =
      psprintf(_("Password of user \"%s\" is in unrecognized format."), role);
  return STATUS_ERROR;
}
