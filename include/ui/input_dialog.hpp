#pragma once

#include <functional>
#include <string>

class InputDialog {
public:
    // returns empty string on success (dialog closes), error message keeps dialog open
    using ConfirmCallback = std::function<std::string(const std::string&)>;
    using ValidatorCallback = std::function<std::string(const std::string&)>;
    using CancelCallback = std::function<void()>;

    static void show(const std::string& title, const std::string& label,
                     const std::string& initialValue, const std::string& confirmButtonText,
                     ConfirmCallback onConfirm, CancelCallback onCancel = nullptr,
                     ValidatorCallback validator = nullptr);
};
