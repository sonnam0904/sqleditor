#pragma once

#if defined(__linux__)

// Configure GLVND/EGL to use the NVIDIA driver before GTK creates a GdkGLContext.
// Safe to call multiple times; does not override variables already set by the user.
void configureLinuxGraphicsEnvironment();

bool isLinuxNvidiaDriverLoaded();

#endif
