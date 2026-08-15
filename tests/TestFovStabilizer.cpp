// MIT License
//
// Copyright(c) 2023 Matthieu Bucchianeri
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
#include "logic/fov_stabilizer.h"
#include "logic/config.h"

namespace openxr_api_layer {

    class FovStabilizerTest : public ::testing::Test {
    protected:
        FoveationConfig config;
        FovStabilizer stabilizer{config};

        static XrFovf makeFov(float l, float r, float u, float d) {
            return XrFovf{l, r, u, d};
        }
    };

    TEST_F(FovStabilizerTest, Disabled_ReturnsTargetUnchanged) {
        config.m_stabilizeFocusFov = false;
        stabilizer = FovStabilizer{config};
        auto t = makeFov(-0.3f, 0.3f, 0.3f, -0.3f);
        EXPECT_FLOAT_EQ(stabilizer.stabilize(0, t).angleRight, 0.3f);
        EXPECT_FLOAT_EQ(stabilizer.stabilize(0, t).angleLeft, -0.3f);
    }

    TEST_F(FovStabilizerTest, Hysteresis_BlocksSmallContraction) {
        config.m_stabilizeFocusFov = true;
        config.m_focusFovHysteresis = 0.1f;
        config.m_focusFovSmoothing = 1.0f; // isolate hysteresis (no smoothing)
        stabilizer = FovStabilizer{config};

        stabilizer.stabilize(0, makeFov(-0.4f, 0.4f, 0.4f, -0.4f)); // establish wide
        // Inward shift 0.05 < hysteresis 0.1 → must not contract.
        auto out = stabilizer.stabilize(0, makeFov(-0.35f, 0.35f, 0.35f, -0.35f));
        EXPECT_FLOAT_EQ(out.angleRight, 0.4f);
        EXPECT_FLOAT_EQ(out.angleLeft, -0.4f);
    }

    TEST_F(FovStabilizerTest, Hysteresis_AllowsLargeContraction) {
        config.m_stabilizeFocusFov = true;
        config.m_focusFovHysteresis = 0.1f;
        config.m_focusFovSmoothing = 1.0f;
        stabilizer = FovStabilizer{config};

        stabilizer.stabilize(0, makeFov(-0.4f, 0.4f, 0.4f, -0.4f));
        // Inward shift 0.2 > hysteresis 0.1 → contraction allowed.
        auto out = stabilizer.stabilize(0, makeFov(-0.2f, 0.2f, 0.2f, -0.2f));
        EXPECT_FLOAT_EQ(out.angleRight, 0.2f);
    }

    TEST_F(FovStabilizerTest, Hysteresis_AllowsExpansion) {
        config.m_stabilizeFocusFov = true;
        config.m_focusFovHysteresis = 0.1f;
        config.m_focusFovSmoothing = 1.0f;
        stabilizer = FovStabilizer{config};

        stabilizer.stabilize(0, makeFov(-0.3f, 0.3f, 0.3f, -0.3f));
        auto out = stabilizer.stabilize(0, makeFov(-0.5f, 0.5f, 0.5f, -0.5f));
        EXPECT_FLOAT_EQ(out.angleRight, 0.5f); // expansion always allowed
    }

    TEST_F(FovStabilizerTest, Smoothing_LerpsTowardTarget) {
        config.m_stabilizeFocusFov = true;
        config.m_focusFovHysteresis = 0.0f; // isolate smoothing
        config.m_focusFovSmoothing = 0.5f;
        stabilizer = FovStabilizer{config};

        stabilizer.stabilize(0, makeFov(-0.2f, 0.2f, 0.2f, -0.2f));
        auto out = stabilizer.stabilize(0, makeFov(-0.4f, 0.4f, 0.4f, -0.4f));
        // 0.2 + (0.4 - 0.2) * 0.5 = 0.3
        EXPECT_NEAR(out.angleRight, 0.3f, 1e-5f);
        EXPECT_NEAR(out.angleLeft, -0.3f, 1e-5f);
    }

    TEST_F(FovStabilizerTest, Smoothing_ConvergesOverFrames) {
        config.m_stabilizeFocusFov = true;
        config.m_focusFovHysteresis = 0.0f;
        config.m_focusFovSmoothing = 0.5f;
        stabilizer = FovStabilizer{config};

        stabilizer.stabilize(0, makeFov(-0.2f, 0.2f, 0.2f, -0.2f));
        XrFovf out = makeFov(-0.2f, 0.2f, 0.2f, -0.2f);
        const XrFovf target = makeFov(-0.4f, 0.4f, 0.4f, -0.4f);
        for (int i = 0; i < 10; i++) {
            out = stabilizer.stabilize(0, target);
        }
        // After 10 frames at s=0.5, should be very close to target.
        EXPECT_NEAR(out.angleRight, 0.4f, 1e-3f);
    }

    TEST_F(FovStabilizerTest, Reset_ClearsState) {
        config.m_stabilizeFocusFov = true;
        config.m_focusFovSmoothing = 0.1f;
        stabilizer = FovStabilizer{config};
        stabilizer.stabilize(0, makeFov(-0.4f, 0.4f, 0.4f, -0.4f));
        stabilizer.reset();
        // After reset, first call should return target directly (no smoothing).
        auto out = stabilizer.stabilize(0, makeFov(-0.3f, 0.3f, 0.3f, -0.3f));
        EXPECT_FLOAT_EQ(out.angleRight, 0.3f);
    }

    TEST_F(FovStabilizerTest, PerEyeStateIndependent) {
        config.m_stabilizeFocusFov = true;
        config.m_focusFovSmoothing = 0.1f;
        stabilizer = FovStabilizer{config};
        stabilizer.stabilize(xr::StereoView::Left,  makeFov(-0.4f, 0.4f, 0.4f, -0.4f));
        stabilizer.stabilize(xr::StereoView::Right, makeFov(-0.2f, 0.2f, 0.2f, -0.2f));
        // Different targets per eye → different sent FOVs.
        auto l = stabilizer.stabilize(xr::StereoView::Left,  makeFov(-0.4f, 0.4f, 0.4f, -0.4f));
        auto r = stabilizer.stabilize(xr::StereoView::Right, makeFov(-0.2f, 0.2f, 0.2f, -0.2f));
        EXPECT_NE(l.angleRight, r.angleRight);
    }

    TEST_F(FovStabilizerTest, PerEdgeIndependence_LeftExpandsRightHolds) {
        // The left edge expands while the right edge holds (within hysteresis).
        config.m_stabilizeFocusFov = true;
        config.m_focusFovHysteresis = 0.1f;
        config.m_focusFovSmoothing = 1.0f;
        stabilizer = FovStabilizer{config};

        stabilizer.stabilize(0, makeFov(-0.3f, 0.3f, 0.3f, -0.3f));
        // Left edge expands to -0.5 (allowed); right edge contracts to 0.25 (blocked, < 0.1).
        auto out = stabilizer.stabilize(0, makeFov(-0.5f, 0.25f, 0.3f, -0.3f));
        EXPECT_FLOAT_EQ(out.angleLeft, -0.5f);  // expanded
        EXPECT_FLOAT_EQ(out.angleRight, 0.3f);  // held — did not contract
    }

    TEST_F(FovStabilizerTest, FirstCall_ReturnsTarget) {
        config.m_stabilizeFocusFov = true;
        config.m_focusFovSmoothing = 0.1f;
        stabilizer = FovStabilizer{config};
        auto target = makeFov(-0.3f, 0.3f, 0.3f, -0.3f);
        auto out = stabilizer.stabilize(0, target);
        // No history → first call returns target directly.
        EXPECT_FLOAT_EQ(out.angleRight, 0.3f);
    }

} // namespace openxr_api_layer
