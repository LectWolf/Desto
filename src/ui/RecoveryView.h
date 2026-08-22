#pragma once

#include "Dialog.h"

#include <string_view>

namespace desto::ui {

class RecoveryView final {
public:
    [[nodiscard]] static DialogSpec backupAvailable(std::wstring_view backupName);
    [[nodiscard]] static DialogSpec noUsableConfiguration();
};

} // namespace desto::ui
