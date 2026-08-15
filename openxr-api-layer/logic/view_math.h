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
#include "framework/dispatch.gen.h"
#include "views.h"
#include "logic/config.h"
#include "logic/fov_stabilizer.h"

namespace openxr_api_layer {

    class ViewManager {
      public:
        ViewManager(OpenXrApi* openXrApi, FoveationConfig& config);

        void populateFovTables(XrSystemId systemId, XrSession session);
        void computeFoveatedViews(XrView* views,
                                  uint32_t viewCount,
                                  XrViewConfigurationType viewConfigType,
                                  bool isGazeValid,
                                  const XrVector3f& gazeUnitVector,
                                  bool wasCacheUsed = false);

        bool m_needComputeBaseFov{true};
        XrFovf m_cachedEyeFov[xr::QuadView::Count]{};
        XrPosef m_cachedEyePoses[xr::StereoView::Count]{};
        XrVector2f m_centerOfFov[xr::StereoView::Count]{};
        XrVector2f m_eyeGaze[xr::StereoView::Count]{};

        XrExtent2Di m_fullFovResolution{};

      private:
        OpenXrApi* m_openXrApi{nullptr};
        FoveationConfig& m_config;
        FovStabilizer m_fovStabilizer;

        // Blink freeze state: when eye tracking is lost (e.g. blink), hold the
        // last valid focus-view FOV instead of falling back to cached FOV.
        // This prevents the post-blink blur caused by the transition from
        // pre-blink to post-blink gaze position.
        XrFovf m_frozenFov[xr::StereoView::Count]{};
        bool m_hasFrozenFov[xr::StereoView::Count]{false};
        bool m_freezeActive[xr::StereoView::Count]{false};
        std::chrono::steady_clock::time_point m_freezeRecoveryTime[xr::StereoView::Count]{};
    };

} // namespace openxr_api_layer
