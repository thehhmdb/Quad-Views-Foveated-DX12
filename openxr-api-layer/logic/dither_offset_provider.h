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

#pragma once
#include "pch.h"

namespace openxr_api_layer {

    // Provides a per-frame 2D offset for the Interleaved Gradient Noise
    // (IGN) dither used in the projection pixel shader.
    //
    // An earlier approach rotated the IGN pattern with a coarse
    // frameCount-based integer offset (`frameCount & 7`, `(frameCount >> 3) & 7`),
    // which only visits 64 distinct positions before repeating and produces a
    // visible periodic shimmer on static images.
    //
    // This provider advances the offset each frame by the golden ratio
    // (phi = (sqrt(5)-1)/2 ~= 0.61803398875) modulo 1, which yields a low-
    // discrepancy sequence that covers the unit square uniformly over time
    // without ever repeating exactly. The two axes use complementary steps
    // (phi and 1-phi) so the 2D trajectory fills the plane rather than walking
    // along a single diagonal.
    //
    // Pure logic — unit-testable. The returned values are in [0, 1) and are
    // multiplied by the render-target resolution in the shader to obtain a
    // per-pixel sub-texel offset.
    class DitherOffsetProvider {
      public:
        // Returns the 2D blue-noise offset for the given frame index.
        // Both components are in [0, 1).
        static DirectX::XMFLOAT2 offset(uint32_t frameCount);
    };

} // namespace openxr_api_layer
