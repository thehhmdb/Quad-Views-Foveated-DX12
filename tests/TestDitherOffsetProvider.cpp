// MIT License
//
// Copyright(c) 2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"
#include <gtest/gtest.h>
#include "logic/dither_offset_provider.h"

namespace openxr_api_layer {

    // ---------------------------------------------------------------------------
    // DitherOffsetProvider Tests
    //
    // The provider returns a per-frame 2D offset in [0, 1) used to rotate the
    // Interleaved Gradient Noise (IGN) dither pattern temporally. The sequence
    // must:
    //   1. Stay within [0, 1) on both axes for all frame indices.
    //   2. Produce distinct offsets for consecutive frames (no immediate repeat).
    //   3. Cover the unit square uniformly over time (low-discrepancy).
    //   4. Use complementary steps on the two axes so the 2D trajectory does
    //      not collapse onto a single diagonal.
    // ---------------------------------------------------------------------------

    TEST(DitherOffsetProviderTest, ReturnsValuesInUnitInterval) {
        // Sample a wide range of frame indices and verify both components are
        // in [0, 1). This guards against accidental integer overflow or a
        // missing fmod.
        for (uint32_t frame = 0; frame < 10000; ++frame) {
            const DirectX::XMFLOAT2 off = DitherOffsetProvider::offset(frame);
            EXPECT_GE(off.x, 0.0f) << "frame=" << frame;
            EXPECT_LT(off.x, 1.0f) << "frame=" << frame;
            EXPECT_GE(off.y, 0.0f) << "frame=" << frame;
            EXPECT_LT(off.y, 1.0f) << "frame=" << frame;
        }
    }

    TEST(DitherOffsetProviderTest, ConsecutiveFramesDiffer) {
        // The whole point of the provider is to decorrelate the dither pattern
        // frame-to-frame. If two consecutive frames produced the same offset,
        // the dither would not rotate and shimmer would return.
        const DirectX::XMFLOAT2 a = DitherOffsetProvider::offset(42);
        const DirectX::XMFLOAT2 b = DitherOffsetProvider::offset(43);
        EXPECT_FALSE(a.x == b.x && a.y == b.y)
            << "Consecutive frames must produce distinct offsets";
    }

    TEST(DitherOffsetProviderTest, GoldenRatioStepOnX) {
        // The X component advances by the golden ratio (phi ~= 0.61803398875)
        // modulo 1 each frame. Verify the step is exactly phi (within float
        // precision) for a frame where no wraparound occurs.
        const float phi = 0.61803398875f;
        const DirectX::XMFLOAT2 a = DitherOffsetProvider::offset(10);
        const DirectX::XMFLOAT2 b = DitherOffsetProvider::offset(11);
        const float expectedX = a.x + phi;
        // b.x may have wrapped if a.x + phi >= 1.0; handle both cases.
        float actualStep = b.x - a.x;
        if (actualStep < 0.0f) {
            actualStep += 1.0f;
        }
        EXPECT_NEAR(actualStep, phi, 1e-5f)
            << "X step should be the golden ratio (mod 1)";
    }

    TEST(DitherOffsetProviderTest, AxesUseComplementarySteps) {
        // The two axes must use different steps (phi and 1-phi) so the 2D
        // trajectory fills the plane rather than walking along a single
        // diagonal. Verify the X and Y steps are not equal.
        const DirectX::XMFLOAT2 a = DitherOffsetProvider::offset(100);
        const DirectX::XMFLOAT2 b = DitherOffsetProvider::offset(101);
        float stepX = b.x - a.x;
        if (stepX < 0.0f) stepX += 1.0f;
        float stepY = b.y - a.y;
        if (stepY < 0.0f) stepY += 1.0f;
        EXPECT_GT(std::abs(stepX - stepY), 1e-4f)
            << "X and Y steps must differ to avoid a degenerate diagonal trajectory";
    }

} // namespace openxr_api_layer
