// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   https://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#if !defined(HAS_STRPTIME)
# if !defined(_MSC_VER) && !defined(__MINGW32__)
#  define HAS_STRPTIME 1  // assume everyone has strptime() except windows
# endif
#endif

#if defined(HAS_STRPTIME) && HAS_STRPTIME
# if !defined(_XOPEN_SOURCE)
#  define _XOPEN_SOURCE  // Definedness suffices for strptime.
# endif
#endif

#include "cctz/time_zone.h"

// Include time.h directly since, by C++ standards, ctime doesn't have to
// declare strptime.
#include <time.h>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <string>
#include <vector>
#if !HAS_STRPTIME
#include <iomanip>
#include <sstream>
#endif

#include "cctz/civil_time.h"
#include "time_zone_if.h"

namespace cctz {
namespace detail {

namespace {

#if !HAS_STRPTIME
// Build a strptime() using C++11's std::get_time().
char* strptime(const char* s, const char* fmt, std::tm* tm) {
  std::istringstream input(s);
  input >> std::get_time(tm, fmt);
  if (input.fail()) return nullptr;
  return const_cast<char*>(s) +
         (input.eof() ? strlen(s) : static_cast<std::size_t>(input.tellg()));
}
#endif

std::tm ToTM(const time_zone::absolute_lookup& al) {
  std::tm tm{};
  tm.tm_sec = al.cs.second();
  tm.tm_min = al.cs.minute();
  tm.tm_hour = al.cs.hour();
  tm.tm_mday = al.cs.day();
  tm.tm_mon = al.cs.month() - 1;

  // Saturate tm.tm_year is cases of over/underflow.
  if (al.cs.year() < std::numeric_limits<int>::min() + 1900) {
    tm.tm_year = std::numeric_limits<int>::min();
  } else if (al.cs.year() - 1900 > std::numeric_limits<int>::max()) {
    tm.tm_year = std::numeric_limits<int>::max();
  } else {
    tm.tm_year = static_cast<int>(al.cs.year() - 1900);
  }

  switch (get_weekday(al.cs)) {
    case weekday::sunday:
      tm.tm_wday = 0;
      break;
    case weekday::monday:
      tm.tm_wday = 1;
      break;
    case weekday::tuesday:
      tm.tm_wday = 2;
      break;
    case weekday::wednesday:
      tm.tm_wday = 3;
      break;
    case weekday::thursday:
      tm.tm_wday = 4;
      break;
    case weekday::friday:
      tm.tm_wday = 5;
      break;
    case weekday::saturday:
      tm.tm_wday = 6;
      break;
  }
  tm.tm_yday = get_yearday(al.cs) - 1;
  tm.tm_isdst = al.is_dst ? 1 : 0;
  return tm;
}

const char kDigits[] = "0123456789";

// Formats a 64-bit integer in the given field width.  Note that it is up
// to the caller of Format64() [and Format02d()/FormatOffset()] to ensure
// that there is sufficient space before ep to hold the conversion.
char* Format64(char* ep, int width, std::int_fast64_t v) {
  bool neg = false;
  if (v < 0) {
    --width;
    neg = true;
    if (v == std::numeric_limits<std::int_fast64_t>::min()) {
      // Avoid negating minimum value.
      std::int_fast64_t last_digit = -(v % 10);
      v /= 10;
      if (last_digit < 0) {
        ++v;
        last_digit += 10;
      }
      --width;
      *--ep = kDigits[last_digit];
    }
    v = -v;
  }
  do {
    --width;
    *--ep = kDigits[v % 10];
  } while (v /= 10);
  while (--width >= 0) *--ep = '0';  // zero pad
  if (neg) *--ep = '-';
  return ep;
}

// Formats [0 .. 99] as %02d.
char* Format02d(char* ep, int v) {
  *--ep = kDigits[v % 10];
  *--ep = kDigits[(v / 10) % 10];
  return ep;
}

// Formats a UTC offset, like +00:00.
char* FormatOffset(char* ep, int offset, const char* mode) {
  // TODO: Follow the RFC3339 "Unknown Local Offset Convention" and
  // generate a "negative zero" when we're formatting a zero offset
  // as the result of a failed load_time_zone().
  char sign = '+';
  if (offset < 0) {
    offset = -offset;  // bounded by 24h so no overflow
    sign = '-';
  }
  const int seconds = offset % 60;
  const int minutes = (offset /= 60) % 60;
  const int hours = offset /= 60;
  const char sep = mode[0];
  const bool ext = (sep != '\0' && mode[1] == '*');
  const bool ccc = (ext && mode[2] == ':');
  if (ext && (!ccc || seconds != 0)) {
    ep = Format02d(ep, seconds);
    *--ep = sep;
  } else {
    // If we're not rendering seconds, sub-minute negative offsets
    // should get a positive sign (e.g., offset=-10s => "+00:00").
    if (hours == 0 && minutes == 0) sign = '+';
  }
  if (!ccc || minutes != 0 || seconds != 0) {
    ep = Format02d(ep, minutes);
    if (sep != '\0') *--ep = sep;
  }
  ep = Format02d(ep, hours);
  *--ep = sign;
  return ep;
}

// Formats a std::tm using strftime(3).
void FormatTM(std::string* out, char_range fmt_range, const std::tm& tm) {
  // strftime(3) returns the number of characters placed in the output
  // array (which may be 0 characters).  It also returns 0 to indicate
  // an error, like the array wasn't large enough.  To accommodate this,
  // the following code grows the buffer size from 2x the format string
  // length up to 32x.
  std::string fmt(fmt_range.begin, fmt_range.end);
  for (std::size_t i = 2; i != 32; i *= 2) {
    std::size_t buf_size = fmt.size() * i;
    std::vector<char> buf(buf_size);
    if (std::size_t len = strftime(&buf[0], buf_size, fmt.c_str(), &tm)) {
      out->append(&buf[0], len);
      return;
    }
  }
}

// Used for %E#S/%E#f specifiers and for data values in parse().
template <typename T>
const char* ParseInt(char_range s, int width, T min, T max, T* vp) {
  const T kmin = std::numeric_limits<T>::min();
  bool neg = false;
  T value = 0;
  if (s.consume_prefix('-')) {
    neg = true;
    if (width == 1) return nullptr;
    if (width > 1) --width;
  }
  const char* cp = s.begin;
  while (cp != s.end) {
    int d = *cp - '0';
    if (d < 0 || 10 <= d) break;
    if (value < kmin / 10) return nullptr;
    value *= 10;
    if (value < kmin + d) return nullptr;
    value -= d;
    ++cp;
    if (width > 0 && --width == 0) break;
  }
  if (cp == s.begin) return nullptr;
  if (!neg && value == kmin) return nullptr;
  if (neg && value == 0) return nullptr;

  if (!neg) value = -value;  // make positive

  if (!(min <= value && value <= max)) {
    return nullptr;
  }
  *vp = value;
  return cp;
}

// The number of base-10 digits that can be represented by a signed 64-bit
// integer.  That is, 10^kDigits10_64 <= 2^63 - 1 < 10^(kDigits10_64 + 1).
const int kDigits10_64 = 18;

// 10^n for everything that can be represented by a signed 64-bit integer.
const std::int_fast64_t kExp10[kDigits10_64 + 1] = {
    1,
    10,
    100,
    1000,
    10000,
    100000,
    1000000,
    10000000,
    100000000,
    1000000000,
    10000000000,
    100000000000,
    1000000000000,
    10000000000000,
    100000000000000,
    1000000000000000,
    10000000000000000,
    100000000000000000,
    1000000000000000000,
};

}  // namespace

// Uses strftime(3) to format the given Time.  The following extended format
// specifiers are also supported:
//
//   - %Ez  - RFC3339-compatible numeric UTC offset (+hh:mm or -hh:mm)
//   - %E*z - Full-resolution numeric UTC offset (+hh:mm:ss or -hh:mm:ss)
//   - %E#S - Seconds with # digits of fractional precision
//   - %E*S - Seconds with full fractional precision (a literal '*')
//   - %E4Y - Four-character years (-999 ... -001, 0000, 0001 ... 9999)
//
// The standard specifiers from RFC3339_* (%Y, %m, %d, %H, %M, and %S) are
// handled internally for performance reasons.  strftime(3) is slow due to
// a POSIX requirement to respect changes to ${TZ}.
//
// The TZ/GNU %s extension is handled internally because strftime() has
// to use mktime() to generate it, and that assumes the local time zone.
//
// We also handle the %z and %Z specifiers to accommodate platforms that do
// not support the tm_gmtoff and tm_zone extensions to std::tm.
//
// Requires that zero() <= fs < seconds(1).
std::string format(char_range format, const time_point<seconds>& tp,
                   const detail::femtoseconds& fs, const time_zone& tz) {
  std::string result;
  result.reserve(format.size());  // A reasonable guess for the result size.
  const time_zone::absolute_lookup al = tz.lookup(tp);
  const std::tm tm = ToTM(al);

  // Scratch buffer for internal conversions.
  char buf[3 + kDigits10_64];  // enough for longest conversion
  char* const ep = buf + sizeof(buf);
  char* bp;  // works back from ep

  // Maintain three, disjoint subsequences that span format.
  //   [format.begin() ... pending) : already formatted into result
  //   [pending ... cur) : formatting pending, but no special cases
  //   [cur ... format.end()) : unexamined
  // Initially, everything is in the unexamined part.
  const char* pending = format.begin;
  const char* cur = format.begin;
  const char* end = format.end;

  while (cur != end) {  // while something is unexamined
    // Moves cur to the next percent sign.
    const char* start = cur;
    while (cur != end && *cur != '%') ++cur;

    // If the new pending text is all ordinary, copy it out.
    if (cur != start && pending == start) {
      result.append(pending, static_cast<std::size_t>(cur - pending));
      pending = start = cur;
    }

    // Span the sequential percent signs.
    const char* percent = cur;
    while (cur != end && *cur == '%') ++cur;

    // If the new pending text is all percents, copy out one
    // percent for every matched pair, then skip those pairs.
    if (cur != start && pending == start) {
      std::size_t escaped = static_cast<std::size_t>(cur - pending) / 2;
      result.append(pending, escaped);
      pending += escaped * 2;
      // Also copy out a single trailing percent.
      if (pending != cur && cur == end) {
        result.push_back(*pending++);
      }
    }

    // Loop unless we have an unescaped percent.
    if (cur == end || (cur - percent) % 2 == 0) continue;

    // Simple specifiers that we handle ourselves.
    if (strchr("YmdeHMSzZs%", *cur)) {
      if (cur - 1 != pending) {
        FormatTM(&result, char_range(pending, cur - 1), tm);
      }
      switch (*cur) {
        case 'Y':
          // This avoids the tm.tm_year overflow problem for %Y, however
          // tm.tm_year will still be used by other specifiers like %D.
          bp = Format64(ep, 0, al.cs.year());
          result.append(bp, static_cast<std::size_t>(ep - bp));
          break;
        case 'm':
          bp = Format02d(ep, al.cs.month());
          result.append(bp, static_cast<std::size_t>(ep - bp));
          break;
        case 'd':
        case 'e':
          bp = Format02d(ep, al.cs.day());
          if (*cur == 'e' && *bp == '0') *bp = ' ';  // for Windows
          result.append(bp, static_cast<std::size_t>(ep - bp));
          break;
        case 'H':
          bp = Format02d(ep, al.cs.hour());
          result.append(bp, static_cast<std::size_t>(ep - bp));
          break;
        case 'M':
          bp = Format02d(ep, al.cs.minute());
          result.append(bp, static_cast<std::size_t>(ep - bp));
          break;
        case 'S':
          bp = Format02d(ep, al.cs.second());
          result.append(bp, static_cast<std::size_t>(ep - bp));
          break;
        case 'z':
          bp = FormatOffset(ep, al.offset, "");
          result.append(bp, static_cast<std::size_t>(ep - bp));
          break;
        case 'Z':
          result.append(al.abbr);
          break;
        case 's':
          bp = Format64(ep, 0, ToUnixSeconds(tp));
          result.append(bp, static_cast<std::size_t>(ep - bp));
          break;
        case '%':
          result.push_back('%');
          break;
      }
      pending = ++cur;
      continue;
    }

    // More complex specifiers that we handle ourselves.
    if (*cur == ':' && cur + 1 != end) {
      if (*(cur + 1) == 'z') {
        // Formats %:z.
        if (cur - 1 != pending) {
          FormatTM(&result, char_range(pending, cur - 1), tm);
        }
        bp = FormatOffset(ep, al.offset, ":");
        result.append(bp, static_cast<std::size_t>(ep - bp));
        pending = cur += 2;
        continue;
      }
      if (*(cur + 1) == ':' && cur + 2 != end) {
        if (*(cur + 2) == 'z') {
          // Formats %::z.
          if (cur - 1 != pending) {
            FormatTM(&result, char_range(pending, cur - 1), tm);
          }
          bp = FormatOffset(ep, al.offset, ":*");
          result.append(bp, static_cast<std::size_t>(ep - bp));
          pending = cur += 3;
          continue;
        }
        if (*(cur + 2) == ':' && cur + 3 != end) {
          if (*(cur + 3) == 'z') {
            // Formats %:::z.
            if (cur - 1 != pending) {
              FormatTM(&result, char_range(pending, cur - 1), tm);
            }
            bp = FormatOffset(ep, al.offset, ":*:");
            result.append(bp, static_cast<std::size_t>(ep - bp));
            pending = cur += 4;
            continue;
          }
        }
      }
    }

    // Loop if there is no E modifier.
    if (*cur != 'E' || ++cur == end) continue;

    // Format our extensions.
    if (*cur == 'z') {
      // Formats %Ez.
      if (cur - 2 != pending) {
        FormatTM(&result, char_range(pending, cur - 2), tm);
      }
      bp = FormatOffset(ep, al.offset, ":");
      result.append(bp, static_cast<std::size_t>(ep - bp));
      pending = ++cur;
    } else if (*cur == '*' && cur + 1 != end && *(cur + 1) == 'z') {
      // Formats %E*z.
      if (cur - 2 != pending) {
        FormatTM(&result, char_range(pending, cur - 2), tm);
      }
      bp = FormatOffset(ep, al.offset, ":*");
      result.append(bp, static_cast<std::size_t>(ep - bp));
      pending = cur += 2;
    } else if (*cur == '*' && cur + 1 != end &&
               (*(cur + 1) == 'S' || *(cur + 1) == 'f')) {
      // Formats %E*S or %E*F.
      if (cur - 2 != pending) {
        FormatTM(&result, char_range(pending, cur - 2), tm);
      }
      char* cp = ep;
      bp = Format64(cp, 15, fs.count());
      while (cp != bp && cp[-1] == '0') --cp;
      switch (*(cur + 1)) {
        case 'S':
          if (cp != bp) *--bp = '.';
          bp = Format02d(bp, al.cs.second());
          break;
        case 'f':
          if (cp == bp) *--bp = '0';
          break;
      }
      result.append(bp, static_cast<std::size_t>(cp - bp));
      pending = cur += 2;
    } else if (*cur == '4' && cur + 1 != end && *(cur + 1) == 'Y') {
      // Formats %E4Y.
      if (cur - 2 != pending) {
        FormatTM(&result, char_range(pending, cur - 2), tm);
      }
      bp = Format64(ep, 4, al.cs.year());
      result.append(bp, static_cast<std::size_t>(ep - bp));
      pending = cur += 2;
    } else if (std::isdigit(*cur)) {
      // Possibly found %E#S or %E#f.
      int n = 0;
      const char* np = ParseInt(char_range(cur, end), 0, 0, 1024, &n);
      if (np == nullptr || np == end) {
        // TODO(kfm): I think this is a bug in the original
        continue;
      }
      if (*np == 'S' || *np == 'f') {
        // Formats %E#S or %E#f.
        if (cur - 2 != pending) {
          FormatTM(&result, char_range(pending, cur - 2), tm);
        }
        bp = ep;
        if (n > 0) {
          if (n > kDigits10_64) n = kDigits10_64;
          bp = Format64(bp, n,
                        (n > 15) ? fs.count() * kExp10[n - 15]
                                 : fs.count() / kExp10[15 - n]);
          if (*np == 'S') *--bp = '.';
        }
        if (*np == 'S') bp = Format02d(bp, al.cs.second());
        result.append(bp, ep);
        pending = cur = ++np;
      }
    }
  }

  // Formats any remaining data.
  if (end != pending) {
    FormatTM(&result, char_range(pending, end), tm);
  }

  return result;
}

namespace {

const char* ParseOffset(char_range data, const char* mode, int* offset) {
  if (data.begin == data.end) return nullptr;

  const char first = *data.begin++;
  if (first == 'Z') {  // Zulu
    *offset = 0;
    return data.begin;
  }
  if (first != '+' && first != '-') return nullptr;

  char sep = mode[0];
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  const char* hours_end = ParseInt(data, 2, 0, 23, &hours);
  if (hours_end == nullptr) return nullptr;
  if (hours_end - data.begin != 2) return nullptr;

  if (sep != '\0' && *hours_end == sep) ++hours_end;
  data.begin = hours_end;
  const char* minutes_end = ParseInt(data, 2, 0, 59, &minutes);

  if (minutes_end != nullptr && minutes_end - data.begin == 2) {
    if (sep != '\0' && *minutes_end == sep) ++minutes_end;
    data.begin = minutes_end;

    const char* seconds_end = ParseInt(data, 2, 0, 59, &seconds);
    if (seconds_end != nullptr && seconds_end - data.begin == 2)
      data.begin = seconds_end;
  }

  *offset = ((hours * 60 + minutes) * 60) + seconds;
  if (first == '-') *offset = -*offset;
  return data.begin;
}

const char* ParseZone(char_range data, std::string* zone) {
  zone->clear();
  const char* dp = data.begin;
  while (dp != data.end) {
    if (std::isspace(*dp)) break;
    ++dp;
  }
  if (dp == data.begin) return nullptr;
  zone->append(data.begin, dp);
  return dp;
}

const char* ParseSubSeconds(char_range s, detail::femtoseconds* subseconds) {
  std::int_fast64_t v = 0;
  std::int_fast64_t exp = 0;
  const char* cp = s.begin;
  while (cp != s.end) {
    int d = *cp - '0';
    if (d < 0 || 10 <= d) break;
    if (exp < 15) {
      exp += 1;
      v *= 10;
      v += d;
    }
    ++cp;
  }
  if (cp == s.begin) return nullptr;

  v *= kExp10[15 - exp];
  *subseconds = detail::femtoseconds(v);
  return cp;
}

}  // namespace

// Uses strptime(3) to parse the given input.  Supports the same extended
// format specifiers as format(), although %E#S and %E*S are treated
// identically (and similarly for %E#f and %E*f).  %Ez and %E*z also accept
// the same inputs.
//
// The standard specifiers from RFC3339_* (%Y, %m, %d, %H, %M, and %S) are
// handled internally so that we can normally avoid strptime() altogether
// (which is particularly helpful when the native implementation is broken).
//
// The TZ/GNU %s extension is handled internally because strptime() has to
// use localtime_r() to generate it, and that assumes the local time zone.
//
// We also handle the %z specifier to accommodate platforms that do not
// support the tm_gmtoff extension to std::tm.  %Z is parsed but ignored.
bool parse(char_range format, char_range input,
           const time_zone& tz, time_point<seconds>* sec,
           detail::femtoseconds* fs, std::string* err) {
  auto make_err = [err]() {
    if (err != nullptr) *err = "Failed to parse input";
    return false;
  };
  // To avoid repeated allocations, we keep the scratch buffer around once we
  // decide that we need to use it.
  std::string null_terminated_scratch_buffer;

  input.consume_leading_spaces();

  const year_t kyearmax = std::numeric_limits<year_t>::max();
  const year_t kyearmin = std::numeric_limits<year_t>::min();

  // Sets default values for unspecified fields.
  bool saw_year = false;
  year_t year = 1970;
  std::tm tm{};
  tm.tm_year = 1970 - 1900;
  tm.tm_mon = 1 - 1;  // Jan
  tm.tm_mday = 1;
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  tm.tm_wday = 4;  // Thu
  tm.tm_yday = 0;
  tm.tm_isdst = 0;
  auto subseconds = detail::femtoseconds::zero();
  bool saw_offset = false;
  int offset = 0;  // No offset from passed tz.
  std::string zone = "UTC";

  bool twelve_hour = false;
  bool afternoon = false;

  bool saw_percent_s = false;
  std::int_fast64_t percent_s = 0;

  // Steps through format, one specifier at a time.
  while (input.begin != input.end && format.begin != format.end) {
    if (format.consume_leading_spaces()) {
      input.consume_leading_spaces();
      continue;
    }

    if (!format.consume_prefix('%')) {
      if (*format.begin != *input.begin) return make_err();

      ++input.begin;
      ++format.begin;
      continue;
    }

    if (format.begin == format.end) return make_err();

    const char* percent = format.begin - 1;
    switch (*format.begin++) {
      case 'Y':
        // Symmetrically with FormatTime(), directly handing %Y avoids the
        // tm.tm_year overflow problem.  However, tm.tm_year will still be
        // used by other specifiers like %D.
        input.begin = ParseInt(input, 0, kyearmin, kyearmax, &year);
        if (input.begin == nullptr) return make_err();
        saw_year = true;
        continue;
      case 'm':
        input.begin = ParseInt(input, 2, 1, 12, &tm.tm_mon);
        if (input.begin == nullptr) return make_err();
        tm.tm_mon -= 1;
        continue;
      case 'd':
      case 'e':
        input.begin = ParseInt(input, 2, 1, 31, &tm.tm_mday);
        if (input.begin == nullptr) return make_err();
        continue;
      case 'H':
        input.begin = ParseInt(input, 2, 0, 23, &tm.tm_hour);
        if (input.begin == nullptr) return make_err();
        twelve_hour = false;
        continue;
      case 'M':
        input.begin = ParseInt(input, 2, 0, 59, &tm.tm_min);
        if (input.begin == nullptr) return make_err();
        continue;
      case 'S':
        input.begin = ParseInt(input, 2, 0, 60, &tm.tm_sec);
        if (input.begin == nullptr) return make_err();
        continue;
      case 'I':
      case 'l':
      case 'r':  // probably uses %I
        twelve_hour = true;
        break;
      case 'R':  // uses %H
      case 'T':  // uses %H
      case 'c':  // probably uses %H
      case 'X':  // probably uses %H
        twelve_hour = false;
        break;
      case 'z':
        input.begin = ParseOffset(input, "", &offset);
        if (input.begin == nullptr) return make_err();
        saw_offset = true;
        continue;
      case 'Z':  // ignored; zone abbreviations are ambiguous
        input.begin = ParseZone(input, &zone);
        if (input.begin == nullptr) return make_err();
        continue;
      case 's':
        input.begin = ParseInt(input, 0,
                        std::numeric_limits<std::int_fast64_t>::min(),
                        std::numeric_limits<std::int_fast64_t>::max(),
                        &percent_s);
        if (input.begin == nullptr) return make_err();
        saw_percent_s = true;
        continue;
      case ':':
        if (format.starts_with('z')) {
          format.begin += 1;
        } else if (format.starts_with(":z")) {
          format.begin += 2;
        } else if (format.starts_with("::z")) {
          format.begin += 3;
        } else {
          break;
        }
        input.begin = ParseOffset(input, ":", &offset);
        if (input.begin == nullptr) return make_err();
        saw_offset = true;
        continue;
      case '%':
        if (!input.consume_prefix('%')) return make_err();
        continue;
      case 'E':
        if (format.consume_prefix('z') || format.consume_prefix("*z")) {
          input.begin = ParseOffset(input, ":", &offset);
          if (input.begin == nullptr) return make_err();
          saw_offset = true;
          continue;
        }
        if (format.consume_prefix("*S")) {
          input.begin = ParseInt(input, 2, 0, 60, &tm.tm_sec);
          if (input.begin == nullptr) return make_err();
          if (input.consume_prefix('.')) {
            input.begin = ParseSubSeconds(input, &subseconds);
            if (input.begin == nullptr) return make_err();
          }
          continue;
        }
        if (format.consume_prefix("*f")) {
          if (std::isdigit(*input.begin)) {
            input.begin = ParseSubSeconds(input, &subseconds);
            if (input.begin == nullptr) return make_err();
          }
          continue;
        }
        if (format.consume_prefix("4Y")) {
          const char* old_pos = input.begin;
          input.begin = ParseInt(input, 4, year_t{-999}, year_t{9999}, &year);
          if (input.begin == nullptr) return make_err();
          if (input.begin - old_pos != 4) return make_err(); // stopped too soon
          saw_year = true;
          continue;
        }

        if (std::isdigit(*format.begin)) {
          int n = 0;  // value ignored
          if (const char* np = ParseInt(format.begin, 0, 0, 1024, &n)) {
            if (np != format.end) {
              if (*np == 'S') {
                input.begin = ParseInt(input, 2, 0, 60, &tm.tm_sec);
                if (input.begin == nullptr) return make_err();
                if (input.consume_prefix('.')) {
                  input.begin = ParseSubSeconds(input, &subseconds);
                  if (input.begin == nullptr) return make_err();
                }
                format.begin = ++np;
                continue;
              }
              if (*np == 'f') {
                if (std::isdigit(*input.begin)) {
                  input.begin = ParseSubSeconds(input, &subseconds);
                  if (input.begin == nullptr) return make_err();
                }
                format.begin = ++np;
                continue;
              }
            }
          }
        }
        if (*format.begin == 'c') twelve_hour = false;  // probably uses %H
        if (*format.begin == 'X') twelve_hour = false;  // probably uses %H
        if (format.begin != format.end) ++format.begin;
        break;
      case 'O':
        if (*format.begin == 'H') twelve_hour = false;
        if (*format.begin == 'I') twelve_hour = true;
        if (format.begin != format.end) ++format.begin;
        break;
    }

    // Parses the current specifier.
    const char* orig_input_pos = input.begin;
    std::string spec(percent, format.begin);
    null_terminated_scratch_buffer.assign(input.begin, input.size());
    const char* tmp_p = strptime(null_terminated_scratch_buffer.c_str(), spec.c_str(), &tm);
    if (tmp_p == nullptr) return make_err();
    input.begin += tmp_p - null_terminated_scratch_buffer.data();

    // If we successfully parsed %p we need to remember whether the result
    // was AM or PM so that we can adjust tm_hour before time_zone::lookup().
    // So reparse the input with a known AM hour, and check if it is shifted
    // to a PM hour.
    if (spec == "%p") {
      std::string test_input = "1";
      test_input.append(orig_input_pos, input.begin);
      std::tm tmp{};
      strptime(test_input.c_str(), "%I%p", &tmp);
      afternoon = (tmp.tm_hour == 13);
    }
  }

  // Adjust a 12-hour tm_hour value if it should be in the afternoon.
  if (twelve_hour && afternoon && tm.tm_hour < 12) {
    tm.tm_hour += 12;
  }

  input.consume_leading_spaces();

  // parse() must consume the entire input string.
  if (input.begin != input.end) {
    if (err != nullptr) *err = "Illegal trailing data in input string";
    return false;
  }

  // If we saw %s then we ignore anything else and return that time.
  if (saw_percent_s) {
    *sec = FromUnixSeconds(percent_s);
    *fs = detail::femtoseconds::zero();
    return true;
  }

  // If we saw %z, %Ez, or %E*z then we want to interpret the parsed fields
  // in UTC and then shift by that offset.  Otherwise we want to interpret
  // the fields directly in the passed time_zone.
  time_zone ptz = saw_offset ? utc_time_zone() : tz;

  // Allows a leap second of 60 to normalize forward to the following ":00".
  if (tm.tm_sec == 60) {
    tm.tm_sec -= 1;
    offset -= 1;
    subseconds = detail::femtoseconds::zero();
  }

  if (!saw_year) {
    year = year_t{tm.tm_year};
    if (year > kyearmax - 1900) {
      // Platform-dependent, maybe unreachable.
      if (err != nullptr) *err = "Out-of-range year";
      return false;
    }
    year += 1900;
  }

  const int month = tm.tm_mon + 1;
  civil_second cs(year, month, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

  // parse() should not allow normalization. Due to the restricted field
  // ranges above (see ParseInt()), the only possibility is for days to roll
  // into months. That is, parsing "Sep 31" should not produce "Oct 1".
  if (cs.month() != month || cs.day() != tm.tm_mday) {
    if (err != nullptr) *err = "Out-of-range field";
    return false;
  }

  // Accounts for the offset adjustment before converting to absolute time.
  if ((offset < 0 && cs > civil_second::max() + offset) ||
      (offset > 0 && cs < civil_second::min() + offset)) {
    if (err != nullptr) *err = "Out-of-range field";
    return false;
  }
  cs -= offset;

  const auto tp = ptz.lookup(cs).pre;
  // Checks for overflow/underflow and returns an error as necessary.
  if (tp == time_point<seconds>::max()) {
    const auto al = ptz.lookup(time_point<seconds>::max());
    if (cs > al.cs) {
      if (err != nullptr) *err = "Out-of-range field";
      return false;
    }
  }
  if (tp == time_point<seconds>::min()) {
    const auto al = ptz.lookup(time_point<seconds>::min());
    if (cs < al.cs) {
      if (err != nullptr) *err = "Out-of-range field";
      return false;
    }
  }

  *sec = tp;
  *fs = subseconds;
  return true;
}

}  // namespace detail
}  // namespace cctz
