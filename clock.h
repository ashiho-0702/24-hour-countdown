#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <vector>

namespace countdown {

constexpr int hoursPerDay = 24;
constexpr int cellCount = 48;
constexpr int minutesPerCell = 30;
constexpr int minutesPerDay = hoursPerDay * 60;
using Instant = std::int64_t;
constexpr Instant ticksPerMillisecond = 10'000;
constexpr Instant ticksPerMinute = 60'000 * ticksPerMillisecond;

struct Coordinates {
    double latitude;
    double longitude;
};

struct City {
    const wchar_t* zone;
    const wchar_t* name;
    Coordinates coordinates;
};

struct Day {
    static Day build(SYSTEMTIME local, DYNAMIC_TIME_ZONE_INFORMATION zone, Coordinates place);
    int elapsedCells(Instant now) const;
    Instant nextChange(int elapsed) const;
    COLORREF color(double minute, int theme) const;

    std::array<Instant, cellCount + 1> boundaries{};
    std::array<double, minutesPerDay + 1> elevations{};
    double sunrise = -1;
    double sunset = -1;
};

struct Theme {
    const wchar_t* name;
    std::array<COLORREF, 6> colors;
};

constexpr int daylightTheme = 0;
extern const std::array<Theme, 7> themes;

std::vector<const City*> citiesFor(const wchar_t* zone);
Instant toInstant(FILETIME fileTime);
Instant toInstant(const SYSTEMTIME& utc);
SYSTEMTIME fromInstant(Instant utc);
double solarElevation(Instant utc, Coordinates place);

} // namespace countdown
