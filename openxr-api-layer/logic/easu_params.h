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

#pragma once
#include "pch.h"
#include "fsr1_constants.h"

namespace openxr_api_layer {

    // Computes the FSR1 EASU cbuffer (con0..con3) from viewport sizes.
    // Wraps AMD's FsrEasuCon helper. Pure logic — unit-testable.
    //
    // The EASU constants pack the input/output viewport and resource sizes into
    // four uint4 values that the GPU-side FsrEasuF() uses to compute sample
    // positions and the edge-adaptive kernel.
    class EASUParamCalculator {
      public:
        struct Input {
            // The rendered image resolution being upscaled (the peripheral
            // swapchain's actual content size).
            uint32_t inputViewportW{0};
            uint32_t inputViewportH{0};
            // The resolution of the resource containing the input image
            // (the peripheral swapchain texture size — usually == inputViewport
            // unless dynamic resolution is in use).
            uint32_t inputResourceW{0};
            uint32_t inputResourceH{0};
            // The display resolution which the input image gets upscaled to
            // (the full-FOV composite target size).
            uint32_t outputW{0};
            uint32_t outputH{0};
        };

        // Fills `out` with the 4 uint4 constants EASU expects.
        // Returns false if inputs are invalid (zero dimensions), leaving `out`
        // zeroed — callers should skip the EASU pass in that case.
        static bool compute(const Input& in, FSR1Constants& out);
    };

} // namespace openxr_api_layer
