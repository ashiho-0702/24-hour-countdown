#include "clock.h"

#include <algorithm>
#include <cmath>

namespace countdown {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double radians = pi / 180.0;
constexpr double horizon = -0.833;
// Extend the warm colors on both sides of the horizon so they span several half-hour cells.
constexpr std::array<double, 6> lightLevels{-30.0, -23.0, -14.0, horizon, 10.0, 23.0};

const City cities[] = {
#include "cities.inc"
};

TIME_ZONE_INFORMATION rulesFor(USHORT year, DYNAMIC_TIME_ZONE_INFORMATION zone) {
    TIME_ZONE_INFORMATION rules{};
    if (!zone.DynamicDaylightTimeDisabled && GetTimeZoneInformationForYear(year, &zone, &rules)) {
        return rules;
    }
    rules.Bias = zone.Bias;
    rules.StandardBias = zone.StandardBias;
    return rules;
}

Instant firstUtcForWallMinute(Instant wall, const TIME_ZONE_INFORMATION& rules) {
    const Instant standard = wall + (rules.Bias + rules.StandardBias) * ticksPerMinute;
    if (rules.DaylightDate.wMonth == 0 || rules.StandardDate.wMonth == 0) {
        return standard;
    }

    // A spring-forward minute can have no UTC mapping, while a fall-back
    // minute can have two. Use the first occurrence to keep the bar monotonic.
    const Instant daylight = wall + (rules.Bias + rules.DaylightBias) * ticksPerMinute;
    for (const Instant candidate : {std::min(standard, daylight), std::max(standard, daylight)}) {
        const SYSTEMTIME utc = fromInstant(candidate);
        SYSTEMTIME local{};
        if (SystemTimeToTzSpecificLocalTime(&rules, &utc, &local) && toInstant(local) == wall) {
            return candidate;
        }
    }
    return 0;
}

COLORREF mix(COLORREF first, COLORREF second, double fraction) {
    const auto channel = [fraction](int a, int b) {
        return static_cast<int>(std::lround(a + (b - a) * fraction));
    };
    return RGB(channel(GetRValue(first), GetRValue(second)),
               channel(GetGValue(first), GetGValue(second)),
               channel(GetBValue(first), GetBValue(second)));
}

} // namespace

const std::array<Theme, 2> themes{{
    {L"白天与黑夜", {RGB(34, 103, 178), RGB(67, 147, 211), RGB(255, 76, 48),
                   RGB(255, 218, 80), RGB(255, 184, 74), RGB(242, 159, 57)}},
    {L"红紫蓝渐变", {RGB(255, 71, 40), RGB(248, 23, 61), RGB(233, 16, 166),
                   RGB(198, 55, 245), RGB(104, 113, 239), RGB(0, 148, 255)}},
}};

std::vector<const City*> citiesFor(const wchar_t* zone) {
    std::vector<const City*> matches;
    for (const City& city : cities) {
        if (CompareStringOrdinal(city.zone, -1, zone, -1, TRUE) == CSTR_EQUAL) {
            matches.push_back(&city);
        }
    }
    return matches;
}

Instant toInstant(FILETIME fileTime) {
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<Instant>(value.QuadPart);
}

Instant toInstant(const SYSTEMTIME& utc) {
    FILETIME fileTime{};
    SystemTimeToFileTime(&utc, &fileTime);
    return toInstant(fileTime);
}

SYSTEMTIME fromInstant(Instant utc) {
    ULARGE_INTEGER value{};
    value.QuadPart = static_cast<ULONGLONG>(utc);
    const FILETIME fileTime{value.LowPart, value.HighPart};
    SYSTEMTIME result{};
    FileTimeToSystemTime(&fileTime, &result);
    return result;
}

double solarElevation(Instant utc, Coordinates place) {
    // NOAA General Solar Position Calculations (fractional year approximation).
    // https://gml.noaa.gov/grad/solcalc/solareqns.PDF
    const SYSTEMTIME date = fromInstant(utc);
    const SYSTEMTIME january{date.wYear, 1, 0, 1};
    const bool leapYear = date.wYear % 4 == 0 && (date.wYear % 100 != 0 || date.wYear % 400 == 0);
    const double day = static_cast<double>(utc - toInstant(january)) / (minutesPerDay * ticksPerMinute);
    const double yearAngle = 2 * pi * (day - 0.5) / (leapYear ? 366 : 365);
    const double timeCorrection = 229.18 * (0.000075 + 0.001868 * std::cos(yearAngle)
        - 0.032077 * std::sin(yearAngle) - 0.014615 * std::cos(2 * yearAngle)
        - 0.040849 * std::sin(2 * yearAngle));
    const double declination = 0.006918 - 0.399912 * std::cos(yearAngle)
        + 0.070257 * std::sin(yearAngle) - 0.006758 * std::cos(2 * yearAngle)
        + 0.000907 * std::sin(2 * yearAngle) - 0.002697 * std::cos(3 * yearAngle)
        + 0.00148 * std::sin(3 * yearAngle);
    const double utcMinutes = date.wHour * 60 + date.wMinute + date.wSecond / 60.0;
    const double hourAngle = ((utcMinutes + timeCorrection) / 4 + place.longitude - 180) * radians;
    const double latitude = place.latitude * radians;
    const double sine = std::sin(latitude) * std::sin(declination)
        + std::cos(latitude) * std::cos(declination) * std::cos(hourAngle);
    return std::asin(std::clamp(sine, -1.0, 1.0)) / radians;
}

Day Day::build(SYSTEMTIME local, DYNAMIC_TIME_ZONE_INFORMATION zone, Coordinates place) {
    local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
    const Instant midnight = toInstant(local);
    const Instant tomorrow = midnight + minutesPerDay * ticksPerMinute;
    const auto todayRules = rulesFor(local.wYear, zone);
    const auto tomorrowDate = fromInstant(tomorrow);
    const auto tomorrowRules = rulesFor(tomorrowDate.wYear, zone);

    // If tomorrow begins with a clock jump, its first valid minute ends today.
    Instant next = firstUtcForWallMinute(tomorrow, tomorrowRules);
    for (int m = 1; !next && m < minutesPerDay; ++m) {
        next = firstUtcForWallMinute(tomorrow + m * ticksPerMinute, tomorrowRules);
    }

    Day day;
    for (int m = minutesPerDay; m >= 0; --m) {
        if (m != minutesPerDay) {
            const Instant mapped = firstUtcForWallMinute(midnight + m * ticksPerMinute, todayRules);
            if (mapped) next = mapped;
        }
        day.elevations[m] = solarElevation(next, place);
        if (m % minutesPerCell == 0) day.boundaries[m / minutesPerCell] = next;
    }
    for (int m = 0; m < minutesPerDay; ++m) {
        const double start = day.elevations[m];
        const double end = day.elevations[m + 1];
        if ((start < horizon) != (end < horizon)) {
            double& event = end > start ? day.sunrise : day.sunset;
            if (event < 0) event = m + (horizon - start) / (end - start);
        }
    }
    return day;
}

int Day::elapsedCells(Instant now) const {
    const auto complete = std::upper_bound(boundaries.begin() + 1, boundaries.end(), now);
    return static_cast<int>(complete - (boundaries.begin() + 1));
}

Instant Day::nextChange(int elapsed) const {
    return boundaries[std::min(elapsed + 1, cellCount)];
}

COLORREF Day::color(double minute, int theme) const {
    const auto& colors = themes[theme].colors;
    minute = std::clamp(minute, 0.0, static_cast<double>(minutesPerDay));
    if (theme == spectrumTheme) {
        const double position = minute * (colors.size() - 1) / minutesPerDay;
        const int left = std::min(static_cast<int>(position), static_cast<int>(colors.size()) - 2);
        return mix(colors[left], colors[left + 1], position - left);
    }
    const int low = std::min(static_cast<int>(minute), minutesPerDay - 1);
    const double elevation = elevations[low] + (elevations[low + 1] - elevations[low]) * (minute - low);
    const auto next = std::upper_bound(lightLevels.begin(), lightLevels.end(), elevation);
    if (next == lightLevels.begin()) return colors.front();
    if (next == lightLevels.end()) return colors.back();
    const auto index = next - lightLevels.begin();
    const double proportion = (elevation - lightLevels[index - 1]) / (lightLevels[index] - lightLevels[index - 1]);
    return mix(colors[index - 1], colors[index], proportion);
}

} // namespace countdown
