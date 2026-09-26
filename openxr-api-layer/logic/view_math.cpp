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

#include "pch.h"
#include "view_math.h"
#include "framework/log.h"
#include "framework/util.h"
#include "views.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;
    using namespace xr::math;

    // Helper: check if gaze is near the frozen FOV region (within ~15 degrees).
    static bool IsGazeNearFrozenFov(const XrView& viewForGazeProjection,
                                    const XrVector3f& gazeUnitVector,
                                    const XrFovf& frozenFov) {
        XrVector2f projectedGaze;
        if (!ProjectPoint(viewForGazeProjection, gazeUnitVector, projectedGaze)) {
            return false;
        }
        // Convert frozen FOV to normalized coordinates for comparison.
        // The frozen FOV is in the same space as the view's FOV.
        // We check if the projected gaze falls within the frozen FOV expanded by a margin.
        const float margin = 0.26f; // ~15 degrees in normalized space
        return projectedGaze.x >= frozenFov.angleLeft - margin &&
               projectedGaze.x <= frozenFov.angleRight + margin &&
               projectedGaze.y >= frozenFov.angleDown - margin &&
               projectedGaze.y <= frozenFov.angleUp + margin;
    }

    ViewManager::ViewManager(OpenXrApi* openXrApi, FoveationConfig& config)
        : m_openXrApi(openXrApi), m_config(config), m_fovStabilizer(config) {}

    void ViewManager::populateFovTables(XrSystemId systemId, XrSession session) {
        if (!m_needComputeBaseFov) {
            return;
        }

        // cacheStereoView() call removed: FOV and poses are now lazily provided by the caller
        // (xrLocateViews) which already has valid view data from the proper frame loop.

        XrView view[xr::StereoView::Count]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        for (uint32_t eye = 0; eye < xr::StereoView::Count; eye++) {
            view[eye].fov = m_cachedEyeFov[eye];
            // Identity: the resting gaze {0,0,-1} is head-relative (VIEW space), so
            // the projection camera must be too. A cached pose would offset
            // m_centerOfFov by the head rotation latched at first locate.
            view[eye].pose = Pose::Identity();

            // Calculate the "resting" gaze position.
            XrVector2f projectedGaze{};
            ProjectPoint(view[eye], {0.f, 0.f, -1.f}, projectedGaze);
            m_eyeGaze[eye] = m_centerOfFov[eye] = projectedGaze;
            m_eyeGaze[eye] = m_eyeGaze[eye] + XrVector2f{eye == xr::StereoView::Left ? -m_config.m_horizontalFixedOffset
                                                                                     : m_config.m_horizontalFixedOffset,
                                                         m_config.m_verticalFixedOffset};

            // Populate the FOV for the focus view (when no eye tracking is used).
            const XrVector2f min{std::clamp(m_eyeGaze[eye].x - m_config.m_horizontalFovSection[0], -1.f, 1.f),
                                 std::clamp(m_eyeGaze[eye].y - m_config.m_verticalFovSection[0], -1.f, 1.f)};
            const XrVector2f max{std::clamp(m_eyeGaze[eye].x + m_config.m_horizontalFovSection[0], -1.f, 1.f),
                                 std::clamp(m_eyeGaze[eye].y + m_config.m_verticalFovSection[0], -1.f, 1.f)};
            m_cachedEyeFov[eye + xr::StereoView::Count] =
                xr::math::ComputeBoundingFov(m_cachedEyeFov[eye], min, max);
        }

        {
            XrViewConfigurationView stereoViews[xr::StereoView::Count]{{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                                                       {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
            uint32_t count;
            CHECK_XRCMD(m_openXrApi->xrEnumerateViewConfigurationViews(m_openXrApi->GetXrInstance(),
                                                                     systemId,
                                                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                     xr::StereoView::Count,
                                                                     &count,
                                                                     stereoViews));
            const float newWidth =
                m_config.m_focusPixelDensity * stereoViews[xr::StereoView::Left].recommendedImageRectWidth;
            const float ratio = (float)stereoViews[xr::StereoView::Left].recommendedImageRectHeight /
                                stereoViews[xr::StereoView::Left].recommendedImageRectWidth;
            const float newHeight = newWidth * ratio;

            m_fullFovResolution.width =
                std::min((uint32_t)newWidth, stereoViews[xr::StereoView::Left].maxImageRectWidth);
            m_fullFovResolution.height =
                std::min((uint32_t)newHeight, stereoViews[xr::StereoView::Left].maxImageRectHeight);
        }

        m_needComputeBaseFov = false;
    }

    void ViewManager::computeFoveatedViews(XrView* views,
                                           uint32_t viewCount,
                                           XrViewConfigurationType viewConfigType,
                                           bool isGazeValid,
                                           const XrVector3f& gazeUnitVector,
                                           bool wasCacheUsed) {
        // Set up the focus view or FOV tangent.
        for (uint32_t i = viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ? 0 : xr::StereoView::Count;
             i < viewCount;
             i++) {
            const uint32_t stereoViewIndex = viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO ? i : (i - xr::StereoView::Count);

            views[i].pose = views[stereoViewIndex].pose;

            // The gaze vector from the eye tracker is head-relative (VIEW space).
            // Projecting it through an eye pose expressed in the application's
            // reference space would inject that pose's rotation into the projected
            // gaze -- and the pose cached here is latched at the first valid
            // xrLocateViews, which can be arbitrarily rotated. The correct
            // projection camera for a VIEW-space direction is the identity: the
            // eye's frustum axes are the head's axes (per-eye poses differ only by
            // IPD translation, which cannot affect a direction).
            XrView viewForGazeProjection{};
            viewForGazeProjection.pose = Pose::Identity();
            viewForGazeProjection.fov = views[stereoViewIndex].fov;
            XrVector2f projectedGaze;
            // [DEBUG-QVF-POS] Always log the raw gaze and the cached eye pose (kept for
            // comparison with the located one; it is no longer used for projection).
            {
                const XrQuaternionf& q = m_cachedEyePoses[stereoViewIndex].orientation;
                LogDebug("  xrLocateViews[{}]: rawGaze=({:.4f},{:.4f},{:.4f}) isGazeValid={} "
                         "cachedPosePos=({:.4f},{:.4f},{:.4f}) cachedPoseOri=(w={:.4f} x={:.4f} y={:.4f} z={:.4f})\n",
                    stereoViewIndex, gazeUnitVector.x, gazeUnitVector.y, gazeUnitVector.z, isGazeValid,
                    m_cachedEyePoses[stereoViewIndex].position.x,
                    m_cachedEyePoses[stereoViewIndex].position.y,
                    m_cachedEyePoses[stereoViewIndex].position.z,
                    q.w, q.x, q.y, q.z);
            }
            if (IsTraceEnabled()) {
                LogDebug("  xrLocateViews[{}]: gazeUnitVector=({},{},{}), isGazeValid={}\n",
                    stereoViewIndex, gazeUnitVector.x, gazeUnitVector.y, gazeUnitVector.z, isGazeValid);
            }

            // Blink freeze logic: when eye tracking is lost (wasCacheUsed), hold the
            // last valid focus-view FOV instead of falling back to cached FOV.
            // This prevents the post-blink blur caused by the transition from
            // pre-blink to post-blink gaze position.
            const bool blinkFreezeEnabled = m_config.m_blinkFreezeFocusRegion;

            if (!isGazeValid || !ProjectPoint(viewForGazeProjection, gazeUnitVector, projectedGaze)) {
                // Gaze is invalid (e.g. blink). If freeze is enabled and we have a frozen FOV,
                // return it and activate freeze. Otherwise fall back to cached FOV.
                if (blinkFreezeEnabled && m_hasFrozenFov[stereoViewIndex]) {
                    views[i].fov = m_frozenFov[stereoViewIndex];
                    m_freezeActive[stereoViewIndex] = true;
                    // Start grace period timer if not already started.
                    auto now = std::chrono::steady_clock::now();
                    if (m_freezeRecoveryTime[stereoViewIndex] == std::chrono::steady_clock::time_point{}) {
                        m_freezeRecoveryTime[stereoViewIndex] =
                            now + std::chrono::milliseconds(m_config.m_blinkFreezeGraceMs);
                    }
                } else {
                    views[i].fov = viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO
                                       ? m_cachedEyeFov[xr::StereoView::Count + i]
                                       : m_cachedEyeFov[i];
                    m_freezeActive[stereoViewIndex] = false;
                    m_freezeRecoveryTime[stereoViewIndex] = std::chrono::steady_clock::time_point{};
                }
            } else {
                // Valid gaze: compute the focus-view FOV from gaze.
                if (IsTraceEnabled()) {
                    QVF_TRACE("xrLocateViews",
                              TLArg(i, "ViewIndex"),
                              TLArg(xr::ToString(projectedGaze).c_str(), "ProjectedGaze"));
                }
                // [DEBUG-QVF-POS] Log the projected gaze NDC and the center-of-FOV it is
                // measured against. If projectedGaze.x is ~+0.4 while looking straight
                // ahead, the raw gaze or cached pose is skewed right.
                {
                    const XrVector2f& c = m_centerOfFov[stereoViewIndex];
                    LogDebug("  xrLocateViews[{}]: projectedGaze=({:.4f},{:.4f}) centerOfFov=({:.4f},{:.4f})\n",
                        stereoViewIndex, projectedGaze.x, projectedGaze.y, c.x, c.y);
                }
                m_eyeGaze[stereoViewIndex] = projectedGaze;
                m_eyeGaze[stereoViewIndex] = m_eyeGaze[stereoViewIndex] +
                                             XrVector2f{stereoViewIndex == xr::StereoView::Left ? -m_config.m_horizontalFocusOffset
                                                                                                  : m_config.m_horizontalFocusOffset,
                                                        m_config.m_verticalFocusOffset};
                const XrVector2f v = m_eyeGaze[stereoViewIndex] - m_centerOfFov[stereoViewIndex];
                const float horizontalFovSection =
                    m_config.m_horizontalFovSection[1] *
                    (1.f + (std::clamp(abs(v.x) - m_config.m_focusWideningDeadzone, 0.f, 1.f) *
                            m_config.m_horizontalFocusWideningMultiplier));
                const float verticalFovSection =
                    m_config.m_verticalFovSection[1] *
                    (1.f + (std::clamp(abs(v.y) - m_config.m_focusWideningDeadzone, 0.f, 1.f) *
                            m_config.m_verticalFocusWideningMultiplier));
                const XrVector2f min{std::clamp(m_eyeGaze[stereoViewIndex].x - horizontalFovSection, -1.f, 1.f),
                                     std::clamp(m_eyeGaze[stereoViewIndex].y - verticalFovSection, -1.f, 1.f)};
                const XrVector2f max{std::clamp(m_eyeGaze[stereoViewIndex].x + horizontalFovSection, -1.f, 1.f),
                                     std::clamp(m_eyeGaze[stereoViewIndex].y + verticalFovSection, -1.f, 1.f)};
                if (IsTraceEnabled()) {
                    QVF_TRACE("xrLocateViews",
                              TLArg(i, "ViewIndex"),
                              TLArg(xr::ToString(min).c_str(), "FocusTopLeft"),
                              TLArg(xr::ToString(max).c_str(), "FocusBottomRight"));
                    LogDebug("  xrLocateViews[{}]: projectedGaze=({},{}), eyeGaze=({},{}), centerOfFov=({},{}), v=({},{}), min=({},{}), max=({},{}), hSection={}, vSection={}\n",
                        stereoViewIndex,
                        projectedGaze.x, projectedGaze.y,
                        m_eyeGaze[stereoViewIndex].x, m_eyeGaze[stereoViewIndex].y,
                        m_centerOfFov[stereoViewIndex].x, m_centerOfFov[stereoViewIndex].y,
                        v.x, v.y,
                        min.x, min.y, max.x, max.y,
                        horizontalFovSection, verticalFovSection);
                }
                XrFovf computedFov = xr::math::ComputeBoundingFov(m_cachedEyeFov[stereoViewIndex], min, max);

                // Stabilize the focus-view FOV against gaze jitter.
                views[i].fov = m_fovStabilizer.stabilize(stereoViewIndex, computedFov);

                // Update frozen FOV state.
                if (blinkFreezeEnabled) {
                    if (m_freezeActive[stereoViewIndex] && m_hasFrozenFov[stereoViewIndex]) {
                        // We're in a freeze period (triggered by a previous blink/cache-use).
                        // Check if the new gaze is near the frozen region.
                        if (IsGazeNearFrozenFov(viewForGazeProjection, gazeUnitVector, m_frozenFov[stereoViewIndex])) {
                            // Gaze is near frozen region: resume normal, update frozen FOV.
                            m_frozenFov[stereoViewIndex] = views[i].fov;
                            m_hasFrozenFov[stereoViewIndex] = true;
                            m_freezeActive[stereoViewIndex] = false;
                            m_freezeRecoveryTime[stereoViewIndex] = std::chrono::steady_clock::time_point{};
                        } else {
                            // Gaze jumped far: hold frozen FOV for grace period.
                            auto now = std::chrono::steady_clock::now();
                            if (now < m_freezeRecoveryTime[stereoViewIndex]) {
                                // Still in grace period: return frozen FOV.
                                views[i].fov = m_frozenFov[stereoViewIndex];
                            } else {
                                // Grace period expired: resume normal, update frozen FOV.
                                m_frozenFov[stereoViewIndex] = views[i].fov;
                                m_hasFrozenFov[stereoViewIndex] = true;
                                m_freezeActive[stereoViewIndex] = false;
                                m_freezeRecoveryTime[stereoViewIndex] = std::chrono::steady_clock::time_point{};
                            }
                        }
                    } else {
                        // Normal frame (no active freeze): update frozen FOV.
                        m_frozenFov[stereoViewIndex] = views[i].fov;
                        m_hasFrozenFov[stereoViewIndex] = true;
                    }
                }
            }
        }
    }

    // cacheStereoView() removed: FOV and poses are now lazily provided by the caller
    // (xrLocateViews) which already has valid view data from the proper frame loop.

} // namespace openxr_api_layer
