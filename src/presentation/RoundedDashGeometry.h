#pragma once

namespace desto::presentation {

struct RoundedDashSpec {
    double width = 0.0;
    double height = 0.0;
    double radius = 0.0;
    double strokeWidth = 2.0;
    double nominalPeriod = 9.0;
    double dashFraction = 0.58;
};

struct RoundedRectSpec {
    double width = 0.0;
    double height = 0.0;
    double radius = 0.0;
    double strokeWidth = 1.0;
};

[[nodiscard]] double SampleRoundedRectCoverage(
    const RoundedRectSpec& spec,
    double x,
    double y) noexcept;

[[nodiscard]] double SampleInnerRoundedOutlineCoverage(
    const RoundedRectSpec& spec,
    double x,
    double y) noexcept;

[[nodiscard]] double SampleRoundedDashCoverage(
    const RoundedDashSpec& spec,
    double x,
    double y) noexcept;

} // namespace desto::presentation
