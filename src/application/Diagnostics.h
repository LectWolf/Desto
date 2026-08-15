#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace desto::application {

enum class DiagnosticLevel {
    Debug,
    Info,
    Warning,
    Error,
};

struct DiagnosticEntry {
    std::uint64_t sequence = 0;
    DiagnosticLevel level = DiagnosticLevel::Info;
    std::string code;
};

class DiagnosticRecorder {
public:
    explicit DiagnosticRecorder(
        DiagnosticLevel minimumLevel = DiagnosticLevel::Info,
        std::size_t capacity = 256);

    void record(DiagnosticLevel level, std::string_view code);

    [[nodiscard]] DiagnosticLevel minimumLevel() const noexcept { return minimumLevel_; }
    [[nodiscard]] const std::vector<DiagnosticEntry>& entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] std::uint64_t droppedCount() const noexcept { return droppedCount_; }

private:
    DiagnosticLevel minimumLevel_;
    std::size_t capacity_;
    std::uint64_t nextSequence_ = 1;
    std::uint64_t droppedCount_ = 0;
    std::vector<DiagnosticEntry> entries_;
};

} // namespace desto::application
