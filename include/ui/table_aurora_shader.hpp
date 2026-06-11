#pragma once

#include "imgui.h"

// experimental — flip to 1 to render the animated aurora behind the table
#define SQLEDITOR_ENABLE_TABLE_AURORA 0

namespace TableAurora {

    // POD payload, copied into ImGui's per-frame command buffer by
    // ImDrawList::AddCallback(..., &params, sizeof(params)).
    struct Params {
        float x, y, w, h; // rect in ImGui points
        float r1, g1, b1; // primary accent
        float r2, g2, b2; // secondary accent
        float time;       // seconds
        float intensity;  // master gain, 0..1
    };

    // Matches ImDrawCallback signature. No-op on non-Apple builds.
    void callback(const ImDrawList* parentList, const ImDrawCmd* cmd);

#ifdef __APPLE__
    // Called once per frame by the Metal backend before ImGui renders its draw data.
    // Uses void* so this header stays Metal-free for portable callers.
    void setMetalRenderContext(void* device, void* encoder, float fbWidth, float fbHeight);
#endif

} // namespace TableAurora
