#import <Metal/Metal.h>
#import <simd/simd.h>

#include "ui/table_aurora_shader.hpp"

namespace {
    id<MTLDevice> gDevice = nil;
    id<MTLRenderCommandEncoder> gEncoder = nil;
    id<MTLRenderPipelineState> gPipeline = nil;
    float gFbWidth = 0.0f;
    float gFbHeight = 0.0f;
    bool gInitFailed = false;

    struct GpuUniforms {
        simd_float4 rect;     // x, y, w, h (ImGui points)
        simd_float4 viewport; // w, h, time, intensity
        simd_float4 color1;
        simd_float4 color2;
    };

    NSString* const kShaderSource = @R"msl(
        #include <metal_stdlib>
        using namespace metal;

        struct Uniforms {
            float4 rect;
            float4 viewport;
            float4 color1;
            float4 color2;
        };

        struct VOut {
            float4 pos [[position]];
            float2 uv;
        };

        vertex VOut aurora_vs(uint vid [[vertex_id]],
                              constant Uniforms& u [[buffer(0)]]) {
            float2 corners[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
            float2 c = corners[vid];
            float2 px = float2(u.rect.x + c.x * u.rect.z,
                               u.rect.y + c.y * u.rect.w);
            float2 ndc = px / u.viewport.xy * 2.0 - 1.0;
            ndc.y = -ndc.y;
            VOut o;
            o.pos = float4(ndc, 0.0, 1.0);
            o.uv = c;
            return o;
        }

        fragment float4 aurora_fs(VOut in [[stage_in]],
                                  constant Uniforms& u [[buffer(0)]]) {
            float2 uv = in.uv;
            float t = u.viewport.z;

            // radial falloff from the top-right corner (1, 0)
            float2 toLight = uv - float2(1.0, 0.0);
            float dist = length(toLight);
            float light = pow(saturate(1.0 - dist / 1.25), 1.8);

            // diagonal coord — 0 at top-right, 1 at bottom-left
            float diag = saturate((1.0 - uv.x + uv.y) * 0.5);

            // flowing bands sweeping along the light axis
            float b1 = sin(diag * 6.0  + t * 0.9);
            float b2 = sin(diag * 12.0 - t * 1.6 + uv.y * 2.0);
            float b3 = sin(diag * 3.0  + t * 0.5 + uv.x * 1.5);
            float aurora = (b1 * 0.45 + b2 * 0.30 + b3 * 0.35) * 0.5 + 0.5;
            aurora = pow(aurora, 1.4);

            float alpha = aurora * light * u.viewport.w;
            alpha = clamp(alpha, 0.0, 0.35);

            // time-driven mix between accent colors, biased by distance from light
            float mixT = sin(diag * 3.0 + t * 0.7) * 0.5 + 0.5;
            float3 col = mix(u.color1.rgb, u.color2.rgb, mixT);

            // premultiplied alpha
            return float4(col * alpha, alpha);
        }
    )msl";

    bool ensurePipeline() {
        if (gPipeline)
            return true;
        if (gInitFailed)
            return false;
        if (!gDevice)
            return false;

        NSError* err = nil;
        id<MTLLibrary> lib = [gDevice newLibraryWithSource:kShaderSource options:nil error:&err];
        if (!lib) {
            NSLog(@"[TableAurora] shader compile failed: %@", err);
            gInitFailed = true;
            return false;
        }

        id<MTLFunction> vs = [lib newFunctionWithName:@"aurora_vs"];
        id<MTLFunction> fs = [lib newFunctionWithName:@"aurora_fs"];
        if (!vs || !fs) {
            NSLog(@"[TableAurora] missing shader functions");
            gInitFailed = true;
            return false;
        }

        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vs;
        desc.fragmentFunction = fs;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].blendingEnabled = YES;
        desc.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        desc.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        gPipeline = [gDevice newRenderPipelineStateWithDescriptor:desc error:&err];
        if (!gPipeline) {
            NSLog(@"[TableAurora] pipeline creation failed: %@", err);
            gInitFailed = true;
            return false;
        }
        return true;
    }

} // namespace

namespace TableAurora {

    void setMetalRenderContext(void* device, void* encoder, float fbWidth, float fbHeight) {
        gDevice = (__bridge id<MTLDevice>)device;
        gEncoder = (__bridge id<MTLRenderCommandEncoder>)encoder;
        gFbWidth = fbWidth;
        gFbHeight = fbHeight;
    }

    void callback(const ImDrawList* /*parentList*/, const ImDrawCmd* cmd) {
        if (!ensurePipeline() || !gEncoder || !cmd->UserCallbackData)
            return;

        const Params* p = static_cast<const Params*>(cmd->UserCallbackData);
        ImVec2 disp = ImGui::GetIO().DisplaySize;
        if (disp.x <= 0 || disp.y <= 0)
            return;

        GpuUniforms u{};
        u.rect = simd_make_float4(p->x, p->y, p->w, p->h);
        u.viewport = simd_make_float4(disp.x, disp.y, p->time, p->intensity);
        u.color1 = simd_make_float4(p->r1, p->g1, p->b1, 1.0f);
        u.color2 = simd_make_float4(p->r2, p->g2, p->b2, 1.0f);

        // open scissor to full framebuffer so the quad isn't clipped
        // by whatever rect ImGui had set for the previous draw cmd
        MTLScissorRect full = {
            0,
            0,
            static_cast<NSUInteger>(gFbWidth),
            static_cast<NSUInteger>(gFbHeight),
        };
        [gEncoder setScissorRect:full];

        [gEncoder setRenderPipelineState:gPipeline];
        [gEncoder setVertexBytes:&u length:sizeof(u) atIndex:0];
        [gEncoder setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [gEncoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
} // namespace TableAurora
