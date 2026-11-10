/*
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 */

/*-------------------------------------------------------------------------
 *
 * hba.h
 *	  How a connection is authenticated: the method, and what it needs.
 *	  digres reads no pg_hba.conf; the rule is fixed, in libpq/auth.c.
 *
 *
 * src/include/libpq/hba.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HBA_H
#define HBA_H

#include "libpq/pqcomm.h" /* needed for NetBSD */
#include "nodes/pg_list.h"

/*
 * The following enum represents the authentication methods that
 * are supported by PostgreSQL.
 *
 * Note: keep this in sync with the UserAuthName array in auth.c.
 */
typedef enum UserAuth {
  uaReject,
  uaImplicitReject, /* Not a user-visible option */
  uaTrust,
  uaIdent,
  uaPassword,
  uaSCRAM,
  uaGSS,
  uaSSPI,
  uaPAM,
  uaBSD,
  uaLDAP,
  uaCert,
  uaRADIUS,
  uaPeer,
  uaOAuth,
#define USER_AUTH_LAST uaOAuth /* Must be last value of this enum */
} UserAuth;

/*
 * What a connection's authentication is to be: PostgreSQL's pg_hba.conf
 * entry, less what matched a connection to it.
 */

typedef enum ConnType {
  ctLocal,
  ctHost,
  ctHostSSL,
  ctHostNoSSL,
  ctHostGSS,
  ctHostNoGSS,
} ConnType;

typedef enum ClientCertMode {
  clientCertOff,
  clientCertCA,
  clientCertFull,
} ClientCertMode;

typedef enum ClientCertName {
  clientCertCN,
  clientCertDN,
} ClientCertName;

typedef struct HbaLine {
  ConnType conntype;
  UserAuth auth_method;
  char *usermap;
  char *pamservice;
  bool pam_use_hostname;
  bool ldaptls;
  char *ldapscheme;
  char *ldapserver;
  int ldapport;
  char *ldapbinddn;
  char *ldapbindpasswd;
  char *ldapsearchattribute;
  char *ldapsearchfilter;
  char *ldapbasedn;
  int ldapscope;
  char *ldapprefix;
  char *ldapsuffix;
  ClientCertMode clientcert;
  ClientCertName clientcertname;
  char *krb_realm;
  bool include_realm;
  bool compat_realm;
  bool upn_username;
  List *radiusservers;
  char *radiusservers_s;
  List *radiussecrets;
  char *radiussecrets_s;
  List *radiusidentifiers;
  char *radiusidentifiers_s;
  List *radiusports;
  char *radiusports_s;
  char *oauth_issuer;
  char *oauth_scope;
  char *oauth_validator;
  bool oauth_skip_usermap;
} HbaLine;

/* kluge to avoid including libpq/libpq-be.h here */
typedef struct Port hbaPort;

extern const char *hba_authname(UserAuth auth_method);
extern void hba_getauthmethod(hbaPort *port);
extern int check_usermap(const char *usermap_name, const char *pg_user,
                         const char *system_user, bool case_insensitive);
extern bool pg_isblank(const char c);

#endif /* HBA_H */
