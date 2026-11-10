/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * interval.cc -- interval (TYPE.md §3.10): PostgreSQL's three
 * components, { time int64 µs, day int32, month int32 },
 * little-endian, 16 bytes. All three at their maximum is
 * infinity, all three at their minimum -infinity.
 *
 * The text is what PostgreSQL prints with IntervalStyle
 * iso_8601: P[nY][nM][nD][T[nH][nM][n[.f]S]], each field signed
 * on its own, PT0S for zero. As for the datetimes, in reads the
 * fields and accepts the text only if it prints back unchanged.
 * The order is PostgreSQL's: the components flattened at 30 days
 * to the month, which calls '1 mon' and '30 days' equal.
 */

#include "native.h"
#include "sm2.h"

#include <cstdint>
#include <limits>

namespace {

constexpr int64_t usecs_per_sec = 1000000;
constexpr int64_t usecs_per_minute = 60 * usecs_per_sec;
constexpr int64_t usecs_per_hour = 60 * usecs_per_minute;
constexpr int64_t usecs_per_day = 24 * usecs_per_hour;

/* PostgreSQL's interval typmod: a field range and a precision. */
constexpr int range_mask(int field) { return 1 << field; }
constexpr int f_month = range_mask(1), f_year = range_mask(2),
              f_day = range_mask(3), f_hour = range_mask(10),
              f_minute = range_mask(11), f_second = range_mask(12);
constexpr int full_range = 0x7FFF;
constexpr int full_precision = 0xFFFF;

/* An interval datum's components. */
struct interval_t {
  int64_t time;
  int32_t day;
  int32_t month;
};

/* True for infinity or -infinity: every component at one extreme. */
bool is_infinite(const interval_t &v) {
  using i32 = std::numeric_limits<int32_t>;
  using i64 = std::numeric_limits<int64_t>;
  return (v.month == i32::max() && v.day == i32::max() &&
          v.time == i64::max()) ||
         (v.month == i32::min() && v.day == i32::min() && v.time == i64::min());
}

/* The components of a 16-byte datum. */
interval_t load(std::string_view datum) {
  return {sm2::load_le<int64_t>(datum.substr(0, 8)),
          sm2::load_le<int32_t>(datum.substr(8, 4)),
          sm2::load_le<int32_t>(datum.substr(12, 4))};
}

/* Replace datum with v's 16 bytes. */
void store(const interval_t &v, std::string &datum) {
  std::string part;
  sm2::store_le(v.time, datum);
  sm2::store_le(v.day, part);
  datum += part;
  sm2::store_le(v.month, part);
  datum += part;
}

/* Append a nonzero field and its unit, as AddISO8601IntPart does. */
void put_part(std::string &out, int64_t v, char unit) {
  if (v != 0) {
    out += std::to_string(v);
    out += unit;
  }
}

/* Print an interval as PostgreSQL does under IntervalStyle iso_8601. */
std::string interval_text(const interval_t &v) {
  if (is_infinite(v)) {
    return v.time > 0 ? "infinity" : "-infinity";
  }
  // Split as interval2itm does, every part truncated toward zero.
  int64_t year = v.month / 12, mon = v.month % 12, mday = v.day;
  int64_t t = v.time;
  int64_t hour = t / usecs_per_hour;
  t -= hour * usecs_per_hour;
  int64_t min = t / usecs_per_minute;
  t -= min * usecs_per_minute;
  int64_t sec = t / usecs_per_sec, fsec = t - sec * usecs_per_sec;
  if (year == 0 && mon == 0 && mday == 0 && hour == 0 && min == 0 && sec == 0 &&
      fsec == 0) {
    return "PT0S";
  }
  std::string text = "P";
  put_part(text, year, 'Y');
  put_part(text, mon, 'M');
  put_part(text, mday, 'D');
  if (hour != 0 || min != 0 || sec != 0 || fsec != 0) {
    text += 'T';
  }
  put_part(text, hour, 'H');
  put_part(text, min, 'M');
  // Seconds carry one sign for the whole value, fraction included.
  if (sec != 0 || fsec != 0) {
    if (sec < 0 || fsec < 0) {
      text += '-';
    }
    text += std::to_string(sec < 0 ? -sec : sec);
    if (fsec != 0) {
      std::string f = std::to_string(fsec < 0 ? -fsec : fsec);
      f.insert(0, 6 - f.size(), '0');
      text += '.';
      text += f.substr(0, f.find_last_not_of('0') + 1);
    }
    text += 'S';
  }
  return text;
}

/* A reader of ISO 8601 interval fields. */
struct cursor_t {
  std::string_view text;
  size_t pos = 0;

  /* The next character, or NUL at the end. */
  char peek() const { return pos < text.size() ? text[pos] : '\0'; }

  /*
   * Take [-]digits[.digits] followed by a unit; the fraction only
   * when frac is given, as up to six digits in microseconds.
   * Returns the unit, or NUL if there is no field here.
   */
  char field(__int128 &whole, int64_t *frac, bool &neg) {
    size_t start = pos;
    neg = peek() == '-';
    pos += neg;
    size_t digits = 0;
    whole = 0;
    while (peek() >= '0' && peek() <= '9' && digits < 20) {
      whole = whole * 10 + (text[pos++] - '0');
      digits++;
    }
    if (frac) {
      *frac = 0;
      if (peek() == '.') {
        pos++;
        int n = 0;
        while (peek() >= '0' && peek() <= '9' && n < 6) {
          *frac = *frac * 10 + (text[pos++] - '0');
          n++;
        }
        for (; n < 6; n++) {
          *frac *= 10;
        }
      }
    }
    char unit = peek();
    if (digits == 0 || unit < 'A' || unit > 'Z') {
      pos = start;
      return '\0';
    }
    pos++;
    return unit;
  }
};

/*
 * Read PostgreSQL's ISO 8601 interval text into v. The fields
 * need only be well formed and in range here: the round trip
 * decides whether the text is canonical.
 */
bool parse(std::string_view text, interval_t &v) {
  cursor_t c{text};
  if (c.peek() != 'P') {
    return false;
  }
  c.pos++;
  __int128 months = 0, days = 0, time = 0, n;
  bool neg;
  // The date part: years, months, days, in that order.
  const char *date_units = "YMD";
  while (c.peek() != 'T' && c.peek() != '\0') {
    char unit = c.field(n, nullptr, neg);
    const char *at = unit ? strchr(date_units, unit) : nullptr;
    if (!at) {
      return false;
    }
    date_units = at + 1;
    n = neg ? -n : n;
    if (unit == 'Y') {
      months += n * 12;
    } else if (unit == 'M') {
      months += n;
    } else {
      days = n;
    }
  }
  // The time part: hours, minutes, seconds, in that order.
  if (c.peek() == 'T') {
    c.pos++;
    const char *time_units = "HMS";
    while (c.peek() != '\0') {
      int64_t frac;
      char unit = c.field(n, &frac, neg);
      const char *at = unit ? strchr(time_units, unit) : nullptr;
      if (!at || (unit != 'S' && frac != 0)) {
        return false;
      }
      time_units = at + 1;
      __int128 usecs = unit == 'H'   ? n * usecs_per_hour
                       : unit == 'M' ? n * usecs_per_minute
                                     : n * usecs_per_sec + frac;
      time += neg ? -usecs : usecs;
    }
  }
  // Each component must fit its field.
  if (months < std::numeric_limits<int32_t>::min() ||
      months > std::numeric_limits<int32_t>::max() ||
      days < std::numeric_limits<int32_t>::min() ||
      days > std::numeric_limits<int32_t>::max() ||
      time < std::numeric_limits<int64_t>::min() ||
      time > std::numeric_limits<int64_t>::max()) {
    return false;
  }
  v = {static_cast<int64_t>(time), static_cast<int32_t>(days),
       static_cast<int32_t>(months)};
  return true;
}

/*
 * Whether typmod leaves v as it is, or -1 if typmod is not valid:
 * PostgreSQL's AdjustIntervalForTypmod zeroes the fields below the
 * range's last one and rounds the seconds to the precision, and
 * canonical text is text it would not change. Infinity it leaves
 * alone.
 */
int typmod_keeps(int32_t typmod, const interval_t &v) {
  if (typmod == -1) {
    return 1;
  }
  int range = (typmod >> 16) & 0x7FFF, precision = typmod & 0xFFFF;
  if (typmod < 0 || (precision != full_precision && precision > 6)) {
    return -1;
  }
  // The unit the range keeps below its last field, as a divisor
  // of time; 0 for no time at all.
  int64_t unit;
  bool days = true, months = true;
  if (range == full_range || range == f_second ||
      range == (f_day | f_hour | f_minute | f_second) ||
      range == (f_hour | f_minute | f_second) ||
      range == (f_minute | f_second)) {
    unit = 1;
  } else if (range == f_hour || range == (f_day | f_hour)) {
    unit = usecs_per_hour;
  } else if (range == f_minute || range == (f_day | f_hour | f_minute) ||
             range == (f_hour | f_minute)) {
    unit = usecs_per_minute;
  } else if (range == f_day) {
    unit = 0;
  } else if (range == f_month || range == (f_year | f_month)) {
    unit = 0;
    days = false;
  } else if (range == f_year) {
    unit = 0;
    days = false;
    months = false;
  } else {
    return -1;
  }
  if (is_infinite(v)) {
    return 1;
  }
  // The seconds' precision, as a divisor of time.
  int64_t scale = 1;
  if (precision != full_precision) {
    for (int i = precision; i < 6; i++) {
      scale *= 10;
    }
  }
  bool kept = (unit == 0 ? v.time == 0 : v.time % unit == 0) &&
              v.time % scale == 0 && (days || v.day == 0) &&
              (months || v.month % 12 == 0);
  return kept ? 1 : 0;
}

/* Parse an interval's canonical text under typmod. */
int interval_in(std::string_view text, int32_t typmod, std::string &datum) {
  interval_t v;
  if (text == "infinity" || text == "-infinity") {
    bool pos = text[0] == 'i';
    v = {pos ? std::numeric_limits<int64_t>::max()
             : std::numeric_limits<int64_t>::min(),
         pos ? std::numeric_limits<int32_t>::max()
             : std::numeric_limits<int32_t>::min(),
         pos ? std::numeric_limits<int32_t>::max()
             : std::numeric_limits<int32_t>::min()};
  } else if (!parse(text, v) || interval_text(v) != text) {
    // Canonical only if it prints back unchanged.
    return sm2::bad_input("interval", text);
  }
  int kept = typmod_keeps(typmod, v);
  if (kept < 0) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "invalid typmod for type interval: " +
                           std::to_string(typmod));
  }
  if (kept == 0) {
    return sm2::bad_input("interval", text);
  }
  store(v, datum);
  return 0;
}

/* Print an interval datum; every 16 bytes are a valid interval. */
int interval_out(std::string_view datum, int32_t, std::string &text) {
  if (datum.size() != 16) {
    return sm2::bad_datum("interval", datum, 16);
  }
  text = interval_text(load(datum));
  return 0;
}

/*
 * Order two intervals as PostgreSQL's interval_cmp_value does:
 * time + (month * 30 + day) days, in 128 bits.
 */
int interval_cmp(std::string_view a, std::string_view b, int32_t) {
  auto span = [](const interval_t &v) {
    return static_cast<__int128>(v.time) +
           static_cast<__int128>(int64_t{v.month} * 30 + v.day) * usecs_per_day;
  };
  __int128 x = span(load(a)), y = span(load(b));
  return (x > y) - (x < y);
}

/*
 * An interval's key (TYPE.md §3.10): its PostgreSQL order, the
 * 128-bit span, then months and days, each sign-flipped and
 * big-endian. Months and days break PostgreSQL's ties, as between
 * '1 mon' and '30 days', and with the span they give back the
 * time, so the key is injective.
 */
int interval_key(std::string_view datum, int32_t, const sm2::collation_t *,
                 std::string &key) {
  if (datum.size() != 16) {
    return sm2::bad_datum("interval", datum, 16);
  }
  interval_t v = load(datum);
  __int128 span =
      static_cast<__int128>(v.time) +
      static_cast<__int128>(int64_t{v.month} * 30 + v.day) * usecs_per_day;
  sm2::put_key_int<unsigned __int128>(span, key);
  sm2::put_key_int<uint32_t>(v.month, key);
  sm2::put_key_int<uint32_t>(v.day, key);
  return 0;
}

/* Read an interval's key: the time is the span less the rest. */
int interval_unkey(std::string_view &key, int32_t, const sm2::collation_t *,
                   std::string &datum) {
  __int128 span;
  int32_t month, day;
  if (!sm2::get_key_int<unsigned __int128>(key, span) ||
      !sm2::get_key_int<uint32_t>(key, month) ||
      !sm2::get_key_int<uint32_t>(key, day)) {
    return sm2::bad_key("interval");
  }
  __int128 time =
      span - static_cast<__int128>(int64_t{month} * 30 + day) * usecs_per_day;
  if (time < std::numeric_limits<int64_t>::min() ||
      time > std::numeric_limits<int64_t>::max()) {
    return sm2::bad_key("interval");
  }
  store({static_cast<int64_t>(time), day, month}, datum);
  return 0;
}

} // namespace

namespace sm2 {

const type_t type_interval = {.code = 15,
                              .name = "interval",
                              .len = 16,
                              .byval = false,
                              .align = 8,
                              .in = interval_in,
                              .out = interval_out,
                              .cmp = interval_cmp,
                              .key_encode = interval_key,
                              .key_decode = interval_unkey};

} // namespace sm2
