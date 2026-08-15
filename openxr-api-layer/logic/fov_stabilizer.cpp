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
#include "logic/fov_stabilizer.h"

namespace openxr_api_layer {

    FovStabilizer::FovStabilizer(const FoveationConfig& config) {
        m_params.enabled         = config.m_stabilizeFocusFov;
        m_params.hysteresisRad   = config.m_focusFovHysteresis;
        m_params.smoothingFactor = config.m_focusFovSmoothing;
    }

    void FovStabilizer::reset() {
        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            m_hasLastTarget[i] = false;
            m_hasLastSent[i]   = false;
        }
    }

    XrFovf FovStabilizer::stabilize(uint32_t idx, const XrFovf& target) {
        if (!m_params.enabled) {
            return target;
        }

        // --- Hysteresis: only contract if target is meaningfully smaller. ---
        // angleLeft/angleDown are "negative" edges (more negative = larger FOV).
        // angleRight/angleUp are "positive" edges (more positive = larger FOV).
        // Expansion = moving toward larger FOV (always allowed).
        // Contraction = moving toward smaller FOV (blocked unless change > h).
        XrFovf hyst = target;
        if (m_hasLastTarget[idx]) {
            const float h = m_params.hysteresisRad;
            const XrFovf& last = m_lastTargetFov[idx];

            // angleLeft (negative): expansion = target < last; contraction = target > last
            hyst.angleLeft = (target.angleLeft < last.angleLeft)
                ? target.angleLeft
                : ((target.angleLeft > last.angleLeft + h) ? target.angleLeft : last.angleLeft);

            // angleRight (positive): expansion = target > last; contraction = target < last
            hyst.angleRight = (target.angleRight > last.angleRight)
                ? target.angleRight
                : ((target.angleRight < last.angleRight - h) ? target.angleRight : last.angleRight);

            // angleUp (positive): expansion = target > last; contraction = target < last
            hyst.angleUp = (target.angleUp > last.angleUp)
                ? target.angleUp
                : ((target.angleUp < last.angleUp - h) ? target.angleUp : last.angleUp);

            // angleDown (negative): expansion = target < last; contraction = target > last
            hyst.angleDown = (target.angleDown < last.angleDown)
                ? target.angleDown
                : ((target.angleDown > last.angleDown + h) ? target.angleDown : last.angleDown);
        }
        m_lastTargetFov[idx] = hyst;
        m_hasLastTarget[idx] = true;

        // --- Smoothing: lerp sent FOV toward (post-hysteresis) target. ---
        XrFovf sent;
        if (m_hasLastSent[idx]) {
            const float s = m_params.smoothingFactor;
            sent.angleLeft  = m_lastSentFov[idx].angleLeft  + (hyst.angleLeft  - m_lastSentFov[idx].angleLeft)  * s;
            sent.angleRight = m_lastSentFov[idx].angleRight + (hyst.angleRight - m_lastSentFov[idx].angleRight) * s;
            sent.angleUp    = m_lastSentFov[idx].angleUp    + (hyst.angleUp    - m_lastSentFov[idx].angleUp)    * s;
            sent.angleDown  = m_lastSentFov[idx].angleDown  + (hyst.angleDown  - m_lastSentFov[idx].angleDown)  * s;
        } else {
            sent = hyst;
        }
        m_lastSentFov[idx] = sent;
        m_hasLastSent[idx] = true;
        return sent;
    }

} // namespace openxr_api_layer
