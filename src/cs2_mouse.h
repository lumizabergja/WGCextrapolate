#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace cs2 {
struct Pose {
    std::int64_t x{}, y{};
    bool valid{};
};

struct Rotation {
    float sinYaw{}, cosYaw{1}, sinPitch{}, cosPitch{1};
    bool operator==(const Rotation&) const = default;
    bool moved() const { return sinYaw != 0 || sinPitch != 0; }
};

// Positive raw X/Y turns the view right/down. The shader maps a destination
// ray back to the source image. Use the player's unscoped CS2 sensitivity.
inline Rotation fromCounts(double x, double y, double maxDegrees) {
    if (x == 0 && y == 0) return {};
    constexpr double degreesPerCount = .022 * 1.5;
    constexpr double radiansPerDegree = 3.14159265358979323846 / 180.;
    double length = std::hypot(x, y) * degreesPerCount;
    double scale = std::min(1., maxDegrees / length) * degreesPerCount * radiansPerDegree;
    return {float(std::sin(x * scale)), float(std::cos(x * scale)),
            float(std::sin(y * scale)), float(std::cos(y * scale))};
}

struct CameraMotion {
    Rotation live, previous;
};

class MouseHistory {
    struct Sample { std::int64_t time; Pose pose; };
    std::array<Sample, 512> samples{};
    std::size_t next{}, count{};
    const Sample& recent(std::size_t age) const {
        return samples[(next + samples.size() - 1 - age) % samples.size()];
    }
public:
    bool active() const { return count != 0; }
    void clear() { next = count = 0; }
    void record(std::int64_t time, std::int64_t x, std::int64_t y) {
        samples[next] = {time, {x, y, true}};
        next = (next + 1) % samples.size();
        count = std::min(count + 1, samples.size());
    }
    Pose at(std::int64_t time) const {
        for (std::size_t age = 0; age < count; ++age)
            if (recent(age).time <= time) return recent(age).pose;
        return {};
    }
    Rotation since(Pose source, std::int64_t now, std::int64_t frequency, bool predict) const {
        if (!active() || !source.valid) return {};
        const auto& last = recent(0);
        double x = double(last.pose.x - source.x), y = double(last.pose.y - source.y);
        if (predict && count >= 4 && frequency > 0) {
            const auto& middle = recent(1);
            const auto& first = recent(2);
            const double msPerTick = 1000. / double(frequency);
            const double dt = double(last.time - middle.time) * msPerTick;
            const double priorDt = double(middle.time - first.time) * msPerTick;
            const double age = double(now - last.time) * msPerTick;
            if (age >= 0 && age < 1.5 && dt >= .5 && dt <= 2 && priorDt >= .5 && priorDt <= 2) {
                double vx = double(last.pose.x - middle.pose.x) / dt;
                double vy = double(last.pose.y - middle.pose.y) / dt;
                double px = double(middle.pose.x - first.pose.x) / priorDt;
                double py = double(middle.pose.y - first.pose.y) / priorDt;
                double speed2 = vx * vx + vy * vy, prior2 = px * px + py * py;
                double dot = px * vx + py * vy;
                if (dot > 0 && dot * dot >= .95 * .95 * speed2 * prior2 &&
                    speed2 >= .64 * prior2 && speed2 <= 1.5625 * prior2 &&
                    vx * px >= 0 && vy * py >= 0) {
                    // Fade the optional lead if a new 1000 Hz report is overdue.
                    const double leadMs = std::min(1., 1.5 - age);
                    x += vx * leadMs;
                    y += vy * leadMs;
                }
            }
        }
        return fromCounts(x, y, 8.);
    }
};
}
