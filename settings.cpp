#include "settings.h"
#include "resources.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <filesystem>

namespace countdown {
namespace {

constexpr wchar_t section[] = L"Countdown";

std::filesystem::path preferencesPath() {
    wchar_t directory[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, ARRAYSIZE(directory));
    if (!length || length >= ARRAYSIZE(directory)) return {};
    return std::filesystem::path(directory) / L"24HourCountdown" / L"settings.ini";
}

bool validCoordinates(Coordinates position) {
    return std::abs(position.latitude) <= 90 && std::abs(position.longitude) <= 180;
}

std::optional<double> readNumber(const wchar_t* text) {
    wchar_t* end = nullptr;
    const double value = std::wcstod(text, &end);
    if (end == text || *end || !std::isfinite(value)) return {};
    return value;
}

std::wstring zoneLabel(const DYNAMIC_TIME_ZONE_INFORMATION& zone) {
    const std::wstring path = std::wstring(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\")
        + zone.TimeZoneKeyName;
    wchar_t label[256]{};
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegLoadMUIStringW(key, L"MUI_Display", label, sizeof(label), nullptr, 0, nullptr) != ERROR_SUCCESS) {
            DWORD size = sizeof(label);
            RegGetValueW(key, nullptr, L"Display", RRF_RT_REG_SZ, nullptr, label, &size);
        }
        RegCloseKey(key);
    }
    if (*label) return label;

    const LONG minutes = -(zone.Bias + zone.StandardBias);
    wchar_t prefix[32]{};
    swprintf_s(prefix, L"(UTC%c%02ld:%02ld) ", minutes < 0 ? L'-' : L'+',
               std::abs(minutes) / 60, std::abs(minutes) % 60);
    return std::wstring(prefix) + zone.TimeZoneKeyName;
}

std::vector<DYNAMIC_TIME_ZONE_INFORMATION> availableZones() {
    std::vector<DYNAMIC_TIME_ZONE_INFORMATION> result;
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    for (DWORD index = 0; EnumDynamicTimeZoneInformation(index, &zone) == ERROR_SUCCESS; ++index) {
        zone.DynamicDaylightTimeDisabled = FALSE;
        result.push_back(zone);
    }
    return result;
}

Coordinates representativeCoordinates(const DYNAMIC_TIME_ZONE_INFORMATION& zone) {
    // Fallback for a new Windows time-zone ID absent from the embedded catalog.
    const double centralMeridian = -(zone.Bias + zone.StandardBias) / 4.0;
    return {0, std::remainder(centralMeridian, 360.0)};
}

struct SettingsDialog {
    Preferences& preferences;
    DYNAMIC_TIME_ZONE_INFORMATION localZone = systemZone();
    std::vector<DYNAMIC_TIME_ZONE_INFORMATION> zones = availableZones();
    std::vector<const City*> cities;

    void fillZones(HWND window) {
        const HWND combo = GetDlgItem(window, IDC_ZONE);
        std::sort(zones.begin(), zones.end(), [](const auto& a, const auto& b) {
            const LONG aBias = a.Bias + a.StandardBias;
            const LONG bBias = b.Bias + b.StandardBias;
            if (aBias != bBias) return aBias > bBias;
            return std::wcscmp(a.TimeZoneKeyName, b.TimeZoneKeyName) < 0;
        });
        const auto systemLabel = L"跟随电脑 · " + zoneLabel(localZone);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(systemLabel.c_str()));
        int selected = 0;
        for (size_t i = 0; i < zones.size(); ++i) {
            const auto label = zoneLabel(zones[i]);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (preferences.zone == zones[i].TimeZoneKeyName) selected = static_cast<int>(i + 1);
        }
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        SendMessageW(combo, CB_SETDROPPEDWIDTH, MulDiv(445, GetDpiForWindow(window), 96), 0);
    }

    int citySelection(HWND window) const {
        return static_cast<int>(SendDlgItemMessageW(window, IDC_CITY, CB_GETCURSEL, 0, 0));
    }

    const DYNAMIC_TIME_ZONE_INFORMATION& dialogZone(HWND window) const {
        const LRESULT index = SendDlgItemMessageW(window, IDC_ZONE, CB_GETCURSEL, 0, 0);
        return index > 0 ? zones[index - 1] : localZone;
    }

    void updateCoordinates(HWND window) const {
        const int choice = citySelection(window);
        const bool custom = choice > static_cast<int>(cities.size());
        Coordinates position = representativeCoordinates(dialogZone(window));
        if (choice > 0 && !custom) position = cities[choice - 1]->coordinates;
        if (choice == 0 && !cities.empty()) position = cities.front()->coordinates;
        if (custom && preferences.customCoordinates) position = *preferences.customCoordinates;

        wchar_t latitude[32]{};
        wchar_t longitude[32]{};
        swprintf_s(latitude, L"%.6g", position.latitude);
        swprintf_s(longitude, L"%.6g", position.longitude);
        SetDlgItemTextW(window, IDC_LATITUDE, latitude);
        SetDlgItemTextW(window, IDC_LONGITUDE, longitude);
        EnableWindow(GetDlgItem(window, IDC_LATITUDE), custom);
        EnableWindow(GetDlgItem(window, IDC_LONGITUDE), custom);
    }

    void fillCities(HWND window) {
        const HWND combo = GetDlgItem(window, IDC_CITY);
        cities = citiesFor(dialogZone(window).TimeZoneKeyName);
        const std::wstring defaultLabel = cities.empty() ? L"按时区估算（日照地点可手动设置）"
            : std::wstring(L"代表地点 · ") + cities.front()->name;
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(defaultLabel.c_str()));
        int selected = 0;
        for (size_t i = 0; i < cities.size(); ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(cities[i]->name));
            if (preferences.city == cities[i]->name) selected = static_cast<int>(i + 1);
        }
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"手动输入经纬度"));
        if (preferences.customCoordinates) selected = static_cast<int>(cities.size() + 1);
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        updateCoordinates(window);
    }

    bool accept(HWND window) {
        const int choice = citySelection(window);
        std::optional<Coordinates> custom;
        if (choice > static_cast<int>(cities.size())) {
            wchar_t latitude[64]{}, longitude[64]{};
            GetDlgItemTextW(window, IDC_LATITUDE, latitude, ARRAYSIZE(latitude));
            GetDlgItemTextW(window, IDC_LONGITUDE, longitude, ARRAYSIZE(longitude));
            const auto lat = readNumber(latitude);
            const auto lon = readNumber(longitude);
            if (!lat || !lon || !validCoordinates({*lat, *lon})) {
                MessageBoxW(window, L"请输入有效的经纬度。纬度范围为 −90 到 90，经度范围为 −180 到 180。",
                            L"经纬度", MB_OK | MB_ICONWARNING);
                SetFocus(GetDlgItem(window, IDC_LATITUDE));
                return false;
            }
            custom = Coordinates{*lat, *lon};
        }

        const bool follow = SendDlgItemMessageW(window, IDC_ZONE, CB_GETCURSEL, 0, 0) <= 0;
        preferences.zone = follow ? L"" : dialogZone(window).TimeZoneKeyName;
        preferences.city = choice > 0 && !custom ? cities[choice - 1]->name : L"";
        preferences.customCoordinates = custom;
        return true;
    }
};

INT_PTR CALLBACK settingsProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        auto* dialog = reinterpret_cast<SettingsDialog*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, lParam);
        SendDlgItemMessageW(window, IDC_LATITUDE, EM_SETLIMITTEXT, 32, 0);
        SendDlgItemMessageW(window, IDC_LONGITUDE, EM_SETLIMITTEXT, 32, 0);
        dialog->fillZones(window);
        dialog->fillCities(window);
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    auto* dialog = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    switch (LOWORD(wParam)) {
    case IDC_ZONE:
        if (HIWORD(wParam) == CBN_SELCHANGE) dialog->fillCities(window);
        return TRUE;
    case IDC_CITY:
        if (HIWORD(wParam) == CBN_SELCHANGE) dialog->updateCoordinates(window);
        return TRUE;
    case IDOK:
        if (dialog->accept(window)) EndDialog(window, IDOK);
        return TRUE;
    case IDCANCEL:
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

} // namespace

Preferences Preferences::load() {
    Preferences result;
    const auto path = preferencesPath();
    if (path.empty()) return result;
    const auto read = [&path](const wchar_t* key) {
        wchar_t value[128]{};
        GetPrivateProfileStringW(section, key, L"", value, ARRAYSIZE(value), path.c_str());
        return std::wstring(value);
    };
    result.zone = read(L"Zone");
    result.city = read(L"City");
    result.theme = std::clamp(_wtoi(read(L"Theme").c_str()), 0, static_cast<int>(themes.size()) - 1);
    result.topmost = read(L"Topmost") != L"0";
    const auto left = read(L"Left");
    const auto top = read(L"Top");
    if (!left.empty() && !top.empty()) result.position = POINT{_wtol(left.c_str()), _wtol(top.c_str())};
    const auto latitude = readNumber(read(L"Latitude").c_str());
    const auto longitude = readNumber(read(L"Longitude").c_str());
    if (latitude && longitude && validCoordinates({*latitude, *longitude})) {
        result.customCoordinates = Coordinates{*latitude, *longitude};
    }
    return result;
}

bool Preferences::save() const {
    const auto path = preferencesPath();
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    // The profile API preserves Unicode if the file starts with a UTF-16 BOM.
    HANDLE newFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (newFile != INVALID_HANDLE_VALUE) {
        const wchar_t bom = 0xFEFF;
        DWORD written{};
        const BOOL success = WriteFile(newFile, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(newFile);
        if (!success || written != sizeof(bom)) return false;
    }

    std::wstring values;
    const auto add = [&values](const wchar_t* key, const std::wstring& value) {
        values += key;
        values += L'=';
        values += value;
        values += L'\0';
    };
    add(L"Zone", zone);
    add(L"City", city);
    add(L"Theme", std::to_wstring(theme));
    add(L"Topmost", topmost ? L"1" : L"0");
    if (position) {
        add(L"Left", std::to_wstring(position->x));
        add(L"Top", std::to_wstring(position->y));
    }
    if (customCoordinates) {
        add(L"Latitude", std::to_wstring(customCoordinates->latitude));
        add(L"Longitude", std::to_wstring(customCoordinates->longitude));
    }
    return WritePrivateProfileSectionW(section, values.c_str(), path.c_str()) != FALSE;
}

DYNAMIC_TIME_ZONE_INFORMATION systemZone() {
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    if (GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) {
        zone = {};
        wcscpy_s(zone.TimeZoneKeyName, L"UTC");
    }
    return zone;
}

DYNAMIC_TIME_ZONE_INFORMATION selectedZone(const std::wstring& id) {
    if (id.empty()) return systemZone();
    for (const auto& zone : availableZones()) {
        if (id == zone.TimeZoneKeyName) return zone;
    }
    return systemZone();
}

Coordinates selectedCoordinates(const Preferences& preferences, const DYNAMIC_TIME_ZONE_INFORMATION& zone) {
    if (preferences.customCoordinates) return *preferences.customCoordinates;
    const auto cities = citiesFor(zone.TimeZoneKeyName);
    for (const City* city : cities) {
        if (preferences.city == city->name) return city->coordinates;
    }
    return cities.empty() ? representativeCoordinates(zone) : cities.front()->coordinates;
}

bool editPreferences(HINSTANCE instance, HWND owner, Preferences& preferences) {
    SettingsDialog dialog{preferences};
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), owner, settingsProcedure,
                           reinterpret_cast<LPARAM>(&dialog)) == IDOK;
}

} // namespace countdown
