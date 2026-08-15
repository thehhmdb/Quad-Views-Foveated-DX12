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

// Gaussian mip generation compute shader.
// Dispatch one group per 8x8 output tile. Each thread samples a 4x4 block
// from the source mip using a [1, 3, 3, 1] binomial (Gaussian approximation)
// kernel. The wider kernel gives a smoother frequency response than a 2x2 box
// filter when the GPU blends between mip levels, reducing edge "crawling".

cbuffer MipConstants : register(b0) {
    uint srcMipLevel;
    uint dstMipLevel;
    uint srcWidth;
    uint srcHeight;
    float2 invSrcSize;   // 1.0 / srcWidth, 1.0 / srcHeight
    float2 _pad;
};

Texture2D<float4> SrcMip : register(t0);
RWTexture2D<float4> DstMip : register(u0);

SamplerState linearClamp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    // Output dimensions are half of source (2x downsample per mip level).
    uint dstWidth = (srcWidth + 1) >> 1;
    uint dstHeight = (srcHeight + 1) >> 1;
    if (dtid.x >= dstWidth || dtid.y >= dstHeight)
        return;

    // Center the UV on the 4x4 block we are downsampling.
    float2 baseUV = (float2(dtid.xy) * 2.0 - 0.5) * invSrcSize;
    float2 du = float2(invSrcSize.x, 0.0);
    float2 dv = float2(0.0, invSrcSize.y);

    // 1D binomial weights: [1, 3, 3, 1] / 8. Combined 2D weight is / 64.
    static const float weights[4] = {1.0, 3.0, 3.0, 1.0};

    float4 acc = 0.0;
    [unroll]
    for (int j = 0; j < 4; j++) {
        [unroll]
        for (int i = 0; i < 4; i++) {
            float w = weights[i] * weights[j];
            acc += SrcMip.SampleLevel(linearClamp, baseUV + i*du + j*dv, 0.0) * w;
        }
    }

    DstMip[dtid.xy] = acc / 64.0;
}
