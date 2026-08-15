// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"
#include <gtest/gtest.h>
#include "logic/compositor_shared.h"

namespace openxr_api_layer {

    // ---------------------------------------------------------------------------
    // Constant Buffer Layout Tests
    // These tests lock the byte layout of constant buffer structs so that any
    // accidental change (field reorder, type change, alignment shift) is caught
    // immediately rather than silently corrupting GPU shader output.
    // ---------------------------------------------------------------------------

    TEST(CompositorLayoutTest, ProjectionVSConstants_Size) {
        // 4x4 matrix (64) + stereoSubRect (16) + focusSubRect (16) +
        // stereoSwapchainSize (8) + focusSwapchainSize (8) = 112 bytes
        EXPECT_EQ(sizeof(ProjectionVSConstants), 112u);
        EXPECT_EQ(offsetof(ProjectionVSConstants, focusProjection), 0u);
        EXPECT_EQ(offsetof(ProjectionVSConstants, stereoSubRect), 64u);
        EXPECT_EQ(offsetof(ProjectionVSConstants, focusSubRect), 80u);
        EXPECT_EQ(offsetof(ProjectionVSConstants, stereoSwapchainSize), 96u);
        EXPECT_EQ(offsetof(ProjectionVSConstants, focusSwapchainSize), 104u);
    }

    TEST(CompositorLayoutTest, ProjectionPSConstants_Size) {
        // smoothingArea(4) + ignoreAlpha(4) + isUnpremultipliedAlpha(4) +
        // debugFocusView(4) + sharpenFocusView(4) + transitionDitherAmount(4) +
        // frameCount(4) = 28 bytes for scalars
        // padding to alignas(8) for blueNoiseOffset = 4 bytes
        // blueNoiseOffset(8, alignas(8) -> offset 32) = 8 bytes
        // padding to alignas(16) for stereoSubRect = 8 bytes
        // stereoSubRect(16) + focusSubRect(16) = 32 bytes for vectors
        // stereoSwapchainSize(8) + focusSwapchainSize(8) = 16 bytes
        // useDirectStereoSampling(1+pad) + useDirectFocusSampling(1+pad) = 8 bytes
        // peripheralEdgeBlur(4) + boundaryDesaturation(4) = 8 bytes
        // radialLodStart(4) + radialLodEnd(4) + radialLodMaxBoost(4) + focusAspect(4) = 16 bytes
        // gazeUV(8, alignas(8) -> offset 128) = 8 bytes
        // Fields total 136 bytes; struct is padded to 144 (multiple of 16, the
        // largest member alignment from the XMFLOAT4 fields).
        EXPECT_EQ(sizeof(ProjectionPSConstants), 144u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, smoothingArea), 0u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, ignoreAlpha), 4u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, isUnpremultipliedAlpha), 8u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, debugFocusView), 12u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, sharpenFocusView), 16u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, transitionDitherAmount), 20u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, frameCount), 24u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, featherFocusEdges), 28u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, blueNoiseOffset), 32u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, stereoSubRect), 48u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, focusSubRect), 64u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, stereoSwapchainSize), 80u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, focusSwapchainSize), 88u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, useDirectStereoSampling), 96u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, useDirectFocusSampling), 100u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, peripheralEdgeBlur), 104u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, boundaryDesaturation), 108u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, radialLodStart), 112u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, radialLodEnd), 116u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, radialLodMaxBoost), 120u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, focusAspect), 124u);
        EXPECT_EQ(offsetof(ProjectionPSConstants, gazeUV), 128u);
    }

    TEST(CompositorLayoutTest, SharpeningCSConstants_Size) {
        // Const0[4] (16) + Const1[4] (16) = 32 bytes
        EXPECT_EQ(sizeof(SharpeningCSConstants), 32u);
        EXPECT_EQ(sizeof(SharpeningCSConstants) % 16u, 0u);
        EXPECT_EQ(offsetof(SharpeningCSConstants, Const0), 0u);
        EXPECT_EQ(offsetof(SharpeningCSConstants, Const1), 16u);
    }

    // ---------------------------------------------------------------------------
    // MipCount Helper Tests
    // ---------------------------------------------------------------------------

    TEST(CompositorLayoutTest, MipCount_Basic) {
        // 5428x2716 -> log2(5428) ≈ 12.4 -> 13 mip levels
        EXPECT_EQ(MipCount(5428, 2716), 13u);
        // 256x256 -> log2(256) = 8 -> 9 mip levels
        EXPECT_EQ(MipCount(256, 256), 9u);
        // 1x1 -> 1 mip level
        EXPECT_EQ(MipCount(1, 1), 1u);
        // 2x1 -> log2(2) = 1 -> 2 mip levels
        EXPECT_EQ(MipCount(2, 1), 2u);
        // 3x3 -> log2(3) ≈ 1.58 -> 2 mip levels
        EXPECT_EQ(MipCount(3, 3), 2u);
        // 1024x768 -> log2(1024) = 10 -> 11 mip levels
        EXPECT_EQ(MipCount(1024, 768), 11u);
        // Zero dimensions -> 1 mip level (degenerate)
        EXPECT_EQ(MipCount(0, 100), 1u);
        EXPECT_EQ(MipCount(100, 0), 1u);
    }

} // namespace openxr_api_layer
