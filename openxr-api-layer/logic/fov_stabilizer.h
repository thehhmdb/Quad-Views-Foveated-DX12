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
#include "logic/config.h"

namespace openxr_api_layer {

    // Stabilizes the focus-view FOV against gaze jitter.
    //
    // The foveation boundary "breathes" with micro-saccades because
    // `computeFoveatedViews` recomputes the FOV every frame with no hysteresis
    // and no output smoothing. This class applies two corrections:
    //   1. Asymmetric hysteresis — expansion is instant, contraction requires
    //      the target to shrink by more than `hysteresisRad` (per edge).
    //   2. Exponential smoothing — the sent FOV lerps toward the post-hysteresis
    //      target by `smoothingFactor` each frame.
    //
    // Pure logic — no graphics, no OpenXR calls. Fully unit-testable.
    class FovStabilizer {
      public:
        struct Params {
            bool  enabled{true};
            float hysteresisRad{0.05f};   // extra margin (radians) before contracting
            float smoothingFactor{0.15f}; // lerp toward target each frame
        };

        explicit FovStabilizer(const FoveationConfig& config);

        // Called per focus-view, per frame. `targetFov` is the raw FOV from
        // ComputeBoundingFov; returns the FOV to actually send to the app.
        // `stereoViewIndex` selects independent state per eye (0=Left, 1=Right).
        XrFovf stabilize(uint32_t stereoViewIndex, const XrFovf& targetFov);

        // Reset state (e.g. on session destroy / config reload).
        void reset();

      private:
        Params m_params{};
        XrFovf m_lastTargetFov[xr::StereoView::Count]{};
        XrFovf m_lastSentFov[xr::StereoView::Count]{};
        bool   m_hasLastTarget[xr::StereoView::Count]{false};
        bool   m_hasLastSent[xr::StereoView::Count]{false};
    };

} // namespace openxr_api_layer
