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
#include "logic/easu_params.h"

// FSR1 portability + header. The FSR1 submodule bundles its own ffx_a.h, so
// we only need the ffx-fsr/ include path on the project.
// FSR1Constants is defined in compositor_shared.h (included via easu_params.h).
#define A_CPU 1
#include <ffx_a.h>
#include <ffx_fsr1.h>

namespace openxr_api_layer {

    bool EASUParamCalculator::compute(const Input& in, FSR1Constants& out) {
        // Zero the output so callers see a clean state on failure.
        out = FSR1Constants{};

        // Validate inputs — any zero dimension makes EASU meaningless.
        if (in.inputViewportW == 0 || in.inputViewportH == 0 ||
            in.inputResourceW == 0 || in.inputResourceH == 0 ||
            in.outputW == 0 || in.outputH == 0) {
            return false;
        }

        // AMD's FsrEasuCon packs viewport + size info into con0..con3.
        // Signature (from ffx_fsr1.h):
        //   FsrEasuCon(out con0..3,
        //              inputViewportInPixelsX, inputViewportInPixelsY,  // rendered image res
        //              inputSizeInPixelsX,    inputSizeInPixelsY,        // resource containing input
        //              outputSizeInPixelsX,   outputSizeInPixelsY);     // display (upscale target)
        FsrEasuCon(
            out.con0, out.con1, out.con2, out.con3,
            static_cast<float>(in.inputViewportW),
            static_cast<float>(in.inputViewportH),
            static_cast<float>(in.inputResourceW),
            static_cast<float>(in.inputResourceH),
            static_cast<float>(in.outputW),
            static_cast<float>(in.outputH));

        return true;
    }

} // namespace openxr_api_layer
