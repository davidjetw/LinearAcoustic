#pragma once
#include <stdint.h>

namespace AudioVersion {
constexpr bool digit(char c) { return c >= '0' && c <= '9'; }
// Custom calendar version, not SemVer: YY.MM.DD+revision, years 2000..2099.
// Zero means invalid. Revision is 1..9999; leading zero is not accepted.
constexpr uint64_t key(const char *s) {
  if (!s) return 0;
  unsigned n = 0;
  while (s[n]) { if (++n > 13) return 0; }
  if (n < 10 || s[2] != '.' || s[5] != '.' || s[8] != '+') return 0;
  if (!digit(s[0]) || !digit(s[1]) || !digit(s[3]) || !digit(s[4]) ||
      !digit(s[6]) || !digit(s[7]) || s[9] == '0') return 0;
  unsigned y = (s[0]-'0')*10 + s[1]-'0';
  unsigned m = (s[3]-'0')*10 + s[4]-'0';
  unsigned d = (s[6]-'0')*10 + s[7]-'0';
  if (m < 1 || m > 12) return 0;
  unsigned days = m == 2 ? (y % 4 == 0 ? 29 : 28) :
                  (m == 4 || m == 6 || m == 9 || m == 11 ? 30 : 31);
  if (d < 1 || d > days) return 0;
  unsigned revision = 0;
  for (unsigned i = 9; i < n; ++i) {
    if (!digit(s[i])) return 0;
    revision = revision * 10 + s[i]-'0';
  }
  if (revision == 0 || revision > 9999) return 0;
  return (uint64_t(y * 10000 + m * 100 + d) << 16) | revision;
}
static_assert(key("26.09.30+10") > key("26.09.30+2"), "Numeric revision order");
static_assert(key("26.10.01+1") > key("26.09.30+9999"), "Date before revision");
static_assert(key("24.02.29+1") > 0, "Leap date");
static_assert(key("26.02.29+1") == 0, "Invalid leap date");
static_assert(key("26.04.31+1") == 0, "Invalid day");
static_assert(key("26.09.30+0") == 0, "Zero revision");
static_assert(key("26.09.30+01") == 0, "Leading zero");
static_assert(key("26.09.30+10000") == 0, "Oversized revision");
static_assert(key("26.09.30+1junk") == 0, "Trailing junk");
static_assert(key("0.1.1") == 0, "Legacy version");
}
