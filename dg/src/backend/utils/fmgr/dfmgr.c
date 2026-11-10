/*-------------------------------------------------------------------------
 *
 * dfmgr.c
 *	  Dynamic function manager code.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/fmgr/dfmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <algorithm>
#include <vector>

#ifndef WIN32
#include <dlfcn.h>
#endif /* !WIN32 */

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "utils/control_block.h"
#include "utils/hsearch.h"

/* hashtable entry for rendezvous variables */
typedef struct {
  char varName[NAMEDATALEN]; /* hash key (must be first) */
  void *varValue;
} rendezvousHashEntry;

/*
 * A dynamically loaded file (kept in malloc'd memory).  Once loaded, it is
 * never written or freed: a session's list may hold its parent's.
 *
 * Note: "typedef struct DynamicFileList DynamicFileList" appears in fmgr.h.
 */
struct DynamicFileList {
  dev_t device;          /* Device file is on */
#ifndef WIN32            /* ensures we never again depend on this under        \
                          * win32 */
  ino_t inode;           /* Inode number of file */
#endif
  void *handle;                         /* a handle for pg_dl* functions */
  const Pg_magic_struct *magic;         /* Location of module's magic block */
  char filename[FLEXIBLE_ARRAY_MEMBER]; /* Full pathname of file */
};

/*
 * This session's loaded files, oldest first: tcb->dfmgr.  Empty: digres
 * loads no library (internal_load_library(), ISSUE.md dg issue 1).
 */
struct dfmgr_state_t {
  std::vector<DynamicFileList *> files;
};

/* This session's loaded files. */
static inline dfmgr_state_t &dfs(void) { return *tcb->dfmgr; }

/* Make a thread's empty list of loaded files. */
dfmgr_state_t *dfmgr_state_new(void) { return new dfmgr_state_t(); }

/* Copy a list for a new session: the files themselves are shared. */
dfmgr_state_t *dfmgr_state_copy(const dfmgr_state_t *state) {
  return new dfmgr_state_t(*state);
}

/* Free a thread's list, with its control block; not the files. */
void dfmgr_state_free_t::operator()(dfmgr_state_t *state) const {
  delete state;
}

static void *internal_load_library(const char *libname);
static char *expand_dynamic_library_name(const char *name);
static void check_restricted_library_name(const char *name);

/*
 * Load the specified dynamic-link library file, and look for a function
 * named funcname in it.
 *
 * If the function is not found, we raise an error if signalNotFound is true,
 * else return NULL.  Note that errors in loading the library
 * will provoke ereport() regardless of signalNotFound.
 *
 * If filehandle is not NULL, then *filehandle will be set to a handle
 * identifying the library file.  The filehandle can be used with
 * lookup_external_function to lookup additional functions in the same file
 * at less cost than repeating load_external_function.
 */
void *load_external_function(const char *filename, const char *funcname,
                             bool signalNotFound, void **filehandle) {
  char *fullname;
  void *lib_handle;
  void *retval;

  /*
   * For extensions with hardcoded '$libdir/' library names, we strip the
   * prefix to allow the library search path to be used. This is done only
   * for simple names (e.g., "$libdir/foo"), not for nested paths (e.g.,
   * "$libdir/foo/bar").
   *
   * For nested paths, 'expand_dynamic_library_name' directly expands the
   * '$libdir' macro, so we leave them untouched.
   */
  if (strncmp(filename, "$libdir/", 8) == 0) {
    if (first_dir_separator(filename + 8) == NULL)
      filename += 8;
  }

  /* Expand the possibly-abbreviated filename to an exact path name */
  fullname = expand_dynamic_library_name(filename);

  /* Load the shared library, unless we already did */
  lib_handle = internal_load_library(fullname);

  /* Return handle if caller wants it */
  if (filehandle)
    *filehandle = lib_handle;

  /* Look up the function within the library. */
  retval = dlsym(lib_handle, funcname);

  if (retval == NULL && signalNotFound)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_FUNCTION),
                    errmsg("could not find function \"%s\" in file \"%s\"",
                           funcname, fullname)));

  pfree(fullname);
  return retval;
}

/*
 * This function loads a shlib file without looking up any particular
 * function in it.  If the same shlib has previously been loaded,
 * we do not load it again.
 *
 * When 'restricted' is true, only libraries in the presumed-secure
 * directory $libdir/plugins may be referenced.
 */
void load_file(const char *filename, bool restricted) {
  char *fullname;

  /* Apply security restriction if requested */
  if (restricted)
    check_restricted_library_name(filename);

  /* Expand the possibly-abbreviated filename to an exact path name */
  fullname = expand_dynamic_library_name(filename);

  /* Load the shared library, unless we already did */
  (void)internal_load_library(fullname);

  pfree(fullname);
}

/*
 * Lookup a function whose library file is already loaded.
 * Return NULL if not found.
 */
void *lookup_external_function(void *filehandle, const char *funcname) {
  return dlsym(filehandle, funcname);
}

/*
 * Load the library libname, an exact path: refused, as digres loads no
 * library until loading is redesigned (ISSUE.md dg issue 1).  Its statics
 * would be one for every session, each a thread of one process.
 */
static void *internal_load_library(const char *libname) {
  ereport(ERROR,
          (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
           errmsg("loading library \"%s\" is not supported yet", libname)));
  return NULL; /* not reached */
}

/*
 * Iterator functions to allow callers to scan the list of loaded modules.
 *
 * Note: currently, there is no special provision for dealing with changes
 * in the list while a scan is happening.  Current callers don't need it.
 */
DynamicFileList *get_first_loaded_module(void) {
  return dfs().files.empty() ? NULL : dfs().files.front();
}

DynamicFileList *get_next_loaded_module(DynamicFileList *dfptr) {
  std::vector<DynamicFileList *> &files = dfs().files;
  auto it = std::find(files.begin(), files.end(), dfptr);

  return (it == files.end() || ++it == files.end()) ? NULL : *it;
}

/*
 * Return some details about the specified module.
 *
 * Note that module_name and module_version could be returned as NULL.
 *
 * We could dispense with this function by exposing struct DynamicFileList
 * globally, but this way seems preferable.
 */
void get_loaded_module_details(DynamicFileList *dfptr,
                               const char **library_path,
                               const char **module_name,
                               const char **module_version) {
  *library_path = dfptr->filename;
  *module_name = dfptr->magic->name;
  *module_version = dfptr->magic->version;
}

/*
 * If name contains a slash, check if the file exists, if so return
 * the name.  Else (no slash) try to expand using search path (see
 * find_in_path below); if that works, return the fully
 * expanded file name.  If the previous failed, append DLSUFFIX and
 * try again.  If all fails, just return the original name.
 *
 * The result will always be freshly palloc'd.
 */
static char *expand_dynamic_library_name(const char *name) {
  bool have_slash;
  char *new_;
  char *full;

  Assert(name);

  have_slash = (first_dir_separator(name) != NULL);

  if (!have_slash) {
    full = find_in_path(name, tcb->guc.Dynamic_library_path,
                        "dynamic_library_path", "$libdir", pcb->pkglib_path);
    if (full)
      return full;
  } else {
    full = substitute_path_macro(name, "$libdir", pcb->pkglib_path);
    if (pg_file_exists(full))
      return full;
    pfree(full);
  }

  new_ = psprintf("%s%s", name, DLSUFFIX);

  if (!have_slash) {
    full = find_in_path(new_, tcb->guc.Dynamic_library_path,
                        "dynamic_library_path", "$libdir", pcb->pkglib_path);
    pfree(new_);
    if (full)
      return full;
  } else {
    full = substitute_path_macro(new_, "$libdir", pcb->pkglib_path);
    pfree(new_);
    if (pg_file_exists(full))
      return full;
    pfree(full);
  }

  /*
   * If we can't find the file, just return the string as-is. The ensuing
   * load attempt will fail and report a suitable message.
   */
  return pstrdup(name);
}

/*
 * Check a restricted library name.  It must begin with "$libdir/plugins/"
 * and there must not be any directory separators after that (this is
 * sufficient to prevent ".." style attacks).
 */
static void check_restricted_library_name(const char *name) {
  if (strncmp(name, "$libdir/plugins/", 16) != 0 ||
      first_dir_separator(name + 16) != NULL)
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("access to library \"%s\" is not allowed", name)));
}

/*
 * Substitute for any macros appearing in the given string.
 * Result is always freshly palloc'd.
 */
char *substitute_path_macro(const char *str, const char *macro,
                            const char *value) {
  const char *sep_ptr;

  Assert(str != NULL);
  Assert(macro[0] == '$');

  /* Currently, we only recognize $macro at the start of the string */
  if (str[0] != '$')
    return pstrdup(str);

  if ((sep_ptr = first_dir_separator(str)) == NULL)
    sep_ptr = str + strlen(str);

  if (strlen(macro) != sep_ptr - str || strncmp(str, macro, strlen(macro)) != 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_NAME),
                    errmsg("invalid macro name in path: %s", str)));

  return psprintf("%s%s", value, sep_ptr);
}

/*
 * Search for a file called 'basename' in the colon-separated search
 * path given.  If the file is found, the full file name
 * is returned in freshly palloc'd memory.  If the file is not found,
 * return NULL.
 *
 * path_param is the name of the parameter that path came from, for error
 * messages.
 *
 * macro and macro_val allow substituting a macro; see
 * substitute_path_macro().
 */
char *find_in_path(const char *basename, const char *path,
                   const char *path_param, const char *macro,
                   const char *macro_val) {
  const char *p;
  size_t baselen;

  Assert(basename != NULL);
  Assert(first_dir_separator(basename) == NULL);
  Assert(path != NULL);
  Assert(path_param != NULL);

  p = path;

  /*
   * If the path variable is empty, don't do a path search.
   */
  if (strlen(p) == 0)
    return NULL;

  baselen = strlen(basename);

  for (;;) {
    size_t len;
    char *piece;
    char *mangled;
    char *full;

    piece = first_path_var_separator(p);
    if (piece == p)
      ereport(ERROR, (errcode(ERRCODE_INVALID_NAME),
                      errmsg("zero-length component in parameter \"%s\"",
                             path_param)));

    if (piece == NULL)
      len = strlen(p);
    else
      len = piece - p;

    piece = (char *)palloc(len + 1);
    strlcpy(piece, p, len + 1);

    mangled = substitute_path_macro(piece, macro, macro_val);
    pfree(piece);

    canonicalize_path(mangled);

    /* only absolute paths */
    if (!is_absolute_path(mangled))
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_NAME),
               errmsg("component in parameter \"%s\" is not an absolute path",
                      path_param)));

    full = (char *)palloc(strlen(mangled) + 1 + baselen + 1);
    sprintf(full, "%s/%s", mangled, basename);
    pfree(mangled);

    elog(DEBUG3, "%s: trying \"%s\"", __func__, full);

    if (pg_file_exists(full))
      return full;

    pfree(full);

    if (p[len] == '\0')
      break;
    else
      p += len + 1;
  }

  return NULL;
}

/*
 * Find (or create) a rendezvous variable that one dynamically
 * loaded library can use to meet up with another.
 *
 * On the first call of this function for a particular varName,
 * a "rendezvous variable" is created with the given name.
 * The value of the variable is a void pointer (initially set to NULL).
 * Subsequent calls with the same varName just return the address of
 * the existing variable.  Once created, a rendezvous variable lasts
 * for the life of the process.
 *
 * Dynamically loaded libraries can use rendezvous variables
 * to find each other and share information: they just need to agree
 * on the variable name and the data it will point to.
 */
void **find_rendezvous_variable(const char *varName) {
  static thread_local /*LOCAL*/ HTAB *rendezvousHash = NULL;

  rendezvousHashEntry *hentry;
  bool found;

  /* Create a hashtable if we haven't already done so in this process */
  if (rendezvousHash == NULL) {
    HASHCTL ctl;

    ctl.keysize = NAMEDATALEN;
    ctl.entrysize = sizeof(rendezvousHashEntry);
    rendezvousHash = hash_create("Rendezvous variable hash", 16, &ctl,
                                 HASH_ELEM | HASH_STRINGS);
  }

  /* Find or create the hashtable entry for this varName */
  hentry = (rendezvousHashEntry *)hash_search(rendezvousHash, varName,
                                              HASH_ENTER, &found);

  /* Initialize to NULL if first time */
  if (!found)
    hentry->varValue = NULL;

  return &hentry->varValue;
}
