#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace desto::presentation {

[[nodiscard]] std::uint32_t SamplePremultipliedBilinear(
    std::span<const std::uint32_t> pixels,
    std::size_t sourceWidth,
    std::size_t sourceHeight,
    std::size_t targetWidth,
    std::size_t targetHeight,
    std::size_t targetX,
    std::size_t targetY);

} // namespace desto::presentation
