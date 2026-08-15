#include "Diagnostics.h"

#include <stdexcept>

namespace desto::application {

DiagnosticRecorder::DiagnosticRecorder(
    DiagnosticLevel minimumLevel,
    std::size_t capacity)
    : minimumLevel_(minimumLevel),
      capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("Diagnostic capacity must be greater than zero.");
    }
    entries_.reserve(capacity_);
}

void DiagnosticRecorder::record(DiagnosticLevel level, std::string_view code) {
    if (code.empty()) {
        throw std::invalid_argument("Diagnostic code must not be empty.");
    }
    if (static_cast<int>(level) < static_cast<int>(minimumLevel_)) {
        return;
    }
    if (entries_.size() == capacity_) {
        entries_.erase(entries_.begin());
        ++droppedCount_;
    }
    entries_.push_back({
        .sequence = nextSequence_++,
        .level = level,
        .code = std::string(code),
    });
}

} // namespace desto::application
