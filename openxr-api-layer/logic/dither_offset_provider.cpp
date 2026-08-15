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

#include "pch.h"
#include "logic/dither_offset_provider.h"

namespace openxr_api_layer {

    namespace {
        // Golden ratio conjugate: (sqrt(5) - 1) / 2.
        // Advancing by this value modulo 1 produces a low-discrepancy sequence
        // that uniformly covers [0, 1) without repeating.
        constexpr float kGoldenRatio = 0.61803398875f;
        // Complementary step (1 - phi) used for the second axis so the 2D
        // trajectory does not collapse onto a single diagonal.
        constexpr float kGoldenRatioComplement = 1.0f - kGoldenRatio; // 0.38196601125f
    } // namespace

    DirectX::XMFLOAT2 DitherOffsetProvider::offset(uint32_t frameCount) {
        // fmodf keeps the result in [0, 1). Using float arithmetic (rather than
        // integer bit-tricks) gives sub-texel precision that the earlier `& 7`
        // scheme could not provide.
        const float x = std::fmodf(static_cast<float>(frameCount) * kGoldenRatio, 1.0f);
        const float y = std::fmodf(static_cast<float>(frameCount) * kGoldenRatioComplement, 1.0f);
        return DirectX::XMFLOAT2(x, y);
    }

} // namespace openxr_api_layer
