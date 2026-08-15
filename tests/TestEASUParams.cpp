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
#include "logic/easu_params.h"
#include "fsr1_constants.h"

#include <cmath>

namespace openxr_api_layer {

    class EASUParamsTest : public ::testing::Test {
    protected:
        FSR1Constants out{};

        // Helper: true if all 16 uint32s in the constants are finite when
        // reinterpreted as float. EASU packs floats into uint32s, so NaN/Inf
        // would indicate a bad computation.
        static bool allFinite(const FSR1Constants& c) {
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    float f;
                    std::memcpy(&f, &c.con0[i * 4 + j], sizeof(float)); // index trick below
                    (void)i; (void)j;
                }
            }
            auto check = [](const uint32_t (&arr)[4]) -> bool {
                for (int k = 0; k < 4; k++) {
                    float f;
                    std::memcpy(&f, &arr[k], sizeof(float));
                    if (!std::isfinite(f)) return false;
                }
                return true;
            };
            return check(c.con0) && check(c.con1) && check(c.con2) && check(c.con3);
        }

        static bool allZero(const FSR1Constants& c) {
            for (int i = 0; i < 4; i++) {
                if (c.con0[i] != 0 || c.con1[i] != 0 ||
                    c.con2[i] != 0 || c.con3[i] != 0) return false;
            }
            return true;
        }
    };

    TEST_F(EASUParamsTest, FullViewport_ProducesValidConstants) {
        EASUParamCalculator::Input in{};
        in.inputViewportW = 1280;
        in.inputViewportH = 1440;
        in.inputResourceW = 1280;
        in.inputResourceH = 1440;
        in.outputW = 2560;
        in.outputH = 2880;

        ASSERT_TRUE(EASUParamCalculator::compute(in, out));
        // Constants should be non-zero and finite.
        EXPECT_FALSE(allZero(out));
        EXPECT_TRUE(allFinite(out));
    }

    TEST_F(EASUParamsTest, ZeroDimensions_ReturnsFalseAndZeroes) {
        EASUParamCalculator::Input in{};
        // All zero by default.
        EXPECT_FALSE(EASUParamCalculator::compute(in, out));
        EXPECT_TRUE(allZero(out));
    }

    TEST_F(EASUParamsTest, ZeroOutputDimension_ReturnsFalse) {
        EASUParamCalculator::Input in{};
        in.inputViewportW = 1280;
        in.inputViewportH = 1440;
        in.inputResourceW = 1280;
        in.inputResourceH = 1440;
        in.outputW = 0; // invalid
        in.outputH = 2880;
        EXPECT_FALSE(EASUParamCalculator::compute(in, out));
    }

    TEST_F(EASUParamsTest, DifferentOutputSize_ProducesDifferentConstants) {
        EASUParamCalculator::Input in{};
        in.inputViewportW = 1280;
        in.inputViewportH = 1440;
        in.inputResourceW = 1280;
        in.inputResourceH = 1440;
        in.outputW = 2560;
        in.outputH = 2880;

        FSR1Constants outA{};
        ASSERT_TRUE(EASUParamCalculator::compute(in, outA));

        in.outputW = 3840; // larger upscale
        in.outputH = 4320;
        FSR1Constants outB{};
        ASSERT_TRUE(EASUParamCalculator::compute(in, outB));

        // con0 encodes the input->output scale, so it must differ.
        bool con0Differs = false;
        for (int i = 0; i < 4; i++) {
            if (outA.con0[i] != outB.con0[i]) { con0Differs = true; break; }
        }
        EXPECT_TRUE(con0Differs);
    }

    TEST_F(EASUParamsTest, DifferentInputSize_ProducesDifferentConstants) {
        EASUParamCalculator::Input in{};
        in.inputViewportW = 1280;
        in.inputViewportH = 1440;
        in.inputResourceW = 1280;
        in.inputResourceH = 1440;
        in.outputW = 2560;
        in.outputH = 2880;

        FSR1Constants outA{};
        ASSERT_TRUE(EASUParamCalculator::compute(in, outA));

        in.inputViewportW = 640; // smaller input (more aggressive upscale)
        in.inputViewportH = 720;
        FSR1Constants outB{};
        ASSERT_TRUE(EASUParamCalculator::compute(in, outB));

        // con0 encodes the input->output scale, so it must differ.
        bool con0Differs = false;
        for (int i = 0; i < 4; i++) {
            if (outA.con0[i] != outB.con0[i]) { con0Differs = true; break; }
        }
        EXPECT_TRUE(con0Differs);
    }

    TEST_F(EASUParamsTest, ResourceLargerThanViewport_ProducesValidConstants) {
        // Dynamic resolution case: viewport (rendered) smaller than resource.
        EASUParamCalculator::Input in{};
        in.inputViewportW = 960;
        in.inputViewportH = 1080;
        in.inputResourceW = 1280; // resource is larger
        in.inputResourceH = 1440;
        in.outputW = 2560;
        in.outputH = 2880;

        ASSERT_TRUE(EASUParamCalculator::compute(in, out));
        EXPECT_FALSE(allZero(out));
        EXPECT_TRUE(allFinite(out));
    }

    TEST_F(EASUParamsTest, SquareInputSquareOutput_ProducesValidConstants) {
        EASUParamCalculator::Input in{};
        in.inputViewportW = 1000;
        in.inputViewportH = 1000;
        in.inputResourceW = 1000;
        in.inputResourceH = 1000;
        in.outputW = 2000;
        in.outputH = 2000;

        ASSERT_TRUE(EASUParamCalculator::compute(in, out));
        EXPECT_FALSE(allZero(out));
        EXPECT_TRUE(allFinite(out));
    }

} // namespace openxr_api_layer
