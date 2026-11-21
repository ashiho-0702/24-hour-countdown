#pragma once

#include "clock.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace countdown {

enum class Activity { walk, run, read, type, eat, stretch, sleep, game, dance, fall };

struct Silhouette {
    static constexpr int width = 14, height = 20;
    std::array<unsigned, height> rows{};

    void block(int x, int y, int w, int h) {
        for (int row = y; row < y + h; ++row)
            rows[row] |= ((1u << w) - 1) << x;
    }

    void limb(int x, int y, int endX, int endY, int thickness = 1) {
        const int steps = std::max({1, std::abs(endX - x), std::abs(endY - y)});
        for (int i = 0; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            block(x + static_cast<int>(std::lround((endX - x) * t)),
                  y + static_cast<int>(std::lround((endY - y) * t)), thickness, 1);
        }
    }
};

inline Silhouette pose(Activity action, unsigned frame) {
    Silhouette s;
    const int phase = static_cast<int>(frame % 8);
    if (action == Activity::fall) {
        if (frame < 3) {                      // Pitch forward, arms out.
            s.block(8, 6, 4, 4); s.limb(8, 10, 4, 14, 3);
            s.limb(8, 11, 12, 13, 2); s.limb(4, 14, 1, 12, 2);
            s.limb(5, 14, 4, 18, 2);
        } else if (frame < 10) {              // Land flat with one knee bent.
            s.block(9, 14, 4, 4); s.block(4, 16, 6, 3);
            s.limb(5, 17, 1, 15, 2); s.limb(1, 15, 0, 13, 2);
            s.block(0, 18, 6, 1); s.block(10, 18, 4, 1);
        } else {                              // Push off the floor.
            s.block(7, 9, 4, 4); s.limb(7, 13, 3, 16, 3);
            s.limb(7, 13, 10, 18, 2); s.limb(4, 16, 2, 18, 2);
            s.block(0, 18, 5, 1);
        }
        return s;
    }
    if (action == Activity::sleep) {
        const int breath = (frame / 7) % 2;
        s.block(1, 12, 3, 3);                 // Head on the pillow.
        s.block(4, 14 - breath, 5, 2 + breath);
        s.block(9, 15, 4, 1);
        s.block(0, 16, 14, 1);                // Cot and legs.
        s.block(0, 13, 1, 6); s.block(12, 17, 1, 2);
        const int z = 7 - static_cast<int>((frame / 4) % 3);
        s.block(5, z, 3, 1); s.block(6, z + 1, 1, 1); s.block(5, z + 2, 3, 1);
        return s;
    }
    const bool moving = action == Activity::walk || action == Activity::run;
    const bool seated = action == Activity::type || action == Activity::eat || action == Activity::game;
    const int bob = (moving || action == Activity::dance) ? (phase % 4 < 2 ? 0 : 1) : (seated ? 2 : 0);
    s.block(4, 1 + bob, 3, 1);
    s.block(3, 2 + bob, 5, 3);
    s.block(4, 5 + bob, 3, 1);
    s.block(5, 6 + bob, 1, 1);
    s.block(3, 7 + bob, 4, 6);
    if (moving) {
        constexpr int stride[]{0, 1, 2, 2, 0, -1, -2, -2};
        const int swing = stride[phase];
        const int reach = action == Activity::run ? 2 * swing : swing;
        s.limb(4, 12 + bob, 4 + reach, 18 - bob, 2);
        s.limb(5, 12 + bob, 6 - reach, 17, 2);
        s.block(4 + reach, 18 - bob, 3, 1);
        s.limb(4, 8 + bob, 4 - swing, 10 + bob, 2);
        s.limb(4 - swing, 10 + bob, 4 - 2 * swing, 11 + bob, 2);
        s.limb(6, 8 + bob, 7 + swing, 11 + bob);
        s.block(7 + swing, 10 + bob, 2, 2);
    } else if (action == Activity::dance) {
        const bool beat = phase < 4;
        s.limb(3, 8 + bob, 1, beat ? 4 : 11, 2);
        s.limb(6, 8 + bob, 10, beat ? 11 : 4, 2);
        s.limb(4, 12 + bob, beat ? 1 : 4, beat ? 15 : 18, 2);
        s.limb(6, 12 + bob, beat ? 7 : 10, beat ? 18 : 15, 2);
        s.block(beat ? 0 : 10, 15, 3, 1);
    } else if (seated) {
        s.block(2, 15, 5, 1); s.block(2, 13, 1, 6); // Stool and bent legs.
        s.block(5, 14, 4, 2); s.block(8, 15, 2, 4); s.block(8, 18, 3, 1);
        if (action == Activity::game) {
            const int y = 8 + (phase >= 5 ? 1 : 0);
            s.limb(3, 10, 2, 13);
            s.limb(2, 13, 4, y + 2);
            s.limb(6, 9, 10, y - 1);
            s.limb(10, y - 1, 12, y + 1);
            // Front of the gamepad. The cutouts let the lamp show through
            // the D-pad and face buttons; the lower notch separates the grips.
            constexpr unsigned pad[] = {
                0b01110001110, 0b11101111111, 0b11000111011,
                0b11101111101, 0b11111111111, 0b11100000111,
                0b11000000011
            };
            for (int row = 0; row < 7; ++row) {
                unsigned bits = 0;
                for (int x = 0; x < 11; ++x) bits |= ((pad[row] >> (10 - x)) & 1u) << (x + 3);
                s.rows[y + row] = (s.rows[y + row] & ~(0x7ffu << 3)) | bits;
            }
            if (phase % 4 < 2) s.block(11, y + 2, 1, 1);
            return s;
        }
        s.block(8, 12, 6, 1); s.block(12, 13, 1, 6);
        if (action == Activity::type) {
            s.limb(11, 11, 13, 7); s.block(8, 11, 4, 1);
            s.limb(6, 9, 8, 11); s.block(8, phase % 4 < 2 ? 9 : 10, 2, 1);
        } else {
            s.block(10, 10, 4, 1); s.block(11, 11, 2, 1);
            constexpr int handX[]{10, 10, 9, 8, 7, 7, 8, 9};
            constexpr int handY[]{10, 10, 9, 8, 7, 7, 8, 9};
            s.limb(6, 9, 7, 11); s.limb(7, 11, handX[phase], handY[phase]);
            s.block(handX[phase], handY[phase], 2, 1);
        }
    } else {
        s.limb(4, 13, 3, 18, 2); s.limb(5, 13, 7, 18, 2);
        if (action == Activity::read) {
            s.limb(5, 8, 7, 11); s.limb(7, 11, 9, 10);
            s.block(8, 8, 2, 3); s.block(10, 9, 1, 3);
            s.block(11, 8, 2, 3); s.block(phase < 4 ? 12 : 9, 7, 1, 1);
        } else {
            constexpr int lift[]{6, 5, 4, 3, 2, 3, 4, 5};
            s.limb(3, 8, 1, lift[phase], 2); s.limb(6, 8, 9, lift[phase], 2);
        }
    }
    return s;
}

class BusyPerson {
public:
    // Position is measured in cells. Integers denote cell centers.
    double position = -1;
    Activity activity = Activity::walk;
    unsigned frame = 0;
    bool facingRight = true;
    const wchar_t* thought = nullptr;

    void click(int first) {
        if (position < 0 || first >= cellCount) return;
        if (activity == Activity::walk) {
            facingRight = !facingRight;
            const int steps = pick(3, 8);
            target = std::clamp(static_cast<int>(std::lround(position)) + (facingRight ? steps : -steps),
                                first, cellCount - 1);
            remaining = 0;
            frame = 0;
            return;
        }
        if (activity == Activity::run) {
            activity = Activity::fall;
            frame = 0;
            return;
        }
        struct Thoughts { Activity action; const wchar_t* lines[3]; };
        static constexpr Thoughts lines[]{
            {Activity::sleep,   {L"ZZZ～", L"再睡五分钟……", L"梦里已经起床了。"}},
            {Activity::read,    {L"他们什么时候在一起？", L"都三百页了，还在误会！", L"再看一章。就一章。"}},
            {Activity::eat,     {L"今天吃顿好的！", L"这口留给明天的我。啊呜。", L"吃饱才有力气想晚饭。"}},
            {Activity::type,    {L"刚才那版存了吗？", L"密码提示：你知道的。", L"撤销！刚才删多了！"}},
            {Activity::stretch, {L"咔哒。嗯，是我的腰。", L"左边拉完，右边吃醋了。", L"就差一点够到脚尖！"}},
            {Activity::game,    {L"刚才那下手柄没反应！", L"再来一局就洗碗。", L"谁把跳跃键改了？"}},
            {Activity::dance,   {L"慢半拍也是我的编舞。", L"脚会了，手还在学。", L"楼下应该没人吧？"}},
            {Activity::fall,    {L"地板挺结实。", L"鞋带！我明明没穿鞋。", L"这一跤算个俯卧撑。"}},
        };
        for (const auto& entry : lines) {
            if (entry.action != activity) continue;
            auto& previous = lastLine[static_cast<size_t>(activity)];
            const int index = previous ? (previous - 1 + pick(1, 2)) % 3 : 0;
            thought = entry.lines[index];
            previous = index + 1;
            thoughtTicks = 26;
            remaining = std::max(remaining, thoughtTicks);
            break;
        }
    }

    void fit(int first) {
        if (first >= cellCount) { position = -1; thought = nullptr; thoughtTicks = 0; return; }
        if (position < 0) {
            position = pick(first, cellCount - 1);
            target = static_cast<int>(position);
            activity = Activity::walk;
            crossRange = true;
            remaining = 0;
        }
        position = std::max(position, static_cast<double>(first));
        target = std::clamp(target, first, cellCount - 1);
    }

    void advance(int first) {
        fit(first);
        if (position < 0) return;
        ++frame;
        if (thoughtTicks > 0 && --thoughtTicks == 0) thought = nullptr;
        if (activity == Activity::fall) {
            if (frame >= 14) {
                activity = Activity::run;
                frame = 0;
                remaining = 0;
                thought = nullptr;
                thoughtTicks = 0;
            }
            return;
        }
        if (remaining > 0) { --remaining; return; }
        if (activity == Activity::walk || activity == Activity::run) {
            const double delta = target - position;
            if (std::abs(delta) > 0.001) {
                facingRight = delta > 0;
                position += std::copysign(std::min(std::abs(delta), activity == Activity::run ? 0.22 : 0.095), delta);
                return;
            }
        }
        const int current = static_cast<int>(std::lround(position));
        const bool arrived = activity == Activity::walk || activity == Activity::run;
        if (first < cellCount - 1 && !arrived) {
            if (crossRange) {
                const int middle = (first + cellCount - 1) / 2;
                target = current <= middle ? pick(middle + 1, cellCount - 1) : pick(first, middle);
            } else {
                const int low = std::max(first, current - 6);
                const int high = std::min(cellCount - 1, current + 6);
                target = pick(low, high - 1);
                if (target >= current) ++target;
            }
            crossRange = !crossRange;
            activity = pick(0, 4) == 0 ? Activity::run : Activity::walk;
            facingRight = target > current;
        } else {
            // Change pose even when there is room for just one stationary activity.
            constexpr Activity stops[]{Activity::read, Activity::type, Activity::eat, Activity::stretch,
                                       Activity::sleep, Activity::game, Activity::dance};
            int choice = pick(0, static_cast<int>(std::size(stops)) - 1);
            if (stops[choice] == activity) choice = (choice + 1) % std::size(stops);
            activity = stops[choice];
            remaining = pick(22, 48);
        }
        frame = 0;
    }

private:
    int pick(int low, int high) { return std::uniform_int_distribution<int>(low, high)(random); }
    std::minstd_rand random{static_cast<unsigned>(GetTickCount64())};
    int target = cellCount - 1;
    int remaining = 0;
    int thoughtTicks = 0;
    std::array<int, static_cast<size_t>(Activity::fall) + 1> lastLine{};
    bool crossRange = true;
};

} // namespace countdown
