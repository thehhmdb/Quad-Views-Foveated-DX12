// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "pch.h"
#include "compositor.h"
#include "fsr1_constants.h"  // FSR1Constants (standalone — no CAS headers)
#include <utils/graphics.h>

#define A_CPU
#include <ffx_a.h>
#include <ffx_cas.h>

namespace openxr_api_layer {

    // ---------------------------------------------------------------------------
    // Shared Constant Buffer Structs
    // ---------------------------------------------------------------------------
    // These structs must byte-match the HLSL cbuffer layouts in:
    //   - ProjectionVS.hlsl / ProjectionVS11.hlsl
    //   - ProjectionPS.hlsl  / ProjectionPS11.hlsl
    //   - SharpeningCS.hlsl  / SharpeningCS11.hlsl
    // ---------------------------------------------------------------------------

    struct ProjectionVSConstants {
        alignas(16) DirectX::XMFLOAT4X4 focusProjection;
        // Direct sampling fields (must match ProjectionVS.hlsl cbuffer)
        alignas(16) DirectX::XMFLOAT4 stereoSubRect;
        alignas(16) DirectX::XMFLOAT4 focusSubRect;
        alignas(8)  DirectX::XMFLOAT2 stereoSwapchainSize;
        alignas(8)  DirectX::XMFLOAT2 focusSwapchainSize;
    };

    struct ProjectionPSConstants {
        alignas(4) float smoothingArea;
        alignas(4) uint32_t ignoreAlpha;
        alignas(4) uint32_t isUnpremultipliedAlpha;
        alignas(4) uint32_t debugFocusView;
        alignas(4) float sharpenFocusView;
        alignas(4) float transitionDitherAmount;
        alignas(4) uint32_t frameCount;
        // Edge blur amount for the focus-view transition zone. Occupies the
        // 4-byte padding slot that was implicit between frameCount and
        // blueNoiseOffset, so the struct size stays 112 bytes.
        alignas(4) float featherFocusEdges;
        // Per-frame blue-noise offset for the IGN dither pattern.
        // Multiplied by the render-target resolution in the shader to obtain a
        // sub-texel offset that rotates the dither pattern temporally.
        alignas(8) DirectX::XMFLOAT2 blueNoiseOffset;
        // Direct sampling fields (must match ProjectionPS.hlsl cbuffer)
        alignas(16) DirectX::XMFLOAT4 stereoSubRect;
        alignas(16) DirectX::XMFLOAT4 focusSubRect;
        alignas(8)  DirectX::XMFLOAT2 stereoSwapchainSize;
        alignas(8)  DirectX::XMFLOAT2 focusSwapchainSize;
        alignas(4) bool useDirectStereoSampling;
        alignas(4) bool useDirectFocusSampling;
        // Localized peripheral transition-zone blur amount. Occupies 4 bytes of
        // the trailing padding, keeping the struct size at 112 bytes.
        alignas(4) float peripheralEdgeBlur;
        // Transition-zone desaturation strength. Fills the final 4-byte trailing
        // pad (was _structPad), keeping the struct size at 112 bytes.
        alignas(4) float boundaryDesaturation;
        // Radial peripheral LOD bias: distance from focus center at which the
        // radial bias begins (in layer1TexCoord space, [0, ~0.7]).
        alignas(4) float radialLodStart;
        // Distance at which the radial bias reaches maximum.
        alignas(4) float radialLodEnd;
        // Maximum additional LOD bias applied at the periphery edge.
        alignas(4) float radialLodMaxBoost;
        // Aspect ratio correction for non-square focus regions.
        alignas(4) float focusAspect;
        // Projected gaze point in peripheral UV space [0, 1]. Used as the
        // center of the radial LOD bias so the foveal center stays sharp where
        // the user is actually looking (instead of the hardcoded screen center).
        alignas(8) DirectX::XMFLOAT2 gazeUV;
    };

    struct SharpeningCSConstants {
        alignas(4) uint32_t Const0[4];
        alignas(4) uint32_t Const1[4];
    };

    // FSR1Constants is defined in fsr1_constants.h (standalone, no CAS headers).
    // The static_assert below guards the byte-match contract at this site too.

    // ---------------------------------------------------------------------------
    // Layout Locks — compile-time guarantees for struct sizes and key offsets
    // ---------------------------------------------------------------------------
    static_assert(sizeof(ProjectionVSConstants) == 112, "ProjectionVSConstants size must be 112 bytes");
    // 136 bytes of fields, padded to 144 (multiple of 16, the largest member
    // alignment from the XMFLOAT4 fields).
    static_assert(sizeof(ProjectionPSConstants) == 144, "ProjectionPSConstants size must be 144 bytes");
    static_assert(sizeof(SharpeningCSConstants) == 32, "SharpeningCSConstants size must be 32 bytes");
    static_assert(sizeof(FSR1Constants) == 64, "FSR1Constants size must be 64 bytes");

    // ---------------------------------------------------------------------------
    // Shared Computation Helpers
    // ---------------------------------------------------------------------------
    // Inline functions guarantee byte-identical constant buffer contents for
    // both D3D11 and D3D12 compositors.
    // ---------------------------------------------------------------------------

    inline void ComputeProjectionConstants(ProjectionVSConstants& out, const XrFovf& cachedEyeFov, const XrFovf& focusViewFov) {
        // The base layer view projection depends only on cachedEyeFov, which is
        // stable per eye (changes only on headset config). Cache its inverse so
        // we avoid a ComposeProjectionMatrix + XMMatrixInverse per frame.
        // thread_local: compositors run on the app's render thread; no locking.
        struct BaseInvCache {
            XrFovf fov{};
            bool valid{false};
            DirectX::XMMATRIX inverse;
        };
        static thread_local BaseInvCache s_cache;

        if (!s_cache.valid ||
            s_cache.fov.angleLeft != cachedEyeFov.angleLeft ||
            s_cache.fov.angleRight != cachedEyeFov.angleRight ||
            s_cache.fov.angleUp != cachedEyeFov.angleUp ||
            s_cache.fov.angleDown != cachedEyeFov.angleDown) {
            const DirectX::XMMATRIX baseLayerViewProjection =
                xr::math::ComposeProjectionMatrix(cachedEyeFov, xr::math::NearFar{0.1f, 20.f});
            s_cache.inverse = DirectX::XMMatrixInverse(nullptr, baseLayerViewProjection);
            s_cache.fov = cachedEyeFov;
            s_cache.valid = true;
        }

        const DirectX::XMMATRIX layerViewProjection =
            xr::math::ComposeProjectionMatrix(focusViewFov, xr::math::NearFar{0.1f, 20.f});

        DirectX::XMStoreFloat4x4(
            &out.focusProjection,
            DirectX::XMMatrixTranspose(s_cache.inverse * layerViewProjection));
    }

    inline void ComputePixelShaderConstants(ProjectionPSConstants& out, const CompositorParams& params) {
        out.smoothingArea = params.useQuadViews ? params.smoothenFocusViewEdges : 0;
        // Logical NOT (!), not bitwise NOT (~): the flag test yields 0 or a
        // single set bit, and ~ of either is always non-zero, which forced
        // ignoreAlpha to 1 unconditionally.
        out.ignoreAlpha = !(params.layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) ? 1 : 0;
        out.isUnpremultipliedAlpha = (params.layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 1 : 0;
        out.debugFocusView = params.debugFocusView ? 1 : 0;
        out.sharpenFocusView = params.sharpenFocusView;
        out.transitionDitherAmount = params.transitionDitherAmount;
        out.frameCount = params.frameCount;
        out.featherFocusEdges = params.featherFocusEdges;
        out.blueNoiseOffset = params.blueNoiseOffset;
        out.peripheralEdgeBlur = params.peripheralEdgeBlur;
        out.boundaryDesaturation = params.boundaryDesaturation;
        out.radialLodStart = params.radialLodStart;
        out.radialLodEnd = params.radialLodEnd;
        out.radialLodMaxBoost = params.radialLodMaxBoost;
        out.focusAspect = params.focusAspect;
        // Convert gaze from NDC [-1, 1] to UV [0, 1]. Y-flip: NDC +Y is up,
        // UV +Y is down. eyeGaze comes from ProjectPoint (NDC) in ViewManager.
        out.gazeUV = DirectX::XMFLOAT2(
            params.eyeGaze.x * 0.5f + 0.5f,
            1.0f - (params.eyeGaze.y * 0.5f + 0.5f));
    }

    inline void ComputeCasConstants(SharpeningCSConstants& out, float sharpness, uint32_t width, uint32_t height) {
        CasSetup(out.Const0,
                 out.Const1,
                 std::clamp(sharpness, 0.f, 1.f),
                 (AF1)width,
                 (AF1)height,
                 (AF1)width,
                 (AF1)height);
    }

    // ---------------------------------------------------------------------------
    // Utility Helpers
    // ---------------------------------------------------------------------------

    /// Computes the number of mip levels for a texture of the given
    /// dimensions. Returns floor(log2(max(w, h))) + 1, with a minimum of 1.
    inline uint32_t MipCount(uint32_t w, uint32_t h) {
        if (w == 0 || h == 0) return 1;
        uint32_t maxDim = std::max(w, h);
        uint32_t levels = 1;
        while (maxDim > 1) {
            ++levels;
            maxDim >>= 1;
        }
        return levels;
    }

    /// Cap the peripheral mip chain at the levels actually reachable by the
    /// peripheral sampler's MipLODBias (clamped [0, 2]) plus a small safety
    /// margin. This avoids generating mip levels that are never sampled.
    inline uint32_t CappedPeripheralMipCount(uint32_t w, uint32_t h, float maxLodBias) {
        uint32_t fullCount = MipCount(w, h);
        // maxLodBias is clamped to [0, 2] in config. Add 2 for safety margin
        // (anisotropic filtering can push slightly beyond the bias).
        uint32_t capped = static_cast<uint32_t>(2 + std::ceil(maxLodBias)) + 2;
        return std::min(fullCount, capped);
    }

    /// Returns true when the sub-image region already covers the full swapchain
    /// at array index 0, so no flatten copy is required.
    ///
    /// Array swapchains always flatten: the direct-bind path uses a plain
    /// TEXTURE2D SRV, which is only valid on an arraySize=1 resource, so
    /// binding an array resource through it is a view/resource dimension
    /// mismatch with undefined results. The staged copy already resolves the
    /// slice index, so the array case costs one small copy.
    inline bool NeedsFlattening(const XrCompositionLayerProjectionView& view,
                                const SwapchainInfo& swapchainInfo) {
        return !(swapchainInfo.createInfo.arraySize <= 1 &&
                 view.subImage.imageRect.offset.x == 0 &&
                 view.subImage.imageRect.offset.y == 0 &&
                 view.subImage.imageRect.extent.width == swapchainInfo.createInfo.width &&
                 view.subImage.imageRect.extent.height == swapchainInfo.createInfo.height &&
                 view.subImage.imageArrayIndex == 0);
    }

    /// Sharpening pass configuration and dispatch helpers.
    struct SharpeningPass {
        static constexpr int ThreadGroupWorkRegionDim = 16;

        uint32_t dispatchX;
        uint32_t dispatchY;

        /// Returns true when sharpening should run for this view.
        bool operator()(const CompositorParams& params,
                        const XrCompositionLayerProjectionView& focusView,
                        const SwapchainInfo& focusSwapchainInfo) {
            // Only sharpen when the compositor flag is set AND the focus view
            // requires a flatten copy (sharpening operates on the flat image).
            if (!params.sharpenFocusView)
                return false;

            // Compute dispatch dimensions
            dispatchX = (focusView.subImage.imageRect.extent.width + (ThreadGroupWorkRegionDim - 1)) / ThreadGroupWorkRegionDim;
            dispatchY = (focusView.subImage.imageRect.extent.height + (ThreadGroupWorkRegionDim - 1)) / ThreadGroupWorkRegionDim;

            return true;
        }

        /// Prepare CAS constants for the sharpening compute shader.
        void PrepareConstants(SharpeningCSConstants& out,
                              float sharpness,
                              uint32_t width,
                              uint32_t height) const {
            ComputeCasConstants(out, sharpness, width, height);
        }
    };

} // namespace openxr_api_layer
