// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
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
#include <filesystem>
#include <string>

namespace openxr_api_layer {

    struct FoveationConfig {
        float m_peripheralPixelDensity{0.5f};
        float m_focusPixelDensity{1.f};
        // [0] = non-foveated, [1] = foveated
        float m_horizontalFovSection[2]{0.5f, 0.35f};
        float m_verticalFovSection[2]{0.45f, 0.35f};
        float m_horizontalFocusOffset{0.f};
        float m_verticalFocusOffset{0.f};
        float m_horizontalFixedOffset{0.f};
        float m_verticalFixedOffset{0.f};
        float m_horizontalFocusWideningMultiplier{0.5f};
        float m_verticalFocusWideningMultiplier{0.2f};
        float m_focusWideningDeadzone{0.15f};

        // Focus-view FOV stabilization (boundary shimmer reduction).
        bool  m_stabilizeFocusFov{true};
        float m_focusFovHysteresis{0.05f};   // extra margin (radians) before contracting
        float m_focusFovSmoothing{0.15f};    // lerp factor toward target FOV each frame

        // Blink freeze: when eye tracking is lost (e.g. a blink), hold the focus
        // FOV at its last stable position instead of following the cached gaze.
        // When tracking recovers, if the new gaze is far from the frozen region,
        // keep the frozen FOV for a short grace period so the eye settles and no
        // blurry transition is visible. Sharp-but-off-center briefly beats blurry.
        bool     m_blinkFreezeFocusRegion{true};
        uint32_t m_blinkFreezeGraceMs{150}; // hold frozen FOV this long after recovery

        // FSR1 EASU peripheral upscaling (static aliasing reduction).
        bool  m_useFSR1EASU{true};
        float m_fsr1Sharpness{0.2f};        // reserved for optional RCAS pass (deferred)

        // Generate the EASU output mip chain for proper minification
        // filtering. Set to false to skip the mip-gen loop for max performance
        // at the cost of some peripheral aliasing.
        bool  m_useEasuMipGen{true};

        // Spatial anti-aliasing controls for the peripheral (EASU) view.
        // Default 0.0: a positive global LOD bias darkens the periphery (lower
        // mips are darker on average), so it is opt-in.
        float    m_peripheralLodBias{0.25f};  // Blends toward lower mips to kill EASU shimmer
        uint32_t m_peripheralAnisotropy{8};   // Higher-quality oblique sampling
        // Localized blur applied to the peripheral texture only in the
        // focus/peripheral transition zone. Kills boundary shimmer without the
        // uniform darkening a global LOD bias causes. 0.0 = off, 1.0 = full.
        float    m_peripheralEdgeBlur{0.5f};
        // Boundary desaturation strength [0..1]. Drains color from the
        // transition zone to exploit the eye's low peripheral color acuity,
        // hiding the resolution seam. 0.0 = off, 1.0 = fully grayscale at seam.
        float    m_boundaryDesaturation{0.5f};

        // Radial peripheral LOD bias: sharpens the focus center by reducing
        // the mip bias near the gaze point while keeping the periphery at the
        // configured bias. 0.0 = disabled (uniform bias only).
        // Defaults tuned for subtle effect: 0.3 max boost with a wide ramp
        // (0.2-0.6) gives slight center sharpening without noticeable edge darkening.
        float    m_radialLodStart{0.2f};
        float    m_radialLodEnd{0.6f};
        float    m_radialLodMaxBoost{0.3f};
        float    m_focusAspect{1.0f};

        // Blue-noise (golden-ratio) temporal rotation of the IGN dither
        // pattern. When true, the dither offset advances by the golden ratio
        // each frame instead of the coarse integer `frameCount & 7` scheme,
        // producing a low-discrepancy sequence that covers the unit square
        // uniformly and reduces visible shimmer on static images.
        bool  m_useBlueNoiseDither{true};

        bool m_preferFoveatedRendering{true};
        bool m_forceNoEyeTracking{false};
        float m_smoothenFocusViewEdges{0.2f};
        float m_sharpenFocusView{0.0f};
        // Independent edge blur for the focus-view transition zone. When > 0, a
        // 4-tap box blur is applied to the focus view in the transition zone to
        // feather the resolution boundary. Previously coupled to
        // sharpen_focus_view; now independently controllable. Default 0.3
        // softens the boundary without visibly affecting the focus interior.
        float m_featherFocusEdges{0.3f};
        float m_fovTangentX{1.f};
        float m_fovTangentY{1.f};
        bool m_useTurboMode{true};
        bool m_unadvertiseQuadViews{false};

        bool m_debugSimulateTracking{false};
        bool m_debugFocusView{false};
        bool m_debugEyeGaze{false};
        bool m_debugKeys{false};

        float m_eyeTrackingConfidenceThreshold{0.5f};
        uint32_t m_eyeGazeCacheTimeoutMs{600};

        // 1-Euro Filter parameters
        float m_eyeTrackingMinCutoff{1.0f}; // Minimum cutoff frequency
        float m_eyeTrackingBeta{0.1f};      // Speed coefficient (cutoff slope)

        // Context needed for parsing sections
        std::string m_runtimeName;
        std::string m_systemName;
        std::string m_applicationName;
        std::string m_applicationExecutableName;

        // Quirk flags parsed from config
        bool m_needFocusFovCorrectionQuirk{false};

        // Dithering amount for blend alpha in transition zone
        float m_transitionDitherAmount{0.04f};

        void LoadConfiguration(const std::filesystem::path& configPath);
        bool ParseConfigurationStatement(const std::string& line, unsigned int lineNumber, bool active,
                                         const std::string& applicationName, const std::string& applicationExecutableName);
    };

} // namespace openxr_api_layer
