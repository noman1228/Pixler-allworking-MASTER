#pragma once
#include <cstdint>
#include <ctime>
struct tmElements_t { uint8_t Second = 0, Minute = 0, Hour = 0, Wday = 0, Day = 0, Month = 0, Year = 0; };
#define CalendarYrToTm(value) ((value) - 1970)
inline std::time_t makeTime(const tmElements_t&) { return 0; }
inline void setTime(std::time_t) {}
