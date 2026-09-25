/*
 * collation.cc -- collations for text and varchar keys, through
 * ICU (SM2_DESIGN.md §3.3): opened by locale, carrying ICU's
 * collator version, and refused when that version is not the one
 * the caller recorded, since keys built under another version may
 * not sort the same.
 *
 * ICU's documentation has const operations on a collator, the
 * sort key among them, safe from several threads at once, so one
 * collation serves every session; sm2 relies on that without
 * having checked it against ICU's source.
 */

#include "collation.h"
#include "native.h"

#include <unicode/uloc.h>
#include <unicode/ustring.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

/* Fail with SM_ERR_INVALID, naming the collation's locale. */
int bad_collation(std::string_view locale, const std::string &why) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     "collation \"" + std::string(locale) + "\": " + why);
}

/*
 * True if ICU knows the language of the locale ID id, as
 * PostgreSQL's icu_validate_locale checks: the root language
 * ("", "root", "und") or one among ICU's available locales. ICU
 * opens any locale, falling back to its root order for a language
 * it does not know, so opening proves nothing; and a known one may
 * still open with that fallback, as English does.
 */
bool known_language(const char *id) {
  char lang[ULOC_LANG_CAPACITY];
  UErrorCode status = U_ZERO_ERROR;
  uloc_getLanguage(id, lang, sizeof(lang), &status);
  if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING) {
    return false;
  }
  if (strcmp(lang, "") == 0 || strcmp(lang, "root") == 0 ||
      strcmp(lang, "und") == 0) {
    return true;
  }
  for (int32_t i = 0; i < uloc_countAvailable(); i++) {
    char other[ULOC_LANG_CAPACITY];
    status = U_ZERO_ERROR;
    uloc_getLanguage(uloc_getAvailable(i), other, sizeof(other), &status);
    if (U_SUCCESS(status) && status != U_STRING_NOT_TERMINATED_WARNING &&
        strcmp(lang, other) == 0) {
      return true;
    }
  }
  return false;
}

/* ICU's collator version, as dotted decimal. */
std::string icu_version(const UCollator *icu) {
  UVersionInfo v;
  ucol_getVersion(icu, v);
  char buf[U_MAX_VERSION_STRING_LENGTH];
  u_versionToString(v, buf);
  return buf;
}

} // namespace

namespace sm2 {

/* Open a collation by locale; see sm2.h. */
int collation_open(std::string_view locale, bool deterministic,
                   std::string_view expected_version, collation_t *&out) {
  return catch_nomem([&] {
    // Byte order needs no ICU, and never changes.
    if (locale == "C" || locale == "POSIX") {
      if (!deterministic) {
        return bad_collation(locale, "byte order is always deterministic");
      }
      if (!expected_version.empty()) {
        return bad_collation(locale, "byte order has no version");
      }
      out = new collation_t{std::string(locale), "", nullptr, true};
      return 0;
    }
    // A BCP 47 tag, as PostgreSQL's ICU collations take, into
    // ICU's own locale ID.
    char id[ULOC_FULLNAME_CAPACITY];
    UErrorCode status = U_ZERO_ERROR;
    std::string tag(locale);
    int32_t parsed = 0;
    uloc_forLanguageTag(tag.c_str(), id, sizeof(id), &parsed, &status);
    if (U_FAILURE(status) || status == U_STRING_NOT_TERMINATED_WARNING ||
        parsed != static_cast<int32_t>(tag.size())) {
      return bad_collation(locale, "not a BCP 47 language tag");
    }
    // Refuse a language ICU does not know, rather than sorting it
    // by ICU's root order unasked.
    if (!known_language(id)) {
      return bad_collation(locale, "ICU does not know its language");
    }
    // Open the collator.
    status = U_ZERO_ERROR;
    UCollator *icu = ucol_open(id, &status);
    if (U_FAILURE(status)) {
      return bad_collation(locale, u_errorName(status));
    }
    // Refuse a version other than the one the keys were built under.
    std::string version = icu_version(icu);
    if (!expected_version.empty() && expected_version != version) {
      ucol_close(icu);
      return bad_collation(locale, "keys were built under ICU collation "
                                   "version " +
                                       std::string(expected_version) +
                                       ", but this ICU has " + version);
    }
    try {
      out = new collation_t{std::string(locale), version, icu, deterministic};
    } catch (...) {
      ucol_close(icu);
      throw;
    }
    return 0;
  });
}

/* Close a collation from collation_open; nullptr is a no-op. */
void collation_close(collation_t *collation) {
  if (collation == nullptr) {
    return;
  }
  // Release ICU's collator.
  if (collation->icu != nullptr) {
    ucol_close(collation->icu);
  }
  delete collation;
}

/* The collation's ICU version, "" for byte order. */
std::string_view collation_version(const collation_t *collation) {
  return collation->version;
}

/* Append s's ICU sort key, without its terminating zero. */
int collation_sortkey(const collation_t *collation, std::string_view s,
                      std::string &key) {
  // UTF-8 into the UTF-16 ICU sorts.
  UErrorCode status = U_ZERO_ERROR;
  int32_t n16 = 0;
  u_strFromUTF8(nullptr, 0, &n16, s.data(), static_cast<int32_t>(s.size()),
                &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    return bad_collation(collation->locale, "text is not valid UTF-8");
  }
  std::vector<UChar> u16(n16 > 0 ? n16 : 1);
  status = U_ZERO_ERROR;
  u_strFromUTF8(u16.data(), n16, nullptr, s.data(),
                static_cast<int32_t>(s.size()), &status);
  if (U_FAILURE(status)) {
    return bad_collation(collation->locale, "text is not valid UTF-8");
  }
  // The sort key, sized by a first call when it does not fit.
  size_t start = key.size();
  key.resize(start + 2 * s.size() + 16);
  int32_t room = static_cast<int32_t>(key.size() - start);
  auto sortkey = [&] {
    return ucol_getSortKey(collation->icu, u16.data(), n16,
                           reinterpret_cast<uint8_t *>(key.data() + start),
                           room);
  };
  int32_t len = sortkey();
  if (len > room) {
    key.resize(start + len);
    room = len;
    len = sortkey();
  }
  if (len <= 0) {
    key.resize(start);
    return bad_collation(collation->locale, "ICU made no sort key");
  }
  // Drop ICU's terminating zero byte.
  key.resize(start + len - 1);
  return 0;
}

} // namespace sm2
