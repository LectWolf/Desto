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

[[nodiscard]] double SampleRoundedDashCoverage(
    const RoundedDashSpec& spec,
    double x,
    double y) noexcept;

} // namespace desto::presentation
