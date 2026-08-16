#include "RoundedDashGeometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace desto::presentation {

double SampleRoundedDashCoverage(
    const RoundedDashSpec& spec,
    double x,
    double y) noexcept {
    if (!std::isfinite(spec.width) || !std::isfinite(spec.height)
        || !std::isfinite(spec.radius) || !std::isfinite(spec.strokeWidth)
        || !std::isfinite(spec.nominalPeriod) || !std::isfinite(spec.dashFraction)
        || !std::isfinite(x) || !std::isfinite(y)
        || spec.width <= 0.0 || spec.height <= 0.0 || spec.strokeWidth <= 0.0
        || spec.nominalPeriod <= 0.0 || spec.dashFraction <= 0.0
        || spec.dashFraction >= 1.0) {
        return 0.0;
    }

    const auto inset = spec.strokeWidth / 2.0;
    const auto left = inset;
    const auto top = inset;
    const auto right = spec.width - inset;
    const auto bottom = spec.height - inset;
    if (right <= left || bottom <= top) return 0.0;

    const auto radius = std::clamp(
        spec.radius - inset,
        0.0,
        std::min((right - left) / 2.0, (bottom - top) / 2.0));
    const auto horizontal = std::max(0.0, right - left - radius * 2.0);
    const auto vertical = std::max(0.0, bottom - top - radius * 2.0);
    const auto quarterArc = std::numbers::pi * radius / 2.0;
    const auto perimeter = horizontal * 2.0 + vertical * 2.0 + quarterArc * 4.0;
    if (perimeter <= 0.0) return 0.0;

    struct NearestPoint {
        double distance = std::numeric_limits<double>::infinity();
        double pathPosition = 0.0;
    } nearest;
    const auto consider = [&](double pointX, double pointY, double pathPosition) {
        const auto distance = std::hypot(x - pointX, y - pointY);
        if (distance < nearest.distance) {
            nearest = {distance, pathPosition};
        }
    };
    const auto considerHorizontal = [&](
        double startX, double endX, double lineY, double pathStart, bool reverse) {
        const auto pointX = std::clamp(x, startX, endX);
        const auto offset = reverse ? endX - pointX : pointX - startX;
        consider(pointX, lineY, pathStart + offset);
    };
    const auto considerVertical = [&](
        double startY, double endY, double lineX, double pathStart, bool reverse) {
        const auto pointY = std::clamp(y, startY, endY);
        const auto offset = reverse ? endY - pointY : pointY - startY;
        consider(lineX, pointY, pathStart + offset);
    };
    const auto considerArc = [&](
        double centerX,
        double centerY,
        double startAngle,
        double endAngle,
        double pathStart) {
        if (radius <= 0.0) return;
        auto angle = std::atan2(y - centerY, x - centerX);
        if (angle < 0.0) angle += std::numbers::pi * 2.0;
        if (startAngle >= std::numbers::pi * 1.5 && angle < startAngle) {
            angle += std::numbers::pi * 2.0;
        }
        angle = std::clamp(angle, startAngle, endAngle);
        consider(
            centerX + std::cos(angle) * radius,
            centerY + std::sin(angle) * radius,
            pathStart + (angle - startAngle) * radius);
    };

    auto path = 0.0;
    considerHorizontal(left + radius, right - radius, top, path, false);
    path += horizontal;
    considerArc(
        right - radius, top + radius,
        std::numbers::pi * 1.5, std::numbers::pi * 2.0, path);
    path += quarterArc;
    considerVertical(top + radius, bottom - radius, right, path, false);
    path += vertical;
    considerArc(
        right - radius, bottom - radius,
        0.0, std::numbers::pi / 2.0, path);
    path += quarterArc;
    considerHorizontal(left + radius, right - radius, bottom, path, true);
    path += horizontal;
    considerArc(
        left + radius, bottom - radius,
        std::numbers::pi / 2.0, std::numbers::pi, path);
    path += quarterArc;
    considerVertical(top + radius, bottom - radius, left, path, true);
    path += vertical;
    considerArc(
        left + radius, top + radius,
        std::numbers::pi, std::numbers::pi * 1.5, path);

    auto dashCount = std::max(4, static_cast<int>(std::lround(
        perimeter / spec.nominalPeriod)));
    dashCount = std::max(4, static_cast<int>(std::lround(dashCount / 4.0)) * 4);
    const auto period = perimeter / dashCount;
    const auto dashLength = period * spec.dashFraction;
    const auto phase = dashLength / 2.0 - horizontal / 2.0;
    auto dashPosition = std::fmod(nearest.pathPosition + phase, period);
    if (dashPosition < 0.0) dashPosition += period;
    const auto dashDistance = dashPosition <= dashLength
        ? -std::min(dashPosition, dashLength - dashPosition)
        : std::min(dashPosition - dashLength, period - dashPosition);
    const auto dashCoverage = std::clamp(0.5 - dashDistance, 0.0, 1.0);
    const auto strokeCoverage = std::clamp(
        spec.strokeWidth / 2.0 + 0.5 - nearest.distance, 0.0, 1.0);
    return dashCoverage * strokeCoverage;
}

} // namespace desto::presentation
