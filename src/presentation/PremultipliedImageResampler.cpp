#include "PremultipliedImageResampler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace desto::presentation {

std::uint32_t SamplePremultipliedBilinear(
    std::span<const std::uint32_t> pixels,
    std::size_t sourceWidth,
    std::size_t sourceHeight,
    std::size_t targetWidth,
    std::size_t targetHeight,
    std::size_t targetX,
    std::size_t targetY) {
    if (sourceWidth == 0 || sourceHeight == 0 || targetWidth == 0 || targetHeight == 0
        || pixels.size() != sourceWidth * sourceHeight
        || targetX >= targetWidth || targetY >= targetHeight) {
        throw std::invalid_argument("Premultiplied image sample dimensions must be valid.");
    }

    const auto sourceX = std::clamp(
        (static_cast<double>(targetX) + 0.5) * sourceWidth / targetWidth - 0.5,
        0.0,
        static_cast<double>(sourceWidth - 1));
    const auto sourceY = std::clamp(
        (static_cast<double>(targetY) + 0.5) * sourceHeight / targetHeight - 0.5,
        0.0,
        static_cast<double>(sourceHeight - 1));
    const auto x0 = static_cast<std::size_t>(std::floor(sourceX));
    const auto y0 = static_cast<std::size_t>(std::floor(sourceY));
    const auto x1 = std::min(x0 + 1, sourceWidth - 1);
    const auto y1 = std::min(y0 + 1, sourceHeight - 1);
    const auto xWeight = sourceX - x0;
    const auto yWeight = sourceY - y0;

    // Keep the resampler defensive at the boundary: a few shell providers
    // leave RGB data in transparent pixels. It must not be interpolated into
    // a visible edge as a colored halo.
    const auto sanitize = [](std::uint32_t pixel) {
        const auto alpha = (pixel >> 24) & 0xFFu;
        const auto red = std::min((pixel >> 16) & 0xFFu, alpha);
        const auto green = std::min((pixel >> 8) & 0xFFu, alpha);
        const auto blue = std::min(pixel & 0xFFu, alpha);
        return (alpha << 24) | (red << 16) | (green << 8) | blue;
    };
    const auto topLeft = sanitize(pixels[y0 * sourceWidth + x0]);
    const auto topRight = sanitize(pixels[y0 * sourceWidth + x1]);
    const auto bottomLeft = sanitize(pixels[y1 * sourceWidth + x0]);
    const auto bottomRight = sanitize(pixels[y1 * sourceWidth + x1]);
    const auto channel = [&](int shift) {
        const auto interpolate = [](double left, double right, double weight) {
            return left + (right - left) * weight;
        };
        const auto top = interpolate(
            static_cast<double>((topLeft >> shift) & 0xFFu),
            static_cast<double>((topRight >> shift) & 0xFFu),
            xWeight);
        const auto bottom = interpolate(
            static_cast<double>((bottomLeft >> shift) & 0xFFu),
            static_cast<double>((bottomRight >> shift) & 0xFFu),
            xWeight);
        return static_cast<std::uint32_t>(std::lround(interpolate(top, bottom, yWeight)));
    };
    return (channel(24) << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

} // namespace desto::presentation
