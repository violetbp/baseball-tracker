#pragma once

#include <ctime>
#include <cstddef>
#include <string>

namespace esphome {
namespace baseball_tracker {

// mktime with TZ=UTC (MLB gameDate is ISO-8601 UTC).
time_t utc_time_from_tm(struct tm *tp);

// Uppercase helpers (ASCII / current locale behavior of std::toupper).
std::string to_upper(std::string s);
void to_upper_in_place(char *s);
// Writes uppercased copy of `in` into `out` (null-terminated).
// Returns `out` (or nullptr if `out` is null / out_size is 0).
char *to_upper(const char *in, char *out, std::size_t out_size);


}  // namespace baseball_tracker
}  // namespace esphome
