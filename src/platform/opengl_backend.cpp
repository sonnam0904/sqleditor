#if defined(__linux__)
// Must include epoxy BEFORE any other GL headers
#include <epoxy/gl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "platform/graphics_backend.hpp"
#include "platform/opengl_texture.hpp"
#include <gtk/gtk.h>
#include <iostream>
#include <string>

// ImGui clipboard — keep a stable in-process buffer. GTK4's async clipboard reader has been
// crashing inside libgtk worker threads ("pool-com.sqledi") on some setups when external apps
// update the selection, so system paste is handled separately on X11 (see linux_platform.cpp).
namespace {
    std::string g_ClipboardTextCache;

    const char* ImGui_ImplGtk_GetClipboardText(void*) {
        return g_ClipboardTextCache.c_str();
    }

    void ImGui_ImplGtk_SetClipboardText(void*, const char* text) {
        if (!text) {
            return;
        }
        g_ClipboardTextCache = text;
    }

    GdkClipboard* getDisplayClipboard(GtkWidget* widget) {
        if (!widget) {
            return nullptr;
        }
        return gdk_display_get_clipboard(gtk_widget_get_display(widget));
    }
} // namespace

void setLinuxClipboardCache(std::string text) {
    g_ClipboardTextCache = std::move(text);
}

void cleanupLinuxClipboard() {
    g_ClipboardTextCache.clear();
}

LinuxOpenGLBackend::LinuxOpenGLBackend() {
    glArea_ = gtk_gl_area_new();
    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(glArea_), GDK_GL_API_GL);
    gtk_gl_area_set_required_version(GTK_GL_AREA(glArea_), 3, 3);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(glArea_), FALSE);
    gtk_gl_area_set_has_stencil_buffer(GTK_GL_AREA(glArea_), FALSE);
    // disable auto-render so only explicit gtk_gl_area_queue_render() calls trigger frames
    gtk_gl_area_set_auto_render(GTK_GL_AREA(glArea_), FALSE);
    gtk_widget_set_hexpand(glArea_, TRUE);
    gtk_widget_set_vexpand(glArea_, TRUE);
    gtk_widget_set_focusable(glArea_, TRUE);
    gtk_widget_set_can_focus(glArea_, TRUE);
}

bool LinuxOpenGLBackend::initializeImGui() {
    ImGui_ImplOpenGL3_Init("#version 330");

    ImGuiIO& io = ImGui::GetIO();
    io.GetClipboardTextFn = ImGui_ImplGtk_GetClipboardText;
    io.SetClipboardTextFn = +[](void* userData, const char* text) {
        ImGui_ImplGtk_SetClipboardText(userData, text);
        if (GdkClipboard* clipboard = getDisplayClipboard(static_cast<GtkWidget*>(userData))) {
            gdk_clipboard_set_text(clipboard, text);
        }
    };
    io.ClipboardUserData = glArea_;

    std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << std::endl;
    imguiGlReady_ = true;
    return true;
}

void LinuxOpenGLBackend::beginFrame(ImVec4 clearColor) {
    glClearColor(clearColor.x, clearColor.y, clearColor.z, clearColor.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_NewFrame();
}

void LinuxOpenGLBackend::renderDrawData(ImDrawData* drawData) {
    glViewport(0, 0, fbWidth_, fbHeight_);
    ImGui_ImplOpenGL3_RenderDrawData(drawData);
}

void LinuxOpenGLBackend::onResize(int width, int height) {
    fbWidth_ = width;
    fbHeight_ = height;
}

void LinuxOpenGLBackend::shutdown() {
    if (imguiGlReady_) {
        ImGui_ImplOpenGL3_Shutdown();
        imguiGlReady_ = false;
    }
    cleanupLinuxClipboard();
    std::cout << "ImGui OpenGL backend shutdown" << std::endl;
}

void LinuxOpenGLBackend::getFramebufferSize(int& width, int& height) {
    width = fbWidth_;
    height = fbHeight_;
}

ImTextureID LinuxOpenGLBackend::createTextureFromRGBA(const uint8_t* pixels, int width,
                                                      int height) {
    return createOpenGLTextureFromRGBA(pixels, width, height);
}

#endif // defined(__linux__)
