// MIT License
//
// Copyright(c) 2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
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

// A Pixel Shader that paints the content of a layer given the specified coordinates.

#include "ProjectionRootSig.hlsli"

cbuffer ConstantBuffer : register(b0) {
    float smoothingArea;
    uint ignoreAlpha;
    uint isUnpremultipliedAlpha;
    uint debugFocusView;
    float sharpenFocusView;
    float transitionDitherAmount;
    uint frameCount;
    // Edge blur amount for the focus-view transition zone. Fills the 4-byte
    // padding slot that kept blueNoiseOffset 8-byte aligned, so the byte
    // layout stays in sync with ProjectionPSConstants.
    float featherFocusEdges;
    // Per-frame blue-noise offset for the IGN dither pattern.
    float2 blueNoiseOffset;
    float2 _blueNoisePad;
    // Direct sampling fields
    float4 stereoSubRect;
    float4 focusSubRect;
    float2 stereoSwapchainSize;
    float2 focusSwapchainSize;
    bool useDirectStereoSampling;
    bool useDirectFocusSampling;
    // Localized peripheral transition-zone blur amount (mirrors C++ offset 104).
    float peripheralEdgeBlur;
    // Transition-zone desaturation strength (mirrors C++ offset 108, was _structPad).
    float boundaryDesaturation;
    // Radial peripheral LOD bias: distance from focus center at which the
    // radial bias begins (in layer1TexCoord space, [0, ~0.7]).
    float radialLodStart;
    // Distance at which the radial bias reaches maximum.
    float radialLodEnd;
    // Maximum additional LOD bias applied at the periphery edge.
    float radialLodMaxBoost;
    // Aspect ratio correction for non-square focus regions.
    float focusAspect;
    // Projected gaze point in peripheral UV space [0, 1]. Center of the radial
    // LOD bias so the foveal center stays sharp where the user is looking.
    float2 gazeUV;
};

// s0: peripheral (stereo) sampler — anisotropic + LOD bias for spatial AA.
// s1: focus sampler — cheap linear clamp (the focus view is sampled ~1:1).
SamplerState peripheralSampler : register(s0);
SamplerState focusSampler : register(s1);
Texture2D sourceStereoTexture : register(t0);
Texture2D sourceFocusTexture : register(t1);

float4 premultiplyAlpha(float4 color) {
    return float4(color.rgb * color.a, color.a);
}

float4 unpremultiplyAlpha(float4 color) {
    if (color.a != 0) {
        return float4(color.rgb / color.a, color.a);
    } else {
        // Preserve RGB when alpha is 0. For truly premultiplied textures RGB
        // would already be 0 here (so this is a no-op for them), but VR render
        // targets commonly have alpha=0 with valid RGB. Zeroing RGB would turn
        // the focus region opaque black once color1.a is later set to edgeFade.
        return color;
    }
}

[RootSignature(PROJECTION_ROOT_SIG)]
float4 main(in float4 position : SV_POSITION, in float2 texcoord : PROJ_COORD0, in float3 projectedFocusCoord : PROJ_COORD1) : SV_TARGET {
    // Guard against division by zero when the projected focus Z is 0 (degenerate projection),
    // which would produce NaN/Inf and corrupt the composited output.
    float projectedZ = abs(projectedFocusCoord.z) > 1e-6f ? projectedFocusCoord.z : 1e-6f;
    float2 layer1ProjectedCoordNdc = projectedFocusCoord.xy / projectedZ;
    float2 layer1TexCoord = layer1ProjectedCoordNdc * float2(0.5f, -0.5f) + 0.5f;

    // Radial peripheral LOD bias: sharpen the focus center by reducing the
    // mip bias near the gaze point while keeping the periphery at the
    // configured bias. When radialLodMaxBoost is 0, this is a no-op.
    // Clamp the total bias to [0, 2] to stay within the capped mip chain.
    // Use texcoord (peripheral view UV) not layer1TexCoord (focus view UV) —
    // the peripheral texture covers the full FOV. Center on the actual gaze
    // point (gazeUV) so the sharp region tracks where the user is looking.
    float2 centerOffset = texcoord - gazeUV;
    float radialDist = length(centerOffset * focusAspect);
    float radialBias = saturate((radialDist - radialLodStart) / max(radialLodEnd - radialLodStart, 1e-6f));
    float lodBias = clamp(radialLodMaxBoost * radialBias, 0.0f, 2.0f);
    float4 color0 = sourceStereoTexture.SampleBias(peripheralSampler, texcoord, lodBias);

    // DEBUG: visualize the radial bias as a red overlay (uncomment to debug)
    // color0.rgb = lerp(color0.rgb, float3(1, 0, 0), radialBias * 0.5);

    // For pixels outside of the focus view, the sampler will give us a fully transparent pixel.
    float4 color1 = sourceFocusTexture.Sample(focusSampler, layer1TexCoord);

    if (ignoreAlpha != 0) {
        color0.a = color1.a = 1;
    }

    if (isUnpremultipliedAlpha == 0) {
        color0 = unpremultiplyAlpha(color0);
        color1 = unpremultiplyAlpha(color1);
    }

    // Do a smooth transition with alpha-blending around the edges.
    float isInside = all(abs(layer1ProjectedCoordNdc) < 1);
    float edgeFade = 1.0; // 1.0 in center, 0.0 at edges
    // Raw transition-zone mask, computed from the clean (pre-dither) edgeFade so
    // the blur radius is temporally stable. Peaks at edgeFade=0.5 (the exact
    // seam), zero in the focus interior and deep periphery. Shared by BOTH the
    // peripheral and focus blurs so the two sides of the seam are blurred over
    // an identical spatial footprint (prevents differential banding).
    float seamMask = 0.0;
    // Peripheral-side blur strength (seamMask * peripheralEdgeBlur).
    float periphBlurMask = 0.0;
    // Focus-side feather strength (seamMask * featherFocusEdges).
    float focusBlurMask = 0.0;

    if (smoothingArea) {
        float2 s = smoothstep(float2(0, 0), float2(smoothingArea, smoothingArea), layer1TexCoord) -
                   smoothstep(float2(1, 1) - float2(smoothingArea, smoothingArea), float2(1, 1), layer1TexCoord);
        edgeFade = s.x * s.y;

        // Remove the max(0.5, ...) floor that caused a hard visibility edge.
        color1.a = isInside * edgeFade;

        // Capture the shared seam mask from the clean edgeFade BEFORE dithering
        // perturbs color1.a. Derive both blur strengths from it so they share
        // the same spatial extent.
        seamMask = saturate(edgeFade * (1.0 - edgeFade) * 4.0);
        periphBlurMask = seamMask * peripheralEdgeBlur;
        focusBlurMask = seamMask * featherFocusEdges;

        // Dither the blend alpha in the transition zone to mask the resolution boundary.
        // R2 low-discrepancy (Roberts) spatial hash — no texture lookup, ~4 ALU ops.
        // The plastic-constant roots give a 2D sequence that avoids the diagonal
        // periodicity (cross-hatch) of the old IGN constants. The
        // golden-ratio blueNoiseOffset still decorrelates the pattern temporally.
        float2 r2 = frac(position.xy * float2(0.7548776662, 0.5698402909) + blueNoiseOffset);
        float ign = frac(r2.x + r2.y);
        // transitionMask is 0 when alpha is 0 or 1, peaks at alpha=0.5 — limits dithering to the boundary.
        float transitionMask = saturate(color1.a * (1.0 - color1.a) * 4.0);
        color1.a = saturate(color1.a + (ign - 0.5) * transitionDitherAmount * transitionMask);
    } else {
        color1.a = isInside;
    }

    // Screen-space peripheral transition blur. A 4-tap cross blur on the
    // peripheral (stereo) texture, applied only in the transition zone
    // (periphBlurMask > 0). This softens the resolution boundary from the
    // peripheral side, complementing featherFocusEdges on the focus side,
    // without darkening the whole periphery the way a global LOD bias did.
    // The cross taps are also reused to estimate local contrast, which gates
    // the boundary desaturation below.
    float localContrast = 0.0;
    if (periphBlurMask > 0.01) {
        float stereoWidth, stereoHeight;
        sourceStereoTexture.GetDimensions(stereoWidth, stereoHeight);
        float2 texel = float2(1.0 / stereoWidth, 1.0 / stereoHeight);

        float3 tapR = sourceStereoTexture.Sample(peripheralSampler, texcoord + float2(texel.x, 0)).rgb;
        float3 tapL = sourceStereoTexture.Sample(peripheralSampler, texcoord - float2(texel.x, 0)).rgb;
        float3 tapU = sourceStereoTexture.Sample(peripheralSampler, texcoord + float2(0, texel.y)).rgb;
        float3 tapD = sourceStereoTexture.Sample(peripheralSampler, texcoord - float2(0, texel.y)).rgb;

        // Local contrast: largest brightness deviation of the cross taps from
        // the center texel. Flat regions (fog, sky) have ~0 contrast; textured
        // or edged content has high contrast. Computed BEFORE the blur lerp so
        // the center reference is the unblurred pixel.
        float centerLum = dot(color0.rgb, float3(0.25, 0.5, 0.25));
        localContrast = abs(dot(tapR, float3(0.25, 0.5, 0.25)) - centerLum);
        localContrast = max(localContrast, abs(dot(tapL, float3(0.25, 0.5, 0.25)) - centerLum));
        localContrast = max(localContrast, abs(dot(tapU, float3(0.25, 0.5, 0.25)) - centerLum));
        localContrast = max(localContrast, abs(dot(tapD, float3(0.25, 0.5, 0.25)) - centerLum));

        float3 blurred = color0.rgb * 0.5;
        blurred += tapR * 0.125;
        blurred += tapL * 0.125;
        blurred += tapU * 0.125;
        blurred += tapD * 0.125;

        color0.rgb = lerp(color0.rgb, blurred, periphBlurMask);
    }

    // Boundary desaturation: drain color toward a brightness-preserving gray in
    // the transition zone to hide the chroma detail loss of the lower-res
    // periphery. Desaturating toward gray ALWAYS shifts perceived brightness
    // for saturated content (Helmholtz-Kohlrausch): toward luma it darkens
    // blue-dominant fog (dark ring), toward the max channel it brightens
    // (bright ring). There is no gray target that is brightness-neutral for
    // every hue, so instead GATE the effect by local contrast: desaturation
    // exists to mask detail loss, and where there is no detail (flat fog) there
    // is nothing to hide — so the effect fades out and no ring appears at all.
    if (boundaryDesaturation > 0 && periphBlurMask > 0.01) {
        // Ramp from 0 at contrast <= 0.02 (flat) to full at >= 0.10 (detailed).
        float contrastGate = saturate((localContrast - 0.02) / 0.08);
        float strength = periphBlurMask * boundaryDesaturation * contrastGate;
        float perceived = dot(color0.rgb, float3(0.25, 0.5, 0.25));
        float gray = dot(color0.rgb, float3(0.299, 0.587, 0.114));
        // Rescale gray up to perceived brightness; guard against div-by-zero.
        float scale = perceived / max(gray, 1e-4);
        float3 desat = gray.xxx * scale;
        color0.rgb = lerp(color0.rgb, desat, strength);
    }

    // Feather the focus view edges in the transition zone. Uses the SAME cross
    // kernel and the SAME spatial footprint (seamMask) as the peripheral blur so
    // both sides of the seam are blurred identically and the premultiplied blend
    // stays flat (no differential banding). Strength is independent via
    // feather_focus_edges. Controlled independently from CAS sharpening.
    if (featherFocusEdges > 0 && smoothingArea > 0) {
        if (focusBlurMask > 0.01) {
            float focusWidth, focusHeight;
            sourceFocusTexture.GetDimensions(focusWidth, focusHeight);
            float2 texel = float2(1.0 / focusWidth, 1.0 / focusHeight);

            // 4-tap cross kernel matching the peripheral blur (center 0.5,
            // neighbors 0.125 each).
            float3 blurred = color1.rgb * 0.5;
            blurred += sourceFocusTexture.Sample(focusSampler, layer1TexCoord + float2(texel.x, 0)).rgb * 0.125;
            blurred += sourceFocusTexture.Sample(focusSampler, layer1TexCoord - float2(texel.x, 0)).rgb * 0.125;
            blurred += sourceFocusTexture.Sample(focusSampler, layer1TexCoord + float2(0, texel.y)).rgb * 0.125;
            blurred += sourceFocusTexture.Sample(focusSampler, layer1TexCoord - float2(0, texel.y)).rgb * 0.125;

            color1.rgb = lerp(color1.rgb, blurred, focusBlurMask);
        }
    }

    color0 = premultiplyAlpha(color0);
    color1 = premultiplyAlpha(color1);

    // Blend the two pixels.
    float4 color;
    if (debugFocusView != 0) {
        color = color1;
    } else {
        color = color1 + color0 * (1 - color1.a);
    }

    return float4(color.rgb, color0.a);
}
