/*
 * datetime.cc -- date, time, timestamp and timestamptz (TYPE.md
 * §3.7-§3.9), PostgreSQL's integer representations counted from
 * its 2000-01-01 epoch, with PostgreSQL's ranges and reserved
 * infinities.
 *
 * The text is what PostgreSQL prints with DateStyle ISO and, for
 * timestamptz, TimeZone UTC. A value is parsed field by field,
 * then printed back: the text is canonical only if it comes back
 * unchanged, which refuses every other spelling -- an unpadded
 * month, February 30, a "24:00:00" in a timestamp -- without a
 * rule for each. The precision typmod of time and the timestamps
 * bounds the fraction digits the text may carry.
 */

#include "native.h"
#include "sm2.h"

#include <cstdint>
#include <limits>

namespace {

constexpr int64_t usecs_per_day = 86400000000LL;
constexpr int64_t postgres_epoch_jdate = 2451545; /* date2j(2000, 1, 1) */

/* PostgreSQL's valid ranges, as Julian days and as timestamps. */
constexpr int64_t date_min_julian = 0;          /* 4714-11-24 BC */
constexpr int64_t date_end_julian = 2147483494; /* 5874898-01-01 */
constexpr int64_t min_timestamp = -211813488000000000LL;
constexpr int64_t end_timestamp = 9223371331200000000LL;

/* The reserved infinities. */
constexpr int32_t date_noend = std::numeric_limits<int32_t>::max();
constexpr int32_t date_nobegin = std::numeric_limits<int32_t>::min();
constexpr int64_t ts_noend = std::numeric_limits<int64_t>::max();
constexpr int64_t ts_nobegin = std::numeric_limits<int64_t>::min();

/* The Julian day of a Gregorian date: PostgreSQL's date2j. */
int64_t date2j(int64_t year, int64_t month, int64_t day) {
  if (month > 2) {
    month += 1;
    year += 4800;
  } else {
    month += 13;
    year += 4799;
  }
  int64_t century = year / 100;
  int64_t julian = year * 365 - 32167;
  julian += year / 4 - century + century / 4;
  julian += 7834 * month / 256 + day;
  return julian;
}

/* The Gregorian date of a Julian day >= 0: PostgreSQL's j2date. */
void j2date(int64_t jd, int &year, int &month, int &day) {
  uint32_t julian = static_cast<uint32_t>(jd) + 32044;
  uint32_t quad = julian / 146097;
  uint32_t extra = (julian - quad * 146097) * 4 + 3;
  julian += 60 + quad * 3 + extra / 146097;
  quad = julian / 1461;
  julian -= quad * 1461;
  int y = julian * 4 / 1461;
  julian = ((y != 0) ? ((julian + 305) % 365) : ((julian + 306) % 366)) + 123;
  y += quad * 4;
  year = y - 4800;
  quad = julian * 2141 / 65536;
  day = julian - 7834 * quad / 256;
  month = (quad + 10) % 12 + 1;
}

/* Append v in decimal, zero-padded to width digits. */
void put_padded(std::string &out, int64_t v, int width) {
  std::string d = std::to_string(v);
  if (d.size() < static_cast<size_t>(width)) {
    out.append(width - d.size(), '0');
  }
  out += d;
}

/*
 * Append a Julian day's date as YYYY-MM-DD, the year at least
 * four digits; set bc when the year is before 1 AD, which ISO
 * prints as a trailing " BC".
 */
void put_date(std::string &out, int64_t jd, bool &bc) {
  int y, m, d;
  j2date(jd, y, m, d);
  bc = y <= 0;
  put_padded(out, bc ? 1 - y : y, 4);
  out += '-';
  put_padded(out, m, 2);
  out += '-';
  put_padded(out, d, 2);
}

/*
 * Append a time of day in microseconds as HH:MM:SS, with the
 * fraction, if any, to at most six digits and no trailing zeros.
 */
void put_time(std::string &out, int64_t usecs) {
  put_padded(out, usecs / 3600000000LL, 2);
  out += ':';
  put_padded(out, usecs / 60000000 % 60, 2);
  out += ':';
  put_padded(out, usecs / 1000000 % 60, 2);
  int64_t frac = usecs % 1000000;
  if (frac != 0) {
    std::string f;
    put_padded(f, frac, 6);
    out += '.';
    out += f.substr(0, f.find_last_not_of('0') + 1);
  }
}

/* Print a timestamp; with tz, as timestamptz in UTC. */
std::string timestamp_text(int64_t t, bool tz) {
  if (t == ts_noend) {
    return "infinity";
  }
  if (t == ts_nobegin) {
    return "-infinity";
  }
  // Split into a day and a time of day, rounding toward minus
  // infinity, as PostgreSQL's timestamp2tm does.
  int64_t days = t / usecs_per_day, usecs = t % usecs_per_day;
  if (usecs < 0) {
    usecs += usecs_per_day;
    days--;
  }
  std::string text;
  bool bc;
  put_date(text, days + postgres_epoch_jdate, bc);
  text += ' ';
  put_time(text, usecs);
  if (tz) {
    text += "+00";
  }
  if (bc) {
    text += " BC";
  }
  return text;
}

/* Print a date. */
std::string date_text(int32_t d) {
  if (d == date_noend) {
    return "infinity";
  }
  if (d == date_nobegin) {
    return "-infinity";
  }
  std::string text;
  bool bc;
  put_date(text, d + postgres_epoch_jdate, bc);
  if (bc) {
    text += " BC";
  }
  return text;
}

/* A reader of text, one field at a time. */
struct cursor_t {
  std::string_view text;
  size_t pos = 0;

  /* Take the literal s, if it is next. */
  bool lit(std::string_view s) {
    if (text.substr(pos).starts_with(s)) {
      pos += s.size();
      return true;
    }
    return false;
  }

  /* Take min..max digits as a number. */
  bool num(size_t min, size_t max, int64_t &v) {
    size_t n = 0;
    v = 0;
    while (pos < text.size() && n < max && text[pos] >= '0' &&
           text[pos] <= '9') {
      v = v * 10 + (text[pos++] - '0');
      n++;
    }
    return n >= min;
  }

  /* True once every character is taken. */
  bool done() const { return pos == text.size(); }
};

/*
 * Read YYYY-MM-DD. The fields need only be plausible here: the
 * round trip decides whether they are canonical.
 */
bool get_ymd(cursor_t &c, int64_t &y, int64_t &m, int64_t &d) {
  return c.num(1, 7, y) && c.lit("-") && c.num(1, 2, m) && c.lit("-") &&
         c.num(1, 2, d) && y != 0 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

/* The Julian day of y-m-d, y counted BC when bc. */
int64_t julian(int64_t y, int64_t m, int64_t d, bool bc) {
  return date2j(bc ? 1 - y : y, m, d);
}

/* Read HH:MM:SS[.ffffff] as microseconds since midnight. */
bool get_time(cursor_t &c, int64_t &usecs) {
  int64_t h, m, s, frac = 0;
  if (!c.num(1, 2, h) || !c.lit(":") || !c.num(1, 2, m) || !c.lit(":") ||
      !c.num(1, 2, s)) {
    return false;
  }
  if (c.lit(".")) {
    size_t start = c.pos;
    if (!c.num(1, 6, frac)) {
      return false;
    }
    for (size_t n = c.pos - start; n < 6; n++) {
      frac *= 10;
    }
  }
  usecs = ((h * 60 + m) * 60 + s) * 1000000 + frac;
  return true;
}

/* Fail with SM_ERR_INVALID: a datum lies outside type's range. */
int bad_range(const char *type) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string("a ") + type + " datum is out of range");
}

/* Fail with SM_ERR_INVALID: typmod is not -1 or a precision 0..6. */
int bad_typmod(const char *type, int32_t typmod) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string("invalid typmod for type ") + type + ": " +
                         std::to_string(typmod));
}

/*
 * The fraction digits canonical text may carry under typmod: 6
 * for -1, else the precision; -1 if typmod is not valid.
 */
int max_digits(int32_t typmod) {
  if (typmod == -1) {
    return 6;
  }
  return typmod >= 0 && typmod <= 6 ? typmod : -1;
}

/* The fraction digits canonical text carries. */
int frac_digits(std::string_view text) {
  size_t dot = text.find('.');
  if (dot == std::string_view::npos) {
    return 0;
  }
  size_t end = dot + 1;
  while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
    end++;
  }
  return static_cast<int>(end - dot - 1);
}

/* Parse a date's canonical text. */
int date_in(std::string_view text, int32_t, std::string &datum) {
  return sm2::catch_nomem([&] {
    int32_t d;
    if (text == "infinity") {
      d = date_noend;
    } else if (text == "-infinity") {
      d = date_nobegin;
    } else {
      // Read the fields, move a BC year, and check the range.
      cursor_t c{text};
      int64_t y, m, dd;
      if (!get_ymd(c, y, m, dd)) {
        return sm2::bad_input("date", text);
      }
      int64_t jd = julian(y, m, dd, c.lit(" BC"));
      if (!c.done() || jd < date_min_julian || jd >= date_end_julian) {
        return sm2::bad_input("date", text);
      }
      d = static_cast<int32_t>(jd - postgres_epoch_jdate);
      // Canonical only if it prints back unchanged.
      if (date_text(d) != text) {
        return sm2::bad_input("date", text);
      }
    }
    sm2::store_le(d, datum);
    return 0;
  });
}

/* Print a date datum; a value outside PostgreSQL's range is invalid. */
int date_out(std::string_view datum, int32_t, std::string &text) {
  return sm2::catch_nomem([&] {
    if (datum.size() != 4) {
      return sm2::bad_datum("date", datum, 4);
    }
    int32_t d = sm2::load_le<int32_t>(datum);
    int64_t jd = int64_t{d} + postgres_epoch_jdate;
    if (d != date_noend && d != date_nobegin &&
        (jd < date_min_julian || jd >= date_end_julian)) {
      return bad_range("date");
    }
    text = date_text(d);
    return 0;
  });
}

/* Parse a time's canonical text, at most typmod fraction digits. */
int time_in(std::string_view text, int32_t typmod, std::string &datum) {
  return sm2::catch_nomem([&] {
    int digits = max_digits(typmod);
    if (digits < 0) {
      return bad_typmod("time", typmod);
    }
    cursor_t c{text};
    int64_t usecs;
    if (!get_time(c, usecs) || !c.done() || usecs > usecs_per_day) {
      return sm2::bad_input("time", text);
    }
    // Canonical only if it prints back unchanged.
    std::string back;
    put_time(back, usecs);
    if (back != text || frac_digits(text) > digits) {
      return sm2::bad_input("time", text);
    }
    sm2::store_le(usecs, datum);
    return 0;
  });
}

/* Print a time datum: 00:00:00 through 24:00:00. */
int time_out(std::string_view datum, int32_t, std::string &text) {
  return sm2::catch_nomem([&] {
    if (datum.size() != 8) {
      return sm2::bad_datum("time", datum, 8);
    }
    int64_t usecs = sm2::load_le<int64_t>(datum);
    if (usecs < 0 || usecs > usecs_per_day) {
      return bad_range("time");
    }
    text.clear();
    put_time(text, usecs);
    return 0;
  });
}

/*
 * Parse a timestamp's canonical text; with tz, a timestamptz's,
 * which carries "+00" after the time.
 */
int timestamp_in(const char *type, bool tz, std::string_view text,
                 int32_t typmod, std::string &datum) {
  int digits = max_digits(typmod);
  if (digits < 0) {
    return bad_typmod(type, typmod);
  }
  int64_t t;
  if (text == "infinity") {
    t = ts_noend;
  } else if (text == "-infinity") {
    t = ts_nobegin;
  } else {
    // Read the fields, move a BC year, and check the range.
    cursor_t c{text};
    int64_t y, m, d, usecs;
    if (!get_ymd(c, y, m, d) || !c.lit(" ") || !get_time(c, usecs) ||
        (tz && !c.lit("+00"))) {
      return sm2::bad_input(type, text);
    }
    int64_t jd = julian(y, m, d, c.lit(" BC"));
    if (!c.done() || jd < date_min_julian || jd >= date_end_julian) {
      return sm2::bad_input(type, text);
    }
    // The day and time, in 128 bits, since a far year overflows.
    __int128 wide =
        static_cast<__int128>(jd - postgres_epoch_jdate) * usecs_per_day +
        usecs;
    if (wide < min_timestamp || wide >= end_timestamp) {
      return sm2::bad_input(type, text);
    }
    t = static_cast<int64_t>(wide);
    // Canonical only if it prints back unchanged.
    if (timestamp_text(t, tz) != text || frac_digits(text) > digits) {
      return sm2::bad_input(type, text);
    }
  }
  sm2::store_le(t, datum);
  return 0;
}

/* Print a timestamp datum; with tz, as a timestamptz in UTC. */
int timestamp_out(const char *type, bool tz, std::string_view datum,
                  std::string &text) {
  if (datum.size() != 8) {
    return sm2::bad_datum(type, datum, 8);
  }
  int64_t t = sm2::load_le<int64_t>(datum);
  if (t != ts_noend && t != ts_nobegin &&
      (t < min_timestamp || t >= end_timestamp)) {
    return bad_range(type);
  }
  text = timestamp_text(t, tz);
  return 0;
}

/* Order two datums of the integer type T. */
template <typename T> int int_order(std::string_view a, std::string_view b) {
  T x = sm2::load_le<T>(a), y = sm2::load_le<T>(b);
  return (x > y) - (x < y);
}

/* Order two dates. */
int date_cmp(std::string_view a, std::string_view b, int32_t) {
  return int_order<int32_t>(a, b);
}

/* Order two times, timestamps or timestamptzs. */
int usecs_cmp(std::string_view a, std::string_view b, int32_t) {
  return int_order<int64_t>(a, b);
}

/* Parse a timestamp's canonical text. */
int ts_in(std::string_view t, int32_t typmod, std::string &d) {
  return sm2::catch_nomem(
      [&] { return timestamp_in("timestamp", false, t, typmod, d); });
}
/* Print a timestamp datum. */
int ts_out(std::string_view d, int32_t, std::string &t) {
  return sm2::catch_nomem(
      [&] { return timestamp_out("timestamp", false, d, t); });
}

/* Parse a timestamptz's canonical text, in UTC. */
int tstz_in(std::string_view t, int32_t typmod, std::string &d) {
  return sm2::catch_nomem(
      [&] { return timestamp_in("timestamptz", true, t, typmod, d); });
}
/* Print a timestamptz datum, in UTC. */
int tstz_out(std::string_view d, int32_t, std::string &t) {
  return sm2::catch_nomem(
      [&] { return timestamp_out("timestamptz", true, d, t); });
}

} // namespace

namespace sm2 {

const type_t type_date = {.code = 11,
                          .name = "date",
                          .len = 4,
                          .byval = true,
                          .align = 4,
                          .in = date_in,
                          .out = date_out,
                          .cmp = date_cmp};

const type_t type_time = {.code = 12,
                          .name = "time",
                          .len = 8,
                          .byval = true,
                          .align = 8,
                          .in = time_in,
                          .out = time_out,
                          .cmp = usecs_cmp};

const type_t type_timestamp = {.code = 13,
                               .name = "timestamp",
                               .len = 8,
                               .byval = true,
                               .align = 8,
                               .in = ts_in,
                               .out = ts_out,
                               .cmp = usecs_cmp};

const type_t type_timestamptz = {.code = 14,
                                 .name = "timestamptz",
                                 .len = 8,
                                 .byval = true,
                                 .align = 8,
                                 .in = tstz_in,
                                 .out = tstz_out,
                                 .cmp = usecs_cmp};

} // namespace sm2
