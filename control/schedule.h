#pragma once

// Weekly schedule, one entry per line:
//
//   <days> <HH:MM> <setting>=<value> [<setting>=<value> ...]   # comment
//
// Days are daily, weekdays, weekends, a comma-separated list such as mon,wed,fri, or a date
// such as 2026-11-06 for an entry that runs once. Settings are the same as the web API's, e.g.:
//
//   fri 15:00 power=on heater=on temp=102
//   daily 02:00 salt=3
//   2026-11-06 15:00 heater=on

#include <cstdint>
#include <string>
#include <vector>

#include "spa.h"

namespace spa {

struct Entry {
  uint8_t days;   // bit 0 = Sunday, as struct tm's tm_wday
  uint32_t date;  // YYYYMMDD for an entry that runs once, else 0
  uint8_t hour;
  uint8_t minute;
  std::vector<Request> requests;
};

// Returns an error such as "line 3: unknown day 'fry'", empty on success; out is only replaced
// on success.
std::string parse_schedule(const std::string &text, std::vector<Entry> &out);

inline bool due(const Entry &e, uint32_t date, int wday, int hour, int minute) {
  return (e.date ? e.date == date : e.days >> wday & 1) && e.hour == hour && e.minute == minute;
}

}  // namespace spa
