#pragma once

#include "clock.h"
#include <optional>
#include <string>

namespace countdown {

struct Preferences {
    std::wstring zone;
    std::wstring city;
    std::optional<Coordinates> customCoordinates;
    std::optional<POINT> position;
    int theme = 0;
    bool topmost = true;
    bool startAtLogin = true;

    static Preferences load();
    bool save() const;
};

DYNAMIC_TIME_ZONE_INFORMATION systemZone();
DYNAMIC_TIME_ZONE_INFORMATION selectedZone(const std::wstring& id);
Coordinates selectedCoordinates(const Preferences& preferences, const DYNAMIC_TIME_ZONE_INFORMATION& zone);
bool editPreferences(HINSTANCE instance, HWND owner, Preferences& preferences);
LSTATUS setStartAtLogin(bool enabled);

} // namespace countdown
