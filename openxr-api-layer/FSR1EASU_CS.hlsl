// FidelityFX Super Resolution v1.0.2 — EASU pass for the Quad-Views-Foveated layer.
//
// Upscales the peripheral (low-res) swapchain texture to the full-FOV composite
// resolution using AMD's FSR1 EASU (Edge-Adaptive Spatial Upsampling) filter.
// The output is then sampled by ProjectionPS.hlsl in place of the raw swapchain.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Copyright (c) 2023 Matthieu Bucchianeri (OpenXR layer integration)
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

#define A_GPU 1
#define A_HLSL 1
#define FSR_EASU_F 1

// Use CAS's ffx_a.h for base portability types (shared with FSR1).
// The EASU functions are inlined from fsr1_easu_inlined.h to avoid pulling in
// FSR1's ffx_fsr1.h, which references DXC-incompatible helpers in FSR1's ffx_a.h.
#include "ffx_a.h"

SamplerState      srcSampler : register(s0);
Texture2D         SrcTexture : register(t0);
RWTexture2D<float4> DstTexture : register(u0);

cbuffer FSR1Constants : register(b0) {
    uint4 con0;
    uint4 con1;
    uint4 con2;
    uint4 con3;
};

// Per-channel gather callbacks required by FsrEasuF. Using GatherRed/Green/Blue
// (the FP32 "slow fallback" path from AMD's sample) — 3 gathers of 4 taps each.
AF4 FsrEasuRF(AF2 p) { return SrcTexture.GatherRed(srcSampler, p, int2(0, 0)); }
AF4 FsrEasuGF(AF2 p) { return SrcTexture.GatherGreen(srcSampler, p, int2(0, 0)); }
AF4 FsrEasuBF(AF2 p) { return SrcTexture.GatherBlue(srcSampler, p, int2(0, 0)); }

#include "fsr1_easu_inlined.h"

[numthreads(64, 1, 1)]
void main(uint3 LocalThreadId : SV_GroupThreadID, uint3 WorkGroupId : SV_GroupID) {
    // Remap local xy into a PS-like swizzle pattern (matches AMD's sample).
    AU2 gxy = ARmp8x8(LocalThreadId.x) + AU2(WorkGroupId.x << 4u, WorkGroupId.y << 4u);

    // Process a 16x16 tile via 4 8x8 quadrants (the standard EASU dispatch pattern).
    // The output UAV is R16G16B16A16_FLOAT (kEasuFormat), so clamp to the 16-bit
    // float range (+-65504) to prevent HDR overflow device-removed crashes.
    AF3 c;
    FsrEasuF(c, gxy, con0, con1, con2, con3);
    DstTexture[gxy] = float4(clamp(c, -65504.0, 65504.0), 1);

    gxy.y += 8u;
    FsrEasuF(c, gxy, con0, con1, con2, con3);
    DstTexture[gxy] = float4(clamp(c, -65504.0, 65504.0), 1);

    gxy.x += 8u;
    FsrEasuF(c, gxy, con0, con1, con2, con3);
    DstTexture[gxy] = float4(clamp(c, -65504.0, 65504.0), 1);

    gxy.y -= 8u;
    FsrEasuF(c, gxy, con0, con1, con2, con3);
    DstTexture[gxy] = float4(clamp(c, -65504.0, 65504.0), 1);
}
