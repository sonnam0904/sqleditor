#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

struct LicenseInfo {
    bool valid = false;
    bool networkError =
        false; // true when validation failed due to connectivity, not server rejection
    std::string licenseKey;
    std::string instanceId;
    std::string customerEmail;
    std::string productName;
    std::string status; // "active", "inactive", "expired", "disabled"
    int activationLimit = 0;
    int activationsCount = 0;
    std::string activatedAt;
    std::string expiresAt;
    std::string error;
};

class LicenseManager {
public:
    using ActivationCallback = std::function<void(const LicenseInfo&)>;

    static LicenseManager& instance();

    LicenseManager(const LicenseManager&) = delete;
    LicenseManager& operator=(const LicenseManager&) = delete;

    [[nodiscard]] bool hasValidLicense() const;
    [[nodiscard]] LicenseInfo getLicenseInfo() const;
    void loadStoredLicense();
    void validateStoredLicense();

    void activateLicense(const std::string& licenseKey, ActivationCallback callback);
    void deactivateLicense(ActivationCallback callback);
    void validateLicense(ActivationCallback callback);

    [[nodiscard]] bool isActivating() const {
        return activating.load();
    }

    // generate a unique instance ID for this machine
    [[nodiscard]] std::string getInstanceId() const;

private:
    LicenseManager() = default;
    ~LicenseManager() = default;

    mutable std::mutex licenseMutex_;
    LicenseInfo currentLicense;
    std::atomic<bool> activating{false};
};
