#include "clock.h"
#include "resources.h"
#include "settings.h"

#include <windowsx.h>
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
constexpr int baseWidth = 746;
constexpr int baseHeight = 115;
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

COLORREF sampleLens(double cell, double u, double v) {
    const int first = static_cast<int>(cell);
    const int column = std::min(3, static_cast<int>(u * 4));
    const int row = std::min(7, static_cast<int>(v * 8));
    const auto sample = [&](int index) {
        const auto& grid = lensSamples[index];
        const double x = u * 4 - column;
        return blend(blend(grid[row][column], grid[row][column + 1], x),
                     blend(grid[row + 1][column], grid[row + 1][column + 1], x), v * 8 - row);
    };
    const COLORREF tint = blend(sample(first), sample((first + 1) % cellCount), cell - first);
    const double blue = std::clamp((GetBValue(tint) - GetRValue(tint)) / 24.0, 0.0, 1.0);
    const double shadows = std::clamp((150 - GetBValue(tint)) / 120.0, 0.0, 1.0);
    const double core = 1.0 - (u - 0.5) * (u - 0.5) - (v - 0.4) * (v - 0.4);
    return blend(tint, RGB(62, 112, 166), blue * shadows * (0.58 + 0.30 * core));
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
    void rememberPosition();
    void refresh(bool rebuild = false, bool forPreview = false);
    void paint(HDC target);
    void drawFace(HDC dc, const RECT& client, int theme, double elapsed);
    void drawLamps(HDC dc, int trackStart, int trackWidth, int theme, double elapsed,
                   const std::vector<DWORD>& metal, const RECT& client);
    static INT_PTR CALLBACK paletteProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    void drawSolarMarks(HDC dc, int trackStart, int trackWidth);
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
    UINT dpi_ = 96;
    UINT taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    Preferences preferences_;
    std::optional<Day> day_;
    DYNAMIC_TIME_ZONE_INFORMATION zone_{};
    SYSTEMTIME localDate_{};
    double elapsed_ = 0;
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
    useTaskbarButton(!addTrayIcon());
    show();
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

void App::updateGeometry() {
    RECT client{};
    GetClientRect(window_, &client);
    const HRGN corners = CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1, scale(21), scale(21));
    if (corners && !SetWindowRgn(window_, corners, TRUE)) DeleteObject(corners);
    SetWindowPos(hideButton_, nullptr, scale(713), scale(11), scale(19), scale(17), SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(window_, nullptr, FALSE);
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
        localDate_ = local;
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
    if (rebuild || newlyElapsed != elapsed_) InvalidateRect(window_, nullptr, FALSE);
    elapsed_ = newlyElapsed;

    if (!visible) return;
    const Instant untilChange = ticksPerMinute - now % ticksPerMinute;
    const Instant milliseconds = (untilChange + ticksPerMillisecond - 1) / ticksPerMillisecond;
    SetTimer(window_, tickTimer, static_cast<UINT>(std::clamp<Instant>(milliseconds, 50, 1'800'000)), nullptr);
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
    if (theme != spectrumTheme) drawSolarMarks(dc, trackStart, trackWidth);

    const int digitWidth = scale(6);
    const int digitGap = scale(2);
    const int digitTop = scale(31);
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
    const int top = scale(69);
    const int bottom = scale(95);
    const int halo = std::max(1, scale(7));
    const auto tickX = [trackStart, trackWidth](int tick) {
        return trackStart + MulDiv(trackWidth, tick, cellCount);
    };
    const bool reference = day_ && theme == 0 && day_->sunrise >= 0 && day_->sunset >= 0;
    const auto colorAt = [&](int x) {
        const double minute = (x - trackStart + 0.5) * minutesPerDay / trackWidth;
        if (reference) return sampleLens(referenceCell(*day_, minute), 0.5, 0.45);
        return day_->color(minute, theme);
    };

    roundRect(dc, {trackStart - scale(3), top - scale(2), tickX(cellCount) + scale(3),
                  bottom + scale(2)}, scale(5), RGB(5, 7, 9), RGB(3, 4, 6));
    const int lightStart = trackStart + static_cast<int>(std::lround(trackWidth * elapsed / cellCount));
    if (day_) {
        for (int x = lightStart; x < std::min<int>(client.right, tickX(cellCount)); ++x) {
            const COLORREF tint = colorAt(x);
            const double strength = 0.40 + warmth(tint) * 0.22;
            const double onset = std::clamp((x - lightStart + 0.5) / scale(6), 0.0, 1.0);
            for (int y = scale(105); y < std::min<int>(client.bottom, scale(114)); ++y) {
                const DWORD base = metal[static_cast<size_t>(y) * client.right + x];
                const COLORREF steel = RGB(GetBValue(base), GetGValue(base), GetRValue(base));
                const double distance = (y - scale(108)) / static_cast<double>(scale(3));
                const double reflection = onset * strength * std::exp(-distance * distance);
                SetPixelV(dc, x, y, blend(steel, tint, reflection));
            }
            for (int band = 0; band < halo; ++band) {
                const double fade = 1.0 - static_cast<double>(band) / halo;
                const COLORREF glow = blend(glassColor, tint, strength * fade * fade);
                SetPixelV(dc, x, top - 1 - band, glow);
                SetPixelV(dc, x, bottom + band, glow);
            }
        }
    }

    for (int cell = 0; cell < cellCount; ++cell) {
        const int left = tickX(cell) + std::max(1, scale(1));
        const int right = tickX(cell + 1);
        const RECT lens{left, top, right, bottom};
        if (!day_ || right <= lightStart) {
            roundRect(dc, lens, scale(3), RGB(3, 5, 7), RGB(14, 16, 20));
            continue;
        }
        const COLORREF mid = colorAt((left + right) / 2);
        roundRect(dc, lens, scale(3), blend(glassColor, mid, 0.63), blend(glassColor, mid, 0.46));
        const int width = right - left;
        const int height = bottom - top;
        if (reference) {
            const double minute = (cell + 0.5) * minutesPerCell;
            const double position = referenceCell(*day_, minute);
            for (int y = top + 1; y < bottom - 1; ++y) {
                const double v = (y - top + 0.5) / height;
                for (int x = left + 1; x < right - 1; ++x) {
                    const double u = (x - left + 0.5) / width;
                    SetPixelV(dc, x, y, sampleLens(position, u, v));
                }
            }
            continue;
        }
        for (int x = left + 1; x < right - 1; ++x) {
            const COLORREF tint = colorAt(x);
            const double warm = warmth(tint);
            const double side = std::abs(2.0 * (x - left + 0.5) / width - 1.0);
            const COLORREF highlight = blend(RGB(179, 225, 255), RGB(255, 249, 215), warm);
            const COLORREF center = blend(tint, highlight, theme == spectrumTheme ? 0.10 : 0.62);
            const COLORREF edge = blend(glassColor, tint, 0.88);
            for (int y = top + 1; y < bottom - 1; ++y) {
                const double position = static_cast<double>(y - top) / height;
                const double distance = (position - 0.26) / 0.62;
                const double falloff = std::clamp(0.52 * distance * distance + 0.25 * side * side, 0.0, 1.0);
                SetPixelV(dc, x, y, blend(center, edge, falloff));
            }
        }
    }
    const int active = static_cast<int>(elapsed);
    if (active < cellCount && elapsed > active) {
        const int left = tickX(active) + std::max(1, scale(1));
        if (left < lightStart) fill(dc, {left, top, lightStart, bottom}, RGB(3, 5, 7));
    }
}

void App::drawSolarMarks(HDC dc, int trackStart, int trackWidth) {
    if (!day_ || (day_->sunrise < 0 && day_->sunset < 0)) return;
    const HFONT font = CreateFontW(-scale(11), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, L"Microsoft YaHei UI");
    const HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(232, 194, 139));
    const auto mark = [&](double minute, const wchar_t* text) {
        if (minute < 0) return;
        const int x = std::clamp(trackStart + static_cast<int>(minute * trackWidth / minutesPerDay),
                                 trackStart + scale(20), trackStart + trackWidth - scale(34));
        RECT title{x - scale(6), scale(11), x + scale(21), scale(28)};
        DrawTextW(dc, text, -1, &title, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

        const int icon = x - scale(14);
        SetDCPenColor(dc, RGB(240, 183, 101));
        SetDCBrushColor(dc, RGB(240, 183, 101));
        Pie(dc, icon - scale(4), scale(17), icon + scale(4), scale(25),
            icon + scale(5), scale(21), icon - scale(5), scale(21));
        line(dc, icon - scale(6), scale(23), icon + scale(6), scale(23));
        line(dc, icon, scale(14), icon, scale(16));
    };
    const bool closeTogether = day_->sunrise >= 0 && day_->sunset >= 0
        && std::abs(day_->sunrise - day_->sunset) * trackWidth / minutesPerDay < scale(47);
    if (!closeTogether) {
        mark(day_->sunrise, L"日出");
        mark(day_->sunset, L"日落");
    }
    if (previous) SelectObject(dc, previous);
    if (font) DeleteObject(font);
}

void App::paint(HDC target) {
    RECT client{};
    GetClientRect(window_, &client);
    const HDC buffer = CreateCompatibleDC(target);
    const HBITMAP bitmap = buffer ? CreateCompatibleBitmap(target, client.right, client.bottom) : nullptr;
    if (bitmap) {
        const auto original = SelectObject(buffer, bitmap);
        drawFace(buffer, client, preferences_.theme, elapsed_);
        BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, original);
        DeleteObject(bitmap);
    } else {
        drawFace(target, client, preferences_.theme, elapsed_);
    }
    if (buffer) DeleteDC(buffer);
}

void App::drawHideButton(const DRAWITEMSTRUCT& item) {
    const RECT bounds = item.rcItem;
    const auto pen = SelectObject(item.hDC, GetStockObject(DC_PEN));
    fill(item.hDC, bounds, item.itemState & ODS_SELECTED ? RGB(79, 49, 48) : glassColor);
    SetDCPenColor(item.hDC, RGB(160, 163, 166));
    const int x = (bounds.left + bounds.right) / 2;
    const int y = (bounds.top + bounds.bottom) / 2;
    const int radius = scale(3);
    line(item.hDC, x - radius, y - radius, x + radius + 1, y + radius + 1);
    line(item.hDC, x - radius, y + radius, x + radius + 1, y - radius - 1);
    SelectObject(item.hDC, pen);
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
    refresh(true);
    SetForegroundWindow(window_);
}

void App::hide() {
    rememberPosition();
    KillTimer(window_, tickTimer);
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
    AppendMenuW(popup, MF_STRING, menuSettings, L"时区与日照地点…");
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
            preferences_.save();
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
        } else if (wParam == SIZE_RESTORED && IsWindowVisible(window_)) {
            refresh();
        }
        return 0;
    case WM_EXITSIZEMOVE:
        rememberPosition();
        return 0;
    case WM_NCHITTEST: {
        RECT closeBounds{};
        const POINT pointer{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (GetWindowRect(hideButton_, &closeBounds) && PtInRect(&closeBounds, pointer)) return HTCLIENT;
        return HTCAPTION;
    }
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
        paint(BeginPaint(window_, &paintInfo));
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
        KillTimer(window_, tickTimer);
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

