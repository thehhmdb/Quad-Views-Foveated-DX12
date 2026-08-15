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
#include <deque>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <memory>

namespace openxr_api_layer {

    class SwapchainManager {
      public:
        struct Swapchain {
            std::deque<uint32_t> acquiredIndex;
            uint32_t lastReleasedIndex{0};
            bool deferredRelease{false};

            XrSwapchainCreateInfo createInfo{};

            // Single shared full-FOV swapchain with arraySize=2 (one layer per eye).
            XrSwapchain fullFovSwapchain{XR_NULL_HANDLE};
        };

        void setDeferredReleaseQuirk(bool enabled) { m_needDeferredSwapchainReleaseQuirk = enabled; }
        bool getDeferredReleaseQuirk() const { return m_needDeferredSwapchainReleaseQuirk; }

        void trackSwapchain(XrSwapchain handle, const XrSwapchainCreateInfo& createInfo);
        void untrackSwapchain(XrSwapchain handle, OpenXrApi* openXrApi);

        // Explicitly destroys all layer-created full-FOV swapchains.
        // Must be called before xrDestroySession.
        void destroyAllFullFovSwapchains(OpenXrApi* openXrApi);

        // Returns a shared_ptr so callers can keep the Swapchain alive for as long as they
        // need it — e.g. across an entire frame of composition, even if untracked meanwhile.
        std::shared_ptr<Swapchain> getSwapchain(XrSwapchain handle);

        void handleAcquire(XrSwapchain handle, OpenXrApi* openXrApi);
        bool handleRelease(XrSwapchain handle);

        std::set<XrSwapchain> checkAndResetDeferredReleases();

      private:
        // shared_mutex: getSwapchain (read) takes a shared lock; all write methods
        // take a unique lock. mutable so shared_lock can be acquired from the const-capable
        // read path.
        mutable std::shared_mutex m_mutex;
        // Stored as shared_ptrs so a Swapchain outlives untracking and stays valid for any
        // in-flight frame that still holds a reference to it.
        std::unordered_map<XrSwapchain, std::shared_ptr<Swapchain>> m_swapchains;
        bool m_needDeferredSwapchainReleaseQuirk{false};
    };

} // namespace openxr_api_layer
