#include "license/license_manager.hpp"
#include <spdlog/spdlog.h>

#include <fstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

LicenseManager& LicenseManager::instance() {
    static LicenseManager inst;
    return inst;
}

bool LicenseManager::hasValidLicense() const {
    return true;
}

LicenseInfo LicenseManager::getLicenseInfo() const {
    std::lock_guard lock(licenseMutex_);
    LicenseInfo info = currentLicense;
    info.valid = true;
    info.status = "active";
    return info;
}

std::string LicenseManager::getInstanceId() const {
    std::string machineId;

#ifdef __linux__
    std::ifstream f("/etc/machine-id");
    if (f.is_open()) {
        std::getline(f, machineId);
        f.close();
    }
#elif __APPLE__
    FILE* pipe = popen(
        "ioreg -rd1 -c IOPlatformExpertDevice | awk '/IOPlatformUUID/ { print $3 }' | tr -d '\"'",
        "r");
    if (pipe) {
        char buffer[128];
        if (fgets(buffer, sizeof(buffer), pipe)) {
            machineId = buffer;
            if (!machineId.empty() && machineId.back() == '\n') {
                machineId.pop_back();
            }
        }
        pclose(pipe);
    }
#elif _WIN32
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ,
                      &hKey) == ERROR_SUCCESS) {
        char value[256];
        DWORD size = sizeof(value);
        if (RegQueryValueExA(hKey, "MachineGuid", nullptr, nullptr, (LPBYTE)value, &size) ==
            ERROR_SUCCESS) {
            machineId = value;
        }
        RegCloseKey(hKey);
    }
#endif

    if (machineId.empty()) {
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            machineId = hostname;
        } else {
            machineId = "unknown-machine";
        }
    }

    return machineId;
}

void LicenseManager::loadStoredLicense() {
    std::lock_guard lock(licenseMutex_);
    currentLicense.valid = true;
    currentLicense.status = "active";
    spdlog::info("Full license enabled (license check bypassed)");
}

void LicenseManager::validateStoredLicense() {}

void LicenseManager::activateLicense(const std::string& licenseKey, ActivationCallback callback) {
    LicenseInfo result;
    result.valid = true;
    result.status = "active";
    result.licenseKey = licenseKey;

    {
        std::lock_guard lock(licenseMutex_);
        currentLicense = result;
    }

    callback(result);
}

void LicenseManager::deactivateLicense(ActivationCallback callback) {
    LicenseInfo result;
    result.valid = true;
    result.status = "active";
    callback(result);
}

void LicenseManager::validateLicense(ActivationCallback callback) {
    callback(getLicenseInfo());
}
