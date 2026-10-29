#include "schedule.h"

#include <cctype>
#include <cstdio>
#include <sstream>

namespace spa {

static const char *const DAYS[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

static std::string parse_days(std::string word, uint8_t &days) {
  for (char &c : word)
    c = std::tolower(static_cast<unsigned char>(c));
  if (word == "daily") {
    days = 0x7F;
    return "";
  }
  if (word == "weekdays") {
    days = 0x3E;
    return "";
  }
  if (word == "weekends") {
    days = 0x41;
    return "";
  }
  days = 0;
  std::stringstream list(word);
  std::string day;
  while (std::getline(list, day, ',')) {
    int i = 0;
    while (i < 7 && day != DAYS[i])
      i++;
    if (i == 7)
      return "unknown day '" + day + "', use daily, weekdays, weekends or e.g. mon,wed,fri";
    days |= 1 << i;
  }
  return "";
}

static std::string parse_time(const std::string &word, uint8_t &hour, uint8_t &minute) {
  unsigned h, m;
  char extra;
  if (std::sscanf(word.c_str(), "%2u:%2u%c", &h, &m, &extra) != 2 || h > 23 || m > 59 || word.size() != 5)
    return "time must be HH:MM, 24-hour, e.g. 07:30 or 19:00";
  hour = h;
  minute = m;
  return "";
}

static std::string parse_line(const std::string &line, Entry &entry) {
  std::stringstream words(line);
  std::string days, time, setting;
  words >> days >> time;
  if (time.empty())
    return "expected days, a time and at least one setting";
  if (auto error = parse_days(days, entry.days); !error.empty())
    return error;
  if (auto error = parse_time(time, entry.hour, entry.minute); !error.empty())
    return error;
  while (words >> setting) {
    auto eq = setting.find('=');
    if (eq == std::string::npos)
      return "expected setting=value, got '" + setting + "'";
    Request r;
    if (auto error = parse_request(setting.substr(0, eq), setting.substr(eq + 1), r); !error.empty())
      return error;
    entry.requests.push_back(r);
  }
  if (entry.requests.empty())
    return "expected at least one setting, e.g. heater=on";
  return "";
}

std::string parse_schedule(const std::string &text, std::vector<Entry> &out) {
  std::vector<Entry> entries;
  std::stringstream lines(text);
  std::string line;
  for (int n = 1; std::getline(lines, line); n++) {
    line = line.substr(0, line.find('#'));
    if (line.find_first_not_of(" \t\r") == std::string::npos)
      continue;
    Entry entry{};
    if (auto error = parse_line(line, entry); !error.empty())
      return "line " + std::to_string(n) + ": " + error;
    entries.push_back(entry);
  }
  out = entries;
  return "";
}

}  // namespace spa
