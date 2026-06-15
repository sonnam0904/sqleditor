#if defined(__linux__)

#include "platform/linux_graphics_env.hpp"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace {
    void setEnvIfUnset(const char* name, const char* value) {
        if (std::getenv(name) == nullptr) {
            setenv(name, value, 0);
        }
    }

    bool pathExists(const char* path) {
        std::ifstream file(path);
        return file.good();
    }
} // namespace

bool isLinuxNvidiaDriverLoaded() {
    return pathExists("/proc/driver/nvidia/version");
}

void configureLinuxGraphicsEnvironment() {
    if (!isLinuxNvidiaDriverLoaded()) {
        return;
    }

    spdlog::info("NVIDIA driver detected; preferring NVIDIA GL/EGL stack");

    setEnvIfUnset("__GLX_VENDOR_LIBRARY_NAME", "nvidia");

    constexpr const char* kNvidiaEglVendor = "/usr/share/glvnd/egl_vendor.d/10_nvidia.json";
    if (pathExists(kNvidiaEglVendor)) {
        setEnvIfUnset("__EGL_VENDOR_LIBRARY_FILENAMES", kNvidiaEglVendor);
    }

    setEnvIfUnset("GBM_BACKEND", "nvidia-drm");
}

#endif
