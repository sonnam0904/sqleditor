#if defined(_WIN32)

#include "platform/updater.hpp"

void initializeUpdater() {}

void checkForUpdates() {}

void pollUpdater() {}

void cleanupUpdater() {}

bool isUpdateAvailable() {
    return false;
}

std::string getLatestVersion() {
    return "";
}

#endif
