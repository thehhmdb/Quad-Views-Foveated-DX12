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
#include <cstdint>

namespace openxr_api_layer {

    // FSR1 EASU constants. Must byte-match FSR1EASU_CS.hlsl cbuffer.
    // con0..con3 are the packed constants produced by AMD's FsrEasuCon().
    // This struct is deliberately isolated in its own header to avoid pulling
    // in CAS headers (which would conflict with FSR1's bundled ffx_a.h).
    struct FSR1Constants {
        alignas(16) uint32_t con0[4];
        alignas(16) uint32_t con1[4];
        alignas(16) uint32_t con2[4];
        alignas(16) uint32_t con3[4];
    };
    static_assert(sizeof(FSR1Constants) == 64, "FSR1Constants size must be 64 bytes");

} // namespace openxr_api_layer