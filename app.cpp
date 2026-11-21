#include "clock.h"
#include "busy_person.h"
#include "resources.h"
#include "settings.h"

#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>

namespace {

using namespace countdown;

constexpr wchar_t appName[] = L"24 小时倒计时";
constexpr wchar_t windowClass[] = L"Countdown.Desktop.Window";
constexpr wchar_t instanceName[] = L"Local\\Countdown.Desktop.425CA4A2";
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT restoreMessage = WM_APP + 2;
constexpr UINT_PTR tickTimer = 1;
constexpr UINT_PTR personTimer = 2;
constexpr UINT poseIntervalMs = 125;
constexpr double fadeCells = 1.25;
constexpr int baseWidth = 746;
constexpr int baseHeight = 115;
constexpr int cornerDiameter = 21;
constexpr int numeralTop = 31;
constexpr int thoughtFontSize = 14;
constexpr int lampTop = 66;
constexpr int lampBottom = 99;
constexpr COLORREF caseColor = RGB(43, 46, 51);
constexpr COLORREF glassColor = RGB(11, 12, 15);
constexpr COLORREF scaleInk = RGB(227, 226, 220);

// Narrow, chamfered numeral silhouettes on a 5 × 9 grid.
constexpr unsigned numerals[10][9] = {
    {0b01110, 0b11011, 0b11011, 0b11011, 0b11011, 0b11011, 0b11011, 0b11011, 0b01110},
    {0b00110, 0b01110, 0b00110, 0b00110, 0b00110, 0b00110, 0b00110, 0b00110, 0b00110},
    {0b01110, 0b11011, 0b00011, 0b00011, 0b00110, 0b01100, 0b11000, 0b11000, 0b11111},
    {0b01110, 0b11011, 0b00011, 0b00011, 0b01110, 0b00011, 0b00011, 0b11011, 0b01110},
    {0b00011, 0b00111, 0b01111, 0b01111, 0b11011, 0b11111, 0b00011, 0b00011, 0b00011},
    {0b11111, 0b11000, 0b11000, 0b11110, 0b00011, 0b00011, 0b00011, 0b11011, 0b01110},
    {0b01110, 0b11011, 0b11000, 0b11000, 0b11110, 0b11011, 0b11011, 0b11011, 0b01110},
    {0b11111, 0b00011, 0b00011, 0b00110, 0b00110, 0b01100, 0b01100, 0b01100, 0b01100},
    {0b01110, 0b11011, 0b11011, 0b11011, 0b01110, 0b11011, 0b11011, 0b11011, 0b01110},
    {0b01110, 0b11011, 0b11011, 0b11011, 0b01111, 0b00011, 0b00011, 0b11011, 0b01110},
};

enum MenuItem { menuShow = 1201, menuHide, menuTopmost, menuSettings, menuExit, menuTheme = 1300 };

void fill(HDC dc, RECT bounds, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void drawDigit(HDC dc, int digit, RECT area) {
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    for (int row = 0; row < 9; ++row) {
        for (int column = 0; column < 5; ++column) {
            if ((numerals[digit][row] & (0b10000 >> column)) == 0) continue;
            fill(dc, {area.left + MulDiv(width, column, 5), area.top + MulDiv(height, row, 9),
                      area.left + MulDiv(width, column + 1, 5), area.top + MulDiv(height, row + 1, 9)}, scaleInk);
        }
    }
}

void line(HDC dc, int x, int y, int toX, int toY) {
    MoveToEx(dc, x, y, nullptr);
    LineTo(dc, toX, toY);
}

void roundRect(HDC dc, RECT bounds, int diameter, COLORREF brush, COLORREF pen) {
    SetDCBrushColor(dc, brush);
    SetDCPenColor(dc, pen);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, diameter, diameter);
}

COLORREF blend(COLORREF from, COLORREF to, double amount) {
    const auto channel = [amount](int a, int b) {
        return static_cast<int>(a + (b - a) * amount + 0.5);
    };
    return RGB(channel(GetRValue(from), GetRValue(to)),
               channel(GetGValue(from), GetGValue(to)),
               channel(GetBValue(from), GetBValue(to)));
}

double warmth(COLORREF color) {
    return std::clamp((GetRValue(color) - GetBValue(color)) / 155.0, 0.0, 1.0);
}

// Five columns and nine rows across each diffuser, sampled from the supplied clock.
constexpr COLORREF lensSamples[cellCount][9][5] = {
#include "lens_samples.inc"
};

double referenceCell(const Day& day, double minute) {
    const auto wrap = [](double value, double period) {
        return value - period * std::floor(value / period);
    };
    const double daytime = wrap(day.sunset - day.sunrise, minutesPerDay);
    const double sinceRise = wrap(minute - day.sunrise, minutesPerDay);
    // The reference has its red dawn core at cell 13 and its golden dusk core at 31.
    const double position = sinceRise < daytime
        ? 13 + 18 * sinceRise / daytime
        : 31 + 30 * (sinceRise - daytime) / (minutesPerDay - daytime);
    return wrap(position, cellCount);
}

COLORREF sampleDiffuser(int index, double u, double v) {
    const int column = std::min(3, static_cast<int>(u * 4));
    const int row = std::min(7, static_cast<int>(v * 8));
    const auto& grid = lensSamples[index];
    const double x = u * 4 - column;
    return blend(blend(grid[row][column], grid[row][column + 1], x),
                 blend(grid[row + 1][column], grid[row + 1][column + 1], x), v * 8 - row);
}

COLORREF sampleLens(double cell, double u, double v) {
    const int first = static_cast<int>(cell);
    return blend(sampleDiffuser(first, u, v), sampleDiffuser((first + 1) % cellCount, u, v), cell - first);
}

const std::vector<DWORD>& drawMetalCase(HDC dc, RECT client) {
    const int width = client.right, height = client.bottom;
    static std::vector<DWORD> pixels;
    if (width <= 0 || height <= 0) return pixels;
    static SIZE cached{};
    if (cached.cx != width || cached.cy != height) {
        cached = {width, height};
        pixels.resize(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y) {
            const double py = (y + 0.5) * baseHeight / height;
            for (int x = 0; x < width; ++x) {
                const double px = (x + 0.5) * baseWidth / width;
                const double dx = std::abs(px - baseWidth / 2.0) - (baseWidth / 2.0 - 12);
                const double dy = std::abs(py - baseHeight / 2.0) - (baseHeight / 2.0 - 12);
                const double depth = 12 - std::hypot(std::max(0.0, dx), std::max(0.0, dy))
                                        - std::min(0.0, std::max(dx, dy));
                DWORD pixel = 0x0b0c0f;
                if (depth < 10) {
                    const double along = px / baseWidth;
                    const double reflection = 0.5 + 0.5 * std::cos(10 * along - 1.1);
                    const double grain = 2.3 * std::sin(py * 11 + px * 0.08)
                                       + 1.5 * std::sin(px * 0.73 + py * 31);
                    const double face = 55 + 41 * reflection + grain;
                    const double outside = std::exp(-std::pow((depth - 1.5) / 1.0, 2));
                    const double inside = std::exp(-std::pow((depth - 7.7) / 1.0, 2));
                    const double shadow = std::clamp((10 - depth) / 1.7, 0.0, 1.0);
                    const double lower = py / baseHeight;
                    const int level = static_cast<int>(std::clamp(
                        (face + outside * (83 - 62 * lower) + inside * (18 + 89 * lower)) * shadow, 5.0, 241.0));
                    const int blue = std::max(0, level - 4);
                    pixel = ((level + 3) << 16) | (level << 8) | blue;
                }
                pixels[static_cast<size_t>(y) * width + x] = pixel;
            }
        }
    }
    BITMAPINFO bitmap{};
    bitmap.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    SetDIBitsToDevice(dc, 0, 0, width, height, 0, 0, 0, height, pixels.data(), &bitmap, DIB_RGB_COLORS);
    return pixels;
}

class App {
public:
    App(HINSTANCE instance, HICON smallIcon)
        : instance_(instance), smallIcon_(smallIcon), preferences_(Preferences::load()) {}

    bool create();
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

private:
    int scale(int value) const { return MulDiv(value, dpi_, 96); }
    void createControls();
    void placeWindow(bool restorePosition);
    void updateGeometry();
    RECT closeButtonBounds(int clientWidth) const;
    void syncShadow();
    void rememberPosition();
    void refresh(bool rebuild = false, bool forPreview = false);
    void paint(HDC target, const RECT* damage = nullptr);
    RECT personBounds() const;
    RECT personPaintBounds(RECT figure) const;
    bool hitPerson(POINT point) const;
    RECT thoughtBounds(RECT figure) const;
    void drawThought(HDC dc, RECT bubble, RECT figure) const;
    void updatePerson(bool clicked);
    void drawPerson(HDC target, RECT bounds);
    int firstWholeCell() const;
    void clearFaceCache();
    RECT timeBounds(int clientWidth) const;
    void drawTime(HDC dc, int clientWidth) const;
    void drawFace(HDC dc, const RECT& client, int theme, double elapsed);
    void drawLamps(HDC dc, int trackStart, int trackWidth, int theme, double elapsed,
                   const std::vector<DWORD>& metal, const RECT& client);
    static INT_PTR CALLBACK paletteProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    void drawSolarMarks(HDC dc, int trackStart, int trackWidth, int clientWidth);
    void drawHideButton(const DRAWITEMSTRUCT& item);

    bool addTrayIcon();
    void removeTrayIcon();
    void useTaskbarButton(bool enabled);
    void show();
    void hide();
    void menu(POINT position, bool fromTray);
    void command(UINT id);

    HINSTANCE instance_;
    HICON smallIcon_;
    HWND window_ = nullptr;
    HWND hideButton_ = nullptr;
    HWND shadow_ = nullptr;
    SIZE shadowSize_{};
    UINT dpi_ = 96;
    UINT taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    Preferences preferences_;
    std::optional<Day> day_;
    DYNAMIC_TIME_ZONE_INFORMATION zone_{};
    SYSTEMTIME localDate_{};
    double elapsed_ = 0;
    BusyPerson person_;
    HDC faceDC_ = nullptr;
    HBITMAP faceBitmap_ = nullptr;
    DWORD* facePixels_ = nullptr;
    SIZE faceSize_{};
    bool faceDirty_ = true;
    std::vector<DWORD> personPixels_;
    bool hasTrayIcon_ = false;
    bool trayVersion4_ = false;
    bool settingsOpen_ = false;
};

bool App::create() {
    zone_ = selectedZone(preferences_.zone);
    dpi_ = GetDpiForSystem();
    const DWORD style = WS_EX_TOOLWINDOW | (preferences_.topmost ? WS_EX_TOPMOST : 0);
    if (!CreateWindowExW(style, windowClass, appName, WS_POPUP | WS_CLIPCHILDREN,
                         0, 0, scale(baseWidth), scale(baseHeight), nullptr, nullptr, instance_, this)) {
        return false;
    }
    placeWindow(true);
    updateGeometry();
    shadow_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                              L"STATIC", L"Countdown shadow", WS_POPUP, 0, 0, 0, 0,
                              window_, nullptr, instance_, nullptr);
    useTaskbarButton(!addTrayIcon());
    show();
    if (setStartAtLogin(preferences_.startAtLogin) != ERROR_SUCCESS) {
        MessageBoxW(window_, L"未能应用开机自启设置。可在右键菜单的「设置」中重试。",
                    L"开机自动启动", MB_OK | MB_ICONWARNING);
    }
    return true;
}

void App::createControls() {
    hideButton_ = CreateWindowExW(0, L"BUTTON", L"关闭小窗", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                 0, 0, 0, 0, window_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HIDE)), instance_, nullptr);
}

void App::placeWindow(bool restorePosition) {
    RECT current{};
    GetWindowRect(window_, &current);
    if (restorePosition && preferences_.position) {
        const auto position = *preferences_.position;
        OffsetRect(&current, position.x - current.left, position.y - current.top);
    }
    const bool useDefault = restorePosition && !preferences_.position;
    const HMONITOR monitor = useDefault ? MonitorFromPoint(POINT{}, MONITOR_DEFAULTTOPRIMARY)
        : MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST);
    MONITORINFO area{sizeof(area)};
    if (!GetMonitorInfoW(monitor, &area)) return;

    const RECT work = area.rcWork;
    const int width = scale(baseWidth);
    const int height = scale(baseHeight);
    const int preferredX = useDefault ? work.left + (work.right - work.left - width) / 2 : current.left;
    const int preferredY = useDefault ? work.top + scale(28) : current.top;
    const int x = std::clamp(preferredX, static_cast<int>(work.left),
                             std::max<int>(work.left, work.right - width));
    const int y = std::clamp(preferredY, static_cast<int>(work.top),
                             std::max<int>(work.top, work.bottom - height));
    SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

RECT App::closeButtonBounds(int clientWidth) const {
    const int inset = (scale(cornerDiameter) + 1) / 2 + scale(1);
    const int side = scale(numeralTop) - inset;
    return {clientWidth - inset - side, inset, clientWidth - inset, inset + side};
}

void App::updateGeometry() {
    faceDirty_ = true;
    RECT client{};
    GetClientRect(window_, &client);
    const int diameter = scale(cornerDiameter);
    const HRGN corners = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, diameter, diameter);
    if (corners && !SetWindowRgn(window_, corners, TRUE)) DeleteObject(corners);
    const RECT button = closeButtonBounds(client.right);
    SetWindowPos(hideButton_, nullptr, button.left, button.top, button.right - button.left,
                 button.bottom - button.top, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
}

void renderShadow(DWORD* pixels, SIZE size, int width, int height, int pad, double unit, double radius) {
    for (int y = 0; y < size.cy; ++y) {
        for (int x = 0; x < size.cx; ++x) {
            const double dx = std::abs(x + 0.5 - pad - width * 0.5) - (width * 0.5 - radius);
            const double py = y + 0.5 - pad - height * 0.5;
            const auto distance = [&](double offset) {
                const double dy = std::abs(py - offset * unit) - (height * 0.5 - radius);
                return (std::hypot(std::max(dx, 0.0), std::max(dy, 0.0))
                        + std::min(std::max(dx, dy), 0.0) - radius) / unit;
            };
            DWORD alpha = 0;
            if (distance(0) > 0) {
                alpha = static_cast<DWORD>(34 * std::erfc(distance(5) / 8.5)
                                           + 20 * std::erfc(distance(1) / 2.8));
            }
            *pixels++ = alpha << 24;
        }
    }
}

void App::syncShadow() {
    if (!shadow_) return;
    if (!IsWindowVisible(window_) || IsIconic(window_)) {
        ShowWindow(shadow_, SW_HIDE);
        return;
    }
    RECT panel{};
    GetWindowRect(window_, &panel);
    const int pad = scale(22);
    const int width = panel.right - panel.left;
    const int height = panel.bottom - panel.top;
    POINT origin{panel.left - pad, panel.top - pad};
    SIZE size{width + 2 * pad, height + 2 * pad};
    if (size.cx != shadowSize_.cx || size.cy != shadowSize_.cy) {
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), size.cx, -size.cy, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!bitmap || !dc) {
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
            return;
        }
        const auto original = SelectObject(dc, bitmap);
        DWORD* pixels = static_cast<DWORD*>(bits);
        renderShadow(pixels, size, width, height, pad, dpi_ / 96.0, scale(cornerDiameter) * 0.5);
        POINT start{};
        BLENDFUNCTION blendFunction{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BOOL ok = UpdateLayeredWindow(shadow_, nullptr, &origin, &size, dc, &start, 0,
                                            &blendFunction, ULW_ALPHA);
        SelectObject(dc, original);
        DeleteDC(dc);
        DeleteObject(bitmap);
        if (!ok) return;
        shadowSize_ = size;
    }
    SetWindowPos(shadow_, nullptr, origin.x, origin.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

void App::rememberPosition() {
    RECT bounds{};
    if (IsIconic(window_) || !GetWindowRect(window_, &bounds)) return;
    if (preferences_.position && preferences_.position->x == bounds.left
        && preferences_.position->y == bounds.top) return;
    preferences_.position = POINT{bounds.left, bounds.top};
    preferences_.save();
}

void App::refresh(bool rebuild, bool forPreview) {
    if (rebuild) {
        zone_ = selectedZone(preferences_.zone);
        day_.reset();
    }
    const bool visible = IsWindowVisible(window_) && !IsIconic(window_);
    if (!visible && !forPreview) return;

    FILETIME nowFile{};
    GetSystemTimeAsFileTime(&nowFile);
    const Instant now = toInstant(nowFile);
    const SYSTEMTIME utc = fromInstant(now);
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTimeEx(&zone_, &utc, &local)) local = utc;
    if (!day_ || local.wYear != localDate_.wYear || local.wMonth != localDate_.wMonth
        || local.wDay != localDate_.wDay) {
        day_ = Day::build(local, zone_, selectedCoordinates(preferences_, zone_));
        rebuild = true;
    }

    const int complete = day_->elapsedCells(now);
    double newlyElapsed = complete;
    if (complete < cellCount) {
        const Instant start = day_->boundaries[complete];
        const Instant duration = day_->nextChange(complete) - start;
        const Instant minute = now - now % ticksPerMinute;
        if (duration > 0) newlyElapsed += std::clamp(static_cast<double>(minute - start) / duration, 0.0, 1.0);
    }
    if (rebuild || newlyElapsed != elapsed_) {
        faceDirty_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    } else if (local.wMinute != localDate_.wMinute ||
               local.wHour != localDate_.wHour) {
        faceDirty_ = true;
        const RECT bounds = timeBounds(scale(baseWidth));
        InvalidateRect(window_, &bounds, FALSE);
    }
    localDate_ = local;
    elapsed_ = newlyElapsed;
    person_.fit(firstWholeCell());

    if (!visible) return;
    if (person_.position >= 0) SetTimer(window_, personTimer, poseIntervalMs, nullptr);
    else KillTimer(window_, personTimer);
    SetTimer(window_, tickTimer, (60 - local.wSecond) * 1000 - local.wMilliseconds, nullptr);
}

RECT App::timeBounds(int clientWidth) const {
    const RECT button = closeButtonBounds(clientWidth);
    const int right = button.left - scale(5);
    return {right - scale(45), button.top, right, scale(27)};
}

void App::drawTime(HDC dc, int clientWidth) const {
    RECT bounds = timeBounds(clientWidth);
    fill(dc, bounds, glassColor);
    const HFONT font = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    const auto old = SelectObject(dc, font ? font : GetStockObject(ANSI_FIXED_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(211, 217, 222));
    wchar_t time[6]{};
    swprintf_s(time, L"%02u:%02u", static_cast<unsigned>(localDate_.wHour),
               static_cast<unsigned>(localDate_.wMinute));
    DrawTextW(dc, time, 5, &bounds, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
    if (font) DeleteObject(font);
}
void App::drawFace(HDC dc, const RECT& client, int theme, double elapsed) {
    const auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    const auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto& metal = drawMetalCase(dc, client);

    constexpr int first = 22;
    constexpr int last = 724;
    const int trackStart = scale(first);
    const int trackWidth = scale(last) - trackStart;
    const auto tickX = [trackStart, trackWidth](int tick) {
        return trackStart + MulDiv(trackWidth, tick, cellCount);
    };
    drawLamps(dc, trackStart, trackWidth, theme, elapsed, metal, client);
    drawTime(dc, client.right);
    if (theme == daylightTheme) drawSolarMarks(dc, trackStart, trackWidth, client.right);

    const int digitWidth = scale(6);
    const int digitGap = scale(2);
    const int digitTop = scale(numeralTop);
    const int digitBottom = scale(48);
    for (int tick = 0; tick <= cellCount; ++tick) {
        const bool major = tick % 2 == 0;
        const int x = tickX(tick);
        SetDCPenColor(dc, major ? RGB(163, 166, 167) : RGB(93, 101, 108));
        line(dc, x, scale(major ? 52 : 57), x, scale(64));
        if (major) {
            const int hour = tick / 2;
            int left = x - digitWidth / 2;
            if (hour >= 10) {
                left = x - (2 * digitWidth + digitGap) / 2;
                drawDigit(dc, hour / 10, {left, digitTop, left + digitWidth, digitBottom});
                left += digitWidth + digitGap;
            }
            drawDigit(dc, hour % 10, {left, digitTop, left + digitWidth, digitBottom});
        }
    }

    SelectObject(dc, brush);
    SelectObject(dc, pen);
}

void App::drawLamps(HDC dc, int trackStart, int trackWidth, int theme, double elapsed,
                    const std::vector<DWORD>& metal, const RECT& client) {
    const int top = scale(lampTop);
    const int bottom = scale(lampBottom);
    const int halo = std::max(1, scale(7));
    const auto tickX = [trackStart, trackWidth](int tick) {
        return trackStart + MulDiv(trackWidth, tick, cellCount);
    };
    const bool reference = day_ && theme == daylightTheme && day_->sunrise >= 0 && day_->sunset >= 0;
    const auto colorAt = [&](int x) {
        const double minute = (x - trackStart + 0.5) * minutesPerDay / trackWidth;
        if (reference) return sampleLens(referenceCell(*day_, minute), 0.5, 0.45);
        return day_->color(minute, theme);
    };

    roundRect(dc, {trackStart - scale(3), top - scale(2), tickX(cellCount) + scale(3),
                  bottom + scale(2)}, scale(5), RGB(5, 7, 9), RGB(3, 4, 6));
    const double boundary = trackStart + trackWidth * elapsed / cellCount;
    const int lightStart = static_cast<int>(std::floor(boundary));
    const double fadeWidth = std::min(boundary - trackStart, fadeCells * trackWidth / cellCount);
    const auto brightnessAt = [&](int x) {
        if (fadeWidth <= 0) return 1.0;
        const double t = std::clamp((x + 0.5 - boundary) / fadeWidth, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    if (day_) {
        // Integrate each diffuser across the brushed bevel, including the end caps.
        const double pitch = static_cast<double>(trackWidth) / cellCount;
        const double spread = 1.65 * pitch;
        struct Emitter { COLORREF tint; double power; };
        std::array<Emitter, cellCount> emitters{};
        for (int cell = 0; cell < cellCount; ++cell) {
            const int left = tickX(cell), width = tickX(cell + 1) - left;
            const COLORREF tint = colorAt(left + width / 2);
            const double light = (brightnessAt(left + width / 4) + brightnessAt(left + 3 * width / 4)) / 2;
            emitters[cell] = {tint, light * (0.40 + warmth(tint) * 0.22)};
        }
        const int bevelTop = scale(baseHeight - 10), bevelCenter = scale(baseHeight - 7);
        const int sideWidth = scale(10), lampCenter = (top + bottom) / 2;
        for (int x = 0; x < client.right; ++x) {
            const int nearest = static_cast<int>(std::floor((x - trackStart) / pitch));
            double power = 0, red = 0, green = 0, blue = 0;
            for (int i = std::max(0, nearest - 5); i <= std::min(cellCount - 1, nearest + 5); ++i) {
                const double dx = (x + 0.5 - trackStart - (i + 0.5) * pitch) / spread;
                const double weight = std::exp(-dx * dx) * emitters[i].power;
                const COLORREF tint = emitters[i].tint;
                power += weight;
                red += weight * GetRValue(tint);
                green += weight * GetGValue(tint);
                blue += weight * GetBValue(tint);
            }
            if (power <= 0) continue;
            const COLORREF tint = RGB(red / power, green / power, blue / power);
            const int edge = std::min<int>(x, client.right - 1 - x);
            const double side = (edge - scale(6)) / static_cast<double>(scale(4));
            const double across = std::exp(-side * side);
            const int firstY = edge < sideWidth ? top - scale(22) : bevelTop;
            for (int y = firstY; y < std::min<int>(client.bottom, scale(baseHeight - 1)); ++y) {
                const double depth = (y - bevelCenter) / static_cast<double>(scale(3));
                const double along = (y - lampCenter) / static_cast<double>(scale(22));
                const double profile = std::max(std::exp(-depth * depth), 2.0 * across * std::exp(-along * along));
                const double amount = power * pitch / (1.77245 * spread) * profile;
                const DWORD base = metal[static_cast<size_t>(y) * client.right + x];
                const COLORREF steel = RGB(GetBValue(base), GetGValue(base), GetRValue(base));
                SetPixelV(dc, x, y, blend(steel, tint, amount));
            }
        }
        for (int x = lightStart; x < std::min<int>(client.right, tickX(cellCount)); ++x) {
            const COLORREF tint = colorAt(x);
            const double strength = 0.40 + warmth(tint) * 0.22;
            const double onset = brightnessAt(x);
            for (int band = 0; band < halo; ++band) {
                const double fade = 1.0 - static_cast<double>(band) / halo;
                const COLORREF glow = blend(glassColor, tint, onset * strength * fade * fade);
                SetPixelV(dc, x, top - 1 - band, glow);
                if (bottom + band < bevelTop) SetPixelV(dc, x, bottom + band, glow);
            }
        }
    }

    for (int cell = 0; cell < cellCount; ++cell) {
        const int left = tickX(cell) + std::max(1, scale(1));
        const int right = tickX(cell + 1);
        const RECT lens{left, top, right, bottom};
        if (!day_ || right <= lightStart) {
            roundRect(dc, lens, scale(4), RGB(3, 5, 7), RGB(14, 16, 20));
            continue;
        }
        const COLORREF mid = colorAt((left + right) / 2);
        const int width = right - left;
        const int height = bottom - top;
        const double position = reference ? referenceCell(*day_, (cell + 0.5) * minutesPerCell) : 0;
        const double radius = scale(2);
        const COLORREF edge = blend(glassColor, mid, 0.68);
        const COLORREF center = blend(mid, RGB(253, 246, 225), 0.30);
        for (int y = top; y < bottom; ++y) {
            const double v = (y - top + 0.5) / height;
            for (int x = left; x < right; ++x) {
                const double u = (x - left + 0.5) / width;
                COLORREF color;
                if (reference) {
                    color = sampleLens(position, u, v);
                } else {
                    // Transfer the measured shading of an amber lens to the selected hue.
                    const double light = (GetGValue(sampleDiffuser(22, u, v)) - 120) / 86.0;
                    color = blend(edge, center, std::clamp(light, 0.0, 1.0));
                }
                const double dx = std::max(0.0, radius - std::min(x - left + 0.5, right - x - 0.5));
                const double dy = std::max(0.0, radius - std::min(y - top + 0.5, bottom - y - 0.5));
                const double coverage = std::clamp(radius + 0.5 - std::hypot(dx, dy), 0.0, 1.0);
                SetPixelV(dc, x, y, blend(RGB(5, 7, 9), color, coverage));
            }
        }
    }
    if (elapsed > 0 && elapsed < cellCount) {
        const int firstCell = static_cast<int>(elapsed);
        const int end = std::min(tickX(cellCount), static_cast<int>(std::ceil(boundary + fadeWidth)));
        for (int cell = firstCell; cell < cellCount && tickX(cell) < end; ++cell) {
            const int left = tickX(cell) + std::max(1, scale(1));
            const int right = tickX(cell + 1);
            for (int x = left; x < std::min(right, end); ++x) {
                const double light = brightnessAt(x);
                for (int y = top; y < bottom; ++y) {
                    const bool rim = x == left || x == right - 1 || y == top || y == bottom - 1;
                    const COLORREF off = rim ? RGB(14, 16, 20) : RGB(3, 5, 7);
                    SetPixelV(dc, x, y, blend(off, GetPixel(dc, x, y), light));
                }
            }
        }
    }
}

bool vectorGraphicsReady() {
    using namespace Gdiplus;
    struct Rasterizer {
        ULONG_PTR token{};
        Rasterizer() {
            GdiplusStartupInput input;
            GdiplusStartup(&token, &input, nullptr);
        }
        ~Rasterizer() { if (token) GdiplusShutdown(token); }
    };
    static Rasterizer rasterizer;
    return rasterizer.token != 0;
}

void drawSunIcon(HDC dc, int centerX, int baseline, UINT dpi) {
    using namespace Gdiplus;
    if (!vectorGraphicsReady()) return;

    Graphics canvas(dc);
    canvas.SetPageUnit(UnitPixel);
    canvas.SetSmoothingMode(SmoothingModeAntiAlias);
    canvas.SetPixelOffsetMode(PixelOffsetModeHalf);
    const REAL unit = dpi / 96.0f;
    Matrix transform(unit, 0, 0, unit, static_cast<REAL>(centerX), static_cast<REAL>(baseline));
    canvas.SetTransform(&transform);

    const Color amber(255, 240, 183, 101);
    SolidBrush fill(amber);
    GraphicsPath disc;
    disc.AddArc(-4.0f, -4.0f, 8.0f, 8.0f, 180.0f, 180.0f);
    disc.CloseFigure();
    canvas.FillPath(&fill, &disc);

    Pen stroke(amber, 1.0f);
    stroke.SetLineCap(LineCapRound, LineCapRound, DashCapFlat);
    GraphicsPath rays;
    rays.AddLine(-5.5f, 1.7f, 5.5f, 1.7f);
    rays.StartFigure();
    rays.AddLine(0.0f, -7.0f, 0.0f, -5.6f);
    rays.StartFigure();
    rays.AddLine(-4.9f, -4.9f, -4.0f, -4.0f);
    rays.StartFigure();
    rays.AddLine(4.9f, -4.9f, 4.0f, -4.0f);
    canvas.DrawPath(&stroke, &rays);
}

void App::drawSolarMarks(HDC dc, int trackStart, int trackWidth, int clientWidth) {
    if (!day_ || (day_->sunrise < 0 && day_->sunset < 0)) return;
    const HFONT font = CreateFontW(-scale(11), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Microsoft YaHei UI");
    const HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(232, 194, 139));
    struct Mark {
        double minute;
        const wchar_t* text;
        int anchor{}, left{}, width{};
    };
    Mark marks[]{{day_->sunrise, L"日出"}, {day_->sunset, L"日落"}};
    if (marks[1].minute < marks[0].minute) std::swap(marks[0], marks[1]);
    const int leftLimit = trackStart;
    const int rightLimit = timeBounds(clientWidth).left - scale(5);
    const int textOffset = scale(15);
    for (auto& mark : marks) {
        if (mark.minute < 0) continue;
        SIZE size{};
        GetTextExtentPoint32W(dc, mark.text, 2, &size);
        mark.width = textOffset + size.cx;
        mark.anchor = trackStart + static_cast<int>(std::lround(mark.minute * trackWidth / minutesPerDay));
        mark.left = std::clamp(mark.anchor - mark.width / 2, leftLimit, rightLimit - mark.width);
    }
    if (marks[0].minute >= 0 && marks[1].minute >= 0) {
        const int spacing = marks[0].width + scale(5);
        const int overlap = marks[0].left + spacing - marks[1].left;
        if (overlap > 0) {
            const int lastStart = rightLimit - marks[1].width - spacing;
            marks[0].left = std::clamp(marks[0].left - overlap / 2, leftLimit, lastStart);
            marks[1].left = marks[0].left + spacing;
        }
    }
    const auto pen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, RGB(140, 112, 73));
    for (const auto& mark : marks) {
        if (mark.minute < 0) continue;
        RECT title{mark.left + textOffset, scale(11), mark.left + mark.width, scale(28)};
        DrawTextW(dc, mark.text, 2, &title, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        drawSunIcon(dc, mark.left + scale(6), scale(21), dpi_);
        line(dc, mark.left + mark.width / 2, scale(27), mark.anchor, scale(30));
    }
    SelectObject(dc, pen);
    if (previous) SelectObject(dc, previous);
    if (font) DeleteObject(font);
}

int App::firstWholeCell() const {
    // The fade reaches into the lit side. Both the figure and its props must clear it.
    return std::min(cellCount, static_cast<int>(std::ceil(elapsed_ + std::min(elapsed_, fadeCells))));
}

RECT App::personBounds() const {
    if (person_.position < 0) return {};
    const int trackStart = scale(22), trackWidth = scale(724) - trackStart;
    const int width = std::min(scale(Silhouette::width), trackWidth / cellCount);
    const int center = trackStart + static_cast<int>(std::lround((person_.position + 0.5) * trackWidth / cellCount));
    const int firstPixel = trackStart + MulDiv(trackWidth, firstWholeCell(), cellCount) + scale(1);
    const int left = std::max(firstPixel, std::min(center - width / 2, scale(724) - width));
    const int feet = scale(lampBottom - 1);
    return {left, feet - scale(Silhouette::height), std::min(left + width, scale(724)), feet};
}

void App::clearFaceCache() {
    if (faceDC_) DeleteDC(faceDC_);
    if (faceBitmap_) DeleteObject(faceBitmap_);
    faceDC_ = nullptr;
    faceBitmap_ = nullptr;
    facePixels_ = nullptr;
    faceSize_ = {};
    faceDirty_ = true;
}

RECT App::personPaintBounds(RECT figure) const {
    if (IsRectEmpty(&figure)) return {};
    return {figure.left - scale(6), figure.top, figure.right + scale(6), scale(baseHeight - 1)};
}

bool App::hitPerson(POINT point) const {
    RECT hit = personBounds();
    if (IsRectEmpty(&hit)) return false;
    InflateRect(&hit, scale(2), scale(2));
    return PtInRect(&hit, point) != FALSE;
}

RECT App::thoughtBounds(RECT figure) const {
    if (!person_.thought || IsRectEmpty(&figure)) return {};
    int textWidth = 0;
    for (const wchar_t* p = person_.thought; *p; ++p)
        textWidth += *p < 128 ? thoughtFontSize / 2 : thoughtFontSize;
    const int width = scale(textWidth + 16);
    const int center = (figure.left + figure.right) / 2;
    const int left = std::clamp(center - width / 2, scale(12), scale(baseWidth - 12) - width);
    return {left, figure.top - scale(28), left + width, figure.top - scale(1)};
}

void App::drawThought(HDC dc, RECT bubble, RECT figure) const {
    using namespace Gdiplus;
    const int saved = SaveDC(dc);
    if (!saved) return;
    RECT body = bubble;
    body.bottom -= scale(5);
    if (vectorGraphicsReady()) {
        Graphics canvas(dc);
        canvas.SetPageUnit(UnitPixel);
        canvas.SetSmoothingMode(SmoothingModeAntiAlias);
        const REAL stroke = dpi_ / 96.0f;
        const REAL inset = stroke * 0.5f + 0.5f;
        const REAL left = body.left + inset, top = body.top + inset;
        const REAL right = body.right - inset, bottom = body.bottom - inset;
        const REAL diameter = 8 * stroke;
        const REAL anchor = (figure.left + figure.right) * 0.5f;
        const REAL tail = std::clamp(anchor, left + diameter, right - diameter);
        GraphicsPath outline;
        outline.AddArc(left, top, diameter, diameter, 180, 90);
        outline.AddArc(right - diameter, top, diameter, diameter, 270, 90);
        outline.AddArc(right - diameter, bottom - diameter, diameter, diameter, 0, 90);
        outline.AddLine(tail + 4 * stroke, bottom, anchor, bubble.bottom - inset);
        outline.AddLine(anchor, bubble.bottom - inset, tail - 3 * stroke, bottom);
        outline.AddArc(left, bottom - diameter, diameter, diameter, 90, 90);
        outline.CloseFigure();
        SolidBrush white(Color(255, 255, 255));
        Pen silver(Color(161, 167, 174), stroke);
        silver.SetLineJoin(LineJoinRound);
        canvas.FillPath(&white, &outline);
        canvas.DrawPath(&silver, &outline);
    } else {
        SelectObject(dc, GetStockObject(DC_BRUSH));
        SelectObject(dc, GetStockObject(DC_PEN));
        roundRect(dc, body, scale(8), RGB(255, 255, 255), RGB(161, 167, 174));
    }
    const HFONT font = CreateFontW(-scale(thoughtFontSize), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_ROMAN, L"KaiTi");
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(40, 44, 49));
    DrawTextW(dc, person_.thought, -1, &body, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RestoreDC(dc, saved);
    if (font) DeleteObject(font);
}

void App::updatePerson(bool clicked) {
    const RECT oldFigure = personBounds();
    const RECT oldPaint = personPaintBounds(oldFigure), oldBubble = thoughtBounds(oldFigure);
    const auto* oldText = person_.thought;
    if (clicked) person_.click(firstWholeCell());
    else person_.advance(firstWholeCell());
    const RECT figure = personBounds(), painted = personPaintBounds(figure), bubble = thoughtBounds(figure);
    RECT dirty{};
    if (UnionRect(&dirty, &oldPaint, &painted)) InvalidateRect(window_, &dirty, FALSE);
    if (oldText != person_.thought || !EqualRect(&oldBubble, &bubble)) {
        if (UnionRect(&dirty, &oldBubble, &bubble)) InvalidateRect(window_, &dirty, FALSE);
    }
}

void App::drawPerson(HDC target, RECT figure) {
    const RECT bounds = personPaintBounds(figure);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0 || bounds.right > faceSize_.cx || bounds.bottom > faceSize_.cy) return;
    const auto mask = pose(person_.activity, person_.frame);
    const int figureWidth = figure.right - figure.left, figureHeight = figure.bottom - figure.top;
    const auto occupied = [&](int col, int row) {
        if (col < 0 || col >= Silhouette::width || row < 0 || row >= Silhouette::height) return false;
        if (!person_.facingRight) col = Silhouette::width - 1 - col;
        return (mask.rows[row] & (1u << col)) != 0;
    };
    personPixels_.resize(static_cast<size_t>(width) * height);
    const int sill = scale(lampBottom);
    for (int y = bounds.top; y < bounds.bottom; ++y) {
        const DWORD* lamp = facePixels_ + y * faceSize_.cx + bounds.left;
        const double depth = std::max(0, y - sill) / static_cast<double>(bounds.bottom - sill);
        const double softness = 1.4 + 3.2 * depth;
        const int shadowRow = static_cast<int>((1.0 - depth) * (Silhouette::height - 1));
        for (int x = bounds.left; x < bounds.right; ++x) {
            int transmission = 256;
            if (y < figure.bottom && x >= figure.left && x < figure.right) {
                if (occupied((x - figure.left) * Silhouette::width / figureWidth,
                             (y - figure.top) * Silhouette::height / figureHeight)) transmission = 80;
            } else if (y >= sill) {
                // Project this pose feet-first onto the sill. The penumbra widens with distance.
                const double col = ((x + 0.5 - figure.left) / figureWidth - 0.5)
                                   * Silhouette::width / (1.0 + 0.27 * depth) + Silhouette::width / 2.0 - 0.5;
                double blocked = 0, total = 0;
                for (int tap = static_cast<int>(std::ceil(col - softness)); tap <= col + softness; ++tap) {
                    const double weight = 1.0 - std::abs(tap - col) / softness;
                    total += weight;
                    blocked += weight * (0.25 * occupied(tap, shadowRow - 1) + 0.5 * occupied(tap, shadowRow)
                                       + 0.25 * occupied(tap, shadowRow + 1));
                }
                transmission -= static_cast<int>(blocked / total * (0.70 - 0.30 * depth) * 256);
            }
            const DWORD color = lamp[x - bounds.left];
            const auto shade = [transmission](DWORD c) { return ((c & 255) * transmission + 128) >> 8; };
            personPixels_[(y - bounds.top) * width + x - bounds.left] =
                (shade(color >> 16) << 16) | (shade(color >> 8) << 8) | shade(color);
        }
    }
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    SetDIBitsToDevice(target, bounds.left, bounds.top, width, height, 0, 0, 0, height,
                      personPixels_.data(), &info, DIB_RGB_COLORS);
}

void App::paint(HDC target, const RECT* damage) {
    RECT client{};
    GetClientRect(window_, &client);
    if (IsRectEmpty(&client)) return;
    if (client.right != faceSize_.cx || client.bottom != faceSize_.cy) {
        clearFaceCache();
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), client.right, -client.bottom, 1, 32, BI_RGB};
        void* pixels = nullptr;
        faceDC_ = CreateCompatibleDC(target);
        faceBitmap_ = CreateDIBSection(target, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!faceDC_ || !faceBitmap_) {
            clearFaceCache();
            drawFace(target, client, preferences_.theme, elapsed_);
            return;
        }
        SelectObject(faceDC_, faceBitmap_);
        facePixels_ = static_cast<DWORD*>(pixels);
        faceSize_ = {client.right, client.bottom};
    }
    if (faceDirty_) {
        drawFace(faceDC_, client, preferences_.theme, elapsed_);
        GdiFlush();
        faceDirty_ = false;
    }
    const RECT dirty = damage ? *damage : client;
    BitBlt(target, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
           faceDC_, dirty.left, dirty.top, SRCCOPY);
    const RECT sprite = personBounds();
    const RECT painted = personPaintBounds(sprite);
    RECT overlap{};
    if (IntersectRect(&overlap, &dirty, &painted)) drawPerson(target, sprite);
    const RECT bubble = thoughtBounds(sprite);
    if (IntersectRect(&overlap, &dirty, &bubble)) drawThought(target, bubble, sprite);
}
void App::drawHideButton(const DRAWITEMSTRUCT& item) {
    using namespace Gdiplus;
    const RECT bounds = item.rcItem;
    fill(item.hDC, bounds, item.itemState & ODS_SELECTED ? RGB(79, 49, 48) : glassColor);
    if (!vectorGraphicsReady()) return;
    Graphics canvas(item.hDC);
    canvas.SetPageUnit(UnitPixel);
    canvas.SetSmoothingMode(SmoothingModeAntiAlias);
    canvas.SetPixelOffsetMode(PixelOffsetModeHalf);
    const REAL unit = dpi_ / 96.0f;
    Matrix transform(unit, 0, 0, unit, (bounds.left + bounds.right) * 0.5f,
                     (bounds.top + bounds.bottom) * 0.5f);
    canvas.SetTransform(&transform);
    Pen pen(Color(255, 171, 174, 176), 1.15f);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapFlat);
    canvas.DrawLine(&pen, -3.25f, -3.25f, 3.25f, 3.25f);
    canvas.DrawLine(&pen, -3.25f, 3.25f, 3.25f, -3.25f);
}

bool App::addTrayIcon() {
    if (hasTrayIcon_) return true;

    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = window_;
    icon.uID = 1;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    icon.uCallbackMessage = trayMessage;
    icon.hIcon = smallIcon_;
    wcscpy_s(icon.szTip, appName);
    hasTrayIcon_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    if (hasTrayIcon_) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        trayVersion4_ = Shell_NotifyIconW(NIM_SETVERSION, &icon) != FALSE;
    }
    return hasTrayIcon_;
}

void App::removeTrayIcon() {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = window_;
    icon.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    hasTrayIcon_ = false;
}

void App::useTaskbarButton(bool enabled) {
    const LONG_PTR style = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    const LONG_PTR replacement = (style & ~(WS_EX_TOOLWINDOW | WS_EX_APPWINDOW))
        | (enabled ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW);
    if (style == replacement) return;
    SetWindowLongPtrW(window_, GWL_EXSTYLE, replacement);
    SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void App::show() {
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    placeWindow(false);
    refresh(true);
    SetForegroundWindow(window_);
}

void App::hide() {
    rememberPosition();
    KillTimer(window_, tickTimer);
    KillTimer(window_, personTimer);
    const bool canUseTray = addTrayIcon();
    useTaskbarButton(!canUseTray);
    ShowWindow(window_, canUseTray ? SW_HIDE : SW_MINIMIZE);
}

void App::menu(POINT position, bool fromTray) {
    if (settingsOpen_) {
        const HWND popup = GetLastActivePopup(window_);
        SetForegroundWindow(popup);
        return;
    }
    HMENU popup = CreatePopupMenu();
    if (!popup) return;

    if (fromTray) AppendMenuW(popup, MF_STRING, menuShow, L"显示小窗");
    AppendMenuW(popup, MF_STRING | (preferences_.topmost ? MF_CHECKED : 0), menuTopmost, L"置顶");
    AppendMenuW(popup, MF_STRING, menuSettings, L"设置…");
    AppendMenuW(popup, MF_STRING, menuTheme, L"配色…");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    if (IsWindowVisible(window_)) AppendMenuW(popup, MF_STRING, menuHide, L"关闭小窗");
    AppendMenuW(popup, MF_STRING, menuExit, L"退出程序");

    SetForegroundWindow(window_);
    const UINT id = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                   position.x, position.y, 0, window_, nullptr);
    DestroyMenu(popup);
    PostMessageW(window_, WM_NULL, 0, 0);
    if (id) command(id);
}

void App::command(UINT id) {
    switch (id) {
    case menuShow:
        show();
        break;
    case IDC_HIDE:
    case menuHide:
        hide();
        break;
    case menuTopmost:
        preferences_.topmost = !preferences_.topmost;
        SetWindowPos(window_, preferences_.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        preferences_.save();
        break;
    case menuSettings:
        if (settingsOpen_) break;
        settingsOpen_ = true;
        if (editPreferences(instance_, window_, preferences_)) {
            refresh(true);
        }
        settingsOpen_ = false;
        break;
    case menuExit:
        rememberPosition();
        DestroyWindow(window_);
        break;
    case menuTheme: {
        if (settingsOpen_) break;
        settingsOpen_ = true;
        refresh(true, true);
        const INT_PTR choice = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_PALETTE), window_,
                                               paletteProcedure, reinterpret_cast<LPARAM>(this));
        settingsOpen_ = false;
        if (choice >= 1 && choice <= static_cast<INT_PTR>(themes.size())) {
            preferences_.theme = static_cast<int>(choice - 1);
            faceDirty_ = true;
            preferences_.save();
            if (IsWindowVisible(window_)) InvalidateRect(window_, nullptr, FALSE);
        }
        break;
    }
    }
}

INT_PTR CALLBACK App::paletteProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        auto* app = reinterpret_cast<App*>(lParam);
        for (const auto& theme : themes) {
            SendDlgItemMessageW(dialog, IDC_THEME, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(theme.name));
        }
        SendDlgItemMessageW(dialog, IDC_THEME, CB_SETCURSEL, app->preferences_.theme, 0);
        return TRUE;
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!app) return FALSE;
    const auto selected = [&] {
        return static_cast<int>(SendDlgItemMessageW(dialog, IDC_THEME, CB_GETCURSEL, 0, 0));
    };
    if (message == WM_DRAWITEM && wParam == IDC_DAY_VIEW) {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        const RECT face{0, 0, app->scale(baseWidth), app->scale(baseHeight)};
        HDC buffer = CreateCompatibleDC(item.hDC);
        HBITMAP bitmap = CreateCompatibleBitmap(item.hDC, face.right, face.bottom);
        if (buffer && bitmap) {
            const auto old = SelectObject(buffer, bitmap);
            app->drawFace(buffer, face, std::max(0, selected()), 0);
            const RECT box = item.rcItem;
            SetStretchBltMode(item.hDC, HALFTONE);
            SetBrushOrgEx(item.hDC, 0, 0, nullptr);
            StretchBlt(item.hDC, box.left, box.top, box.right - box.left, box.bottom - box.top,
                       buffer, 0, 0, face.right, face.bottom, SRCCOPY);
            SelectObject(buffer, old);
        }
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        return TRUE;
    }
    if (message == WM_CLOSE) {
        EndDialog(dialog, 0);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IDC_THEME:
            if (HIWORD(wParam) == CBN_SELCHANGE) InvalidateRect(GetDlgItem(dialog, IDC_DAY_VIEW), nullptr, FALSE);
            return TRUE;
        case IDOK:
            EndDialog(dialog, selected() + 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, 0);
            return TRUE;
        }
    }
    return FALSE;
}

LRESULT App::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == taskbarCreated_) {
        hasTrayIcon_ = false;
        const bool canUseTray = addTrayIcon();
        useTaskbarButton(!canUseTray);
        if (canUseTray && IsIconic(window_)) ShowWindow(window_, SW_HIDE);
        if (!canUseTray && !IsWindowVisible(window_)) ShowWindow(window_, SW_SHOWMINNOACTIVE);
        return 0;
    }
    switch (message) {
    case WM_CREATE:
        dpi_ = GetDpiForWindow(window_);
        createControls();
        return 0;
    case WM_WINDOWPOSCHANGED:
        syncShadow();
        break;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wParam);
        const RECT& bounds = *reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window_, nullptr, bounds.left, bounds.top, scale(baseWidth), scale(baseHeight),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        updateGeometry();
        return 0;
    }
    case WM_DISPLAYCHANGE:
        if (!IsIconic(window_)) placeWindow(false);
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            KillTimer(window_, tickTimer);
            KillTimer(window_, personTimer);
        } else if (wParam == SIZE_RESTORED && IsWindowVisible(window_)) {
            refresh();
        }
        return 0;
    case WM_EXITSIZEMOVE:
        placeWindow(false);
        rememberPosition();
        return 0;
    case WM_NCHITTEST: {
        RECT closeBounds{};
        const POINT pointer{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (GetWindowRect(hideButton_, &closeBounds) && PtInRect(&closeBounds, pointer)) return HTCLIENT;
        POINT client = pointer;
        if (ScreenToClient(window_, &client) && hitPerson(client)) return HTCLIENT;
        return HTCAPTION;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (hitPerson({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)})) updatePerson(true);
        return 0;
    case WM_NCLBUTTONDBLCLK:
        return 0;
    case WM_NCRBUTTONUP:
        menu({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, false);
        return 0;
    case WM_CONTEXTMENU: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (point.x == -1 && point.y == -1) {
            point = {scale(20), scale(80)};
            ClientToScreen(window_, &point);
        }
        menu(point, false);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paintInfo{};
        HDC dc = BeginPaint(window_, &paintInfo);
        paint(dc, &paintInfo.rcPaint);
        EndPaint(window_, &paintInfo);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint(reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_DRAWITEM:
        if (wParam == IDC_HIDE) {
            drawHideButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        command(LOWORD(wParam));
        return 0;
    case WM_TIMER:
        if (wParam == tickTimer) refresh();
        if (wParam == personTimer && IsWindowVisible(window_) && !IsIconic(window_)) {
            updatePerson(false);
        }
        return 0;
    case WM_TIMECHANGE:
    case WM_SETTINGCHANGE:
        refresh(true);
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC) refresh(true);
        return TRUE;
    case restoreMessage:
        show();
        return 0;
    case trayMessage:
        if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT
            || (!trayVersion4_ && LOWORD(lParam) == WM_LBUTTONUP)) {
            show();
        } else if (LOWORD(lParam) == WM_CONTEXTMENU || (!trayVersion4_ && LOWORD(lParam) == WM_RBUTTONUP)) {
            POINT point{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
            if (!trayVersion4_ || (point.x == -1 && point.y == -1)) GetCursorPos(&point);
            menu(point, true);
        }
        return 0;
    case WM_CLOSE:
        hide();
        return 0;
    case WM_DESTROY:
        clearFaceCache();
        KillTimer(window_, tickTimer);
        KillTimer(window_, personTimer);
        removeTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT CALLBACK App::procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* app = static_cast<App*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return app ? app->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HANDLE singleton = CreateMutexW(nullptr, FALSE, instanceName);
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const HWND existing = FindWindowW(windowClass, nullptr);
        if (existing) {
            DWORD process{};
            GetWindowThreadProcessId(existing, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(existing, restoreMessage, 0, 0);
        }
        CloseHandle(singleton);
        return 0;
    }

    WNDCLASSEXW windowType{sizeof(windowType)};
    windowType.style = CS_DBLCLKS;
    windowType.lpfnWndProc = App::procedure;
    windowType.hInstance = instance;
    windowType.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_COUNTDOWN));
    windowType.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_COUNTDOWN), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    windowType.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowType.lpszClassName = windowClass;

    App app(instance, windowType.hIconSm);
    if (!RegisterClassExW(&windowType) || !app.create()) {
        MessageBoxW(nullptr, L"无法打开倒计时小窗。", appName, MB_OK | MB_ICONERROR);
        CloseHandle(singleton);
        return 1;
    }

    MSG message{};
    BOOL result = FALSE;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CloseHandle(singleton);
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}

