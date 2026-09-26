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
#include "logic/layer_composer.h"
#include "framework/log.h"
#include "framework/util.h"
#include "compositor.h"
#include "logic/dither_offset_provider.h"
#include "views.h"

namespace openxr_api_layer {

    using namespace log;
    using namespace xr;

    LayerComposer::LayerComposer(OpenXrApi* openXrApi,
                                 FoveationConfig& config,
                                 ViewManager& viewManager,
                                 SwapchainManager& swapchainManager,
                                 GraphicsContext& graphicsContext,
                                 EyeTracker& eyeTracker,
                                 FocusFovQuirk& focusFovQuirk)
        : m_openXrApi(openXrApi), m_config(config), m_viewManager(viewManager),
          m_swapchainManager(swapchainManager), m_graphicsContext(graphicsContext),
          m_eyeTracker(eyeTracker), m_focusFovQuirk(focusFovQuirk) {
    }

    void LayerComposer::clearSwapchainCache() {
        // Clear the map but keep its bucket capacity for reuse next frame.
        m_swapchainCache.clear();
    }

    SwapchainManager::Swapchain* LayerComposer::getCachedSwapchain(XrSwapchain handle) {
        auto it = m_swapchainCache.find(handle);
        if (it != m_swapchainCache.end()) {
            return it->second.get();
        }
        // Miss: do the (locked) lookup once and cache the raw pointer + shared_ptr.
        std::shared_ptr<SwapchainManager::Swapchain> entry = m_swapchainManager.getSwapchain(handle);
        if (!entry) {
            return nullptr;
        }
        m_swapchainCache.emplace(handle, entry);
        return entry.get();
    }

    XrResult LayerComposer::processLayers(XrSession session,
                                          const XrFrameEndInfo* frameEndInfo,
                                          bool useQuadViews,
                                          bool useFovTangent,
                                          bool requestedDepthSubmission,
                                          std::vector<const XrCompositionLayerBaseHeader*>& outLayers,
                                          std::vector<XrSwapchain>& outSwapchainsToRelease) {
        // If the layer's own compositor failed earlier, skip processing and let
        // xrEndFrame fall through with no patched layers — submitting our custom
        // quad-view layers to a broken compositor would fail runtime validation.
        if (m_compositorFailed) {
            // outLayers.assign(frameEndInfo->layers, frameEndInfo->layers + frameEndInfo->layerCount);
            return XR_SUCCESS;
        }

        // Flush so diagnostics logged during this submit survive an unexpected
        // termination.
        if (frameEndInfo->layerCount > 0) {
            log::Flush();
        }

        // Reuse the member allocation buffers (capacity persists across frames).
        m_projectionAllocator.clear();
        m_projectionViewAllocator.clear();
        m_projectionAllocator.reserve(frameEndInfo->layerCount);
        m_projectionViewAllocator.reserve(frameEndInfo->layerCount);

        // Reset the per-frame swapchain lookup cache (bucket capacity persists).
        clearSwapchainCache();

        // Use a plain vector instead of a set to avoid per-insert tree allocations;
        // duplicates are harmless and get deduplicated once at the end of the frame.
        std::vector<XrSwapchain> swapchainsToRelease;

        for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
            if (!frameEndInfo->layers[i]) {
                return XR_ERROR_LAYER_INVALID;
            }

            if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const XrCompositionLayerProjection* proj =
                    reinterpret_cast<const XrCompositionLayerProjection*>(frameEndInfo->layers[i]);

                QVF_TRACE("xrEndFrame_Layer",
                          TLArg(xr::ToCString(proj->type), "Type"),
                          TLArg(proj->layerFlags, "Flags"),
                          TLXArg(proj->space, "Space"));

                if (proj->viewCount != (useQuadViews ? xr::QuadView::Count : xr::StereoView::Count)) {
                    return XR_ERROR_VALIDATION_FAILURE;
                }

                // [DEBUG-QVF] Log the incoming layer exactly as our mod submitted it.
                for (uint32_t v = 0; v < proj->viewCount; ++v) {
                    const auto& pv = proj->views[v];
                    LogDebug("[DEBUG-QVF] input view={} swapchain={:x} arrayIndex={} rect=({},{})-({},{}) "
                             "fov=(L={:.4f} R={:.4f} U={:.4f} D={:.4f})\n",
                             v,
                             (uint64_t)pv.subImage.swapchain,
                             pv.subImage.imageArrayIndex,
                             pv.subImage.imageRect.offset.x, pv.subImage.imageRect.offset.y,
                             pv.subImage.imageRect.extent.width, pv.subImage.imageRect.extent.height,
                             pv.fov.angleLeft, pv.fov.angleRight, pv.fov.angleUp, pv.fov.angleDown);
                }

                m_projectionViewAllocator.push_back(
                    {proj->views[xr::StereoView::Left], proj->views[xr::StereoView::Right]});

                for (uint32_t viewIndex = 0; viewIndex < xr::StereoView::Count; viewIndex++) {
                    if (useQuadViews) {
                        for (uint32_t j = viewIndex; j < xr::QuadView::Count; j += xr::StereoView::Count) {
                            QVF_TRACE("xrEndFrame_View",
                                      TLArg("Color", "Type"),
                                      TLArg(j, "ViewIndex"),
                                      TLXArg(proj->views[j].subImage.swapchain, "Swapchain"),
                                      TLArg(proj->views[j].subImage.imageArrayIndex, "ImageArrayIndex"),
                                      TLArg(xr::ToString(proj->views[j].subImage.imageRect).c_str(), "ImageRect"),
                                      TLArg(xr::ToString(proj->views[j].pose).c_str(), "Pose"),
                                      TLArg(xr::ToString(proj->views[j].fov).c_str(), "Fov"));
                        }
                    }

                    const uint32_t focusViewIndex =
                        useQuadViews ? (viewIndex + xr::StereoView::Count) : viewIndex;

                    // The swapchains are kept alive by shared_ptrs held in the manager,
                    // so these raw pointers stay valid for the whole frame even if the app
                    // destroys a swapchain mid-frame. Lookups go through the per-frame cache
                    // to avoid repeated shared_mutex locks and refcount atomics on the hot path.
                    SwapchainManager::Swapchain* swapchainForStereoView =
                        getCachedSwapchain(proj->views[viewIndex].subImage.swapchain);
                    SwapchainManager::Swapchain* swapchainForFocusView =
                        getCachedSwapchain(proj->views[focusViewIndex].subImage.swapchain);
                    if (!swapchainForStereoView || !swapchainForFocusView) {
                        return XR_ERROR_HANDLE_INVALID;
                    }

                    if (swapchainForStereoView->deferredRelease) {
                        swapchainsToRelease.push_back(proj->views[viewIndex].subImage.swapchain);
                        swapchainForStereoView->deferredRelease = false;
                    }
                    if (swapchainForFocusView->deferredRelease) {
                        swapchainsToRelease.push_back(proj->views[focusViewIndex].subImage.swapchain);
                        swapchainForFocusView->deferredRelease = false;
                    }

                    // Allocate a shared destination swapchain (arraySize=2, one layer per eye).
                    // SteamVR D3D12 runtime may reject multiple large swapchains, so we use a single
                    // array swapchain shared by both eyes.
                    if (swapchainForStereoView->fullFovSwapchain == XR_NULL_HANDLE) {
                        XrSwapchainCreateInfo createInfo = swapchainForStereoView->createInfo;
                        createInfo.arraySize = xr::StereoView::Count;
                        createInfo.width = m_viewManager.m_fullFovResolution.width;
                        createInfo.height = m_viewManager.m_fullFovResolution.height;
                        // Use only the flags needed for composition (render target + shader resource).
                        // Don't inherit extra flags from the app swapchain.
                        createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
                        // Clear the dangling next pointer from the shallow copy.
                        // D3D12 graphics bindings are session-level, not swapchain-level.
                        createInfo.next = nullptr;

                        LogDebug("xrEndFrame_CreateSwapchain: format={}, usageFlags=0x{:x}, arraySize={}, width={}x{}\n",
                                        createInfo.format, createInfo.usageFlags, createInfo.arraySize,
                                        createInfo.width, createInfo.height);
                        QVF_TRACE("xrEndFrame_CreateSwapchain",
                                  TLArg(m_viewManager.m_fullFovResolution.width, "Width"),
                                  TLArg(m_viewManager.m_fullFovResolution.height, "Height"));
                        const XrResult swapchainResult = m_openXrApi->OpenXrApi::xrCreateSwapchain(
                            session, &createInfo, &swapchainForStereoView->fullFovSwapchain);
                        if (swapchainResult != XR_SUCCESS) {
                            LogWarning("xrEndFrame_CreateSwapchain failed with XrResult={}\n", static_cast<int>(swapchainResult));
                            return XR_ERROR_RUNTIME_FAILURE;
                        }
                        // Checkpoint flush: the full-FOV destination swapchain exists.
                        log::Flush();
                    }

                    XrCompositionLayerProjectionView focusView = proj->views[focusViewIndex];
                    if (useQuadViews && m_focusFovQuirk.isEnabled()) {
                        // Quirk for DCS World: the application does not pass the correct FOV for the
                        // focus views in xrEndFrame(). We must keep track of the correct values for
                        // each frame.
                        m_focusFovQuirk.lookupFov(frameEndInfo->displayTime, focusViewIndex, focusView.fov);
                    }

                    // Composite the focus view and the stereo view together into a single stereo view.
                    compositeViewContent(viewIndex,
                                         proj->views[viewIndex],
                                         *swapchainForStereoView,
                                         focusView,
                                         *swapchainForFocusView,
                                         proj->layerFlags,
                                         useQuadViews);
                    // Checkpoint flush: this view's composition completed.
                    log::Flush();

                    // If compositing this view failed, stop patching layers and fall back to
                    // submitting the app's original layers unchanged.
                    if (m_compositorFailed) {
                        outLayers.assign(frameEndInfo->layers, frameEndInfo->layers + frameEndInfo->layerCount);
                        return XR_SUCCESS;
                    }

                    // Patch the view to reference the new swapchain at full FOV.
                    XrCompositionLayerProjectionView& patchedView =
                        m_projectionViewAllocator.back()[viewIndex];
                    patchedView.fov = m_viewManager.m_cachedEyeFov[viewIndex];
                    patchedView.subImage.swapchain = swapchainForStereoView->fullFovSwapchain;
                    patchedView.subImage.imageArrayIndex = viewIndex;
                    patchedView.subImage.imageRect.offset = {0, 0};
                    patchedView.subImage.imageRect.extent = m_viewManager.m_fullFovResolution;

                    if (requestedDepthSubmission && m_swapchainManager.getDeferredReleaseQuirk()) {
                        const XrBaseInStructure* entry =
                            reinterpret_cast<const XrBaseInStructure*>(proj->views[viewIndex].next);
                        while (entry) {
                            if (entry->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
                                const XrCompositionLayerDepthInfoKHR* depth =
                                    reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(entry);

                                QVF_TRACE("xrEndFrame_View",
                                          TLArg("Depth", "Type"),
                                          TLArg(viewIndex, "ViewIndex"),
                                          TLXArg(depth->subImage.swapchain, "Swapchain"),
                                          TLArg(depth->subImage.imageArrayIndex, "ImageArrayIndex"),
                                          TLArg(xr::ToString(depth->subImage.imageRect).c_str(), "ImageRect"),
                                          TLArg(depth->nearZ, "Near"),
                                          TLArg(depth->farZ, "Far"),
                                          TLArg(depth->minDepth, "MinDepth"),
                                          TLArg(depth->maxDepth, "MaxDepth"));

                                SwapchainManager::Swapchain* swapchainForDepthInfo =
                                    getCachedSwapchain(depth->subImage.swapchain);
                                if (!swapchainForDepthInfo) {
                                    return XR_ERROR_HANDLE_INVALID;
                                }

                                if (swapchainForDepthInfo->deferredRelease) {
                                    swapchainsToRelease.push_back(depth->subImage.swapchain);
                                    swapchainForDepthInfo->deferredRelease = false;
                                }
                            }
                            entry = entry->next;
                        }
                    }
                }

                // Note: if a depth buffer was attached, we will use it as-is (per copy of the proj
                // struct below, and therefore its entire chain of next structs). This is good: we will
                // submit a depth that matches the composited view, but that is lower resolution.

                m_projectionAllocator.push_back(*proj);
                // Our shader always premultiplies the alpha channel.
                m_projectionAllocator.back().layerFlags &= ~XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                m_projectionAllocator.back().views = m_projectionViewAllocator.back().data();
                m_projectionAllocator.back().viewCount = xr::StereoView::Count;
                // [DEBUG-QVF] Log the composed layer exactly as SteamVR will see it.
                for (uint32_t v = 0; v < m_projectionAllocator.back().viewCount; ++v) {
                    const auto& pv = m_projectionAllocator.back().views[v];
                    LogDebug("[DEBUG-QVF] composed view={} swapchain={:x} arrayIndex={} rect=({},{})-({},{}) "
                             "fov=(L={:.4f} R={:.4f} U={:.4f} D={:.4f}) pose=({:.3f},{:.3f},{:.3f})\n",
                             v,
                             (uint64_t)pv.subImage.swapchain,
                             pv.subImage.imageArrayIndex,
                             pv.subImage.imageRect.offset.x, pv.subImage.imageRect.offset.y,
                             pv.subImage.imageRect.extent.width, pv.subImage.imageRect.extent.height,
                             pv.fov.angleLeft, pv.fov.angleRight, pv.fov.angleUp, pv.fov.angleDown,
                             pv.pose.position.x, pv.pose.position.y, pv.pose.position.z);
                }
                outLayers.push_back(
                    reinterpret_cast<XrCompositionLayerBaseHeader*>(&m_projectionAllocator.back()));

            } else {
                if (m_swapchainManager.getDeferredReleaseQuirk()) {
                    if (frameEndInfo->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                        const XrCompositionLayerQuad* quad =
                            reinterpret_cast<const XrCompositionLayerQuad*>(frameEndInfo->layers[i]);

                        SwapchainManager::Swapchain* swapchainEntry =
                            getCachedSwapchain(quad->subImage.swapchain);
                        if (swapchainEntry && swapchainEntry->deferredRelease) {
                            swapchainsToRelease.push_back(quad->subImage.swapchain);
                            swapchainEntry->deferredRelease = false;
                        }
                    }
                    // TODO: We need to handle all other types of composition layers in order to mark
                    // the swapchains for deferred release. Luckily we only need this quirk on Varjo and
                    // the runtime does not support any other type of composition layers.
                }

                QVF_TRACE("xrEndFrame_Layer",
                          TLArg(xr::ToCString(frameEndInfo->layers[i]->type), "Type"));
                outLayers.push_back(frameEndInfo->layers[i]);
            }
        }

        // Deduplicate before release — processLayers() may have pushed the same
        // swapchain more than once. sort + unique keeps the "each released exactly
        // once" guarantee a set would have given, without per-insert tree allocations.
        std::sort(swapchainsToRelease.begin(), swapchainsToRelease.end());
        swapchainsToRelease.erase(std::unique(swapchainsToRelease.begin(), swapchainsToRelease.end()),
                                  swapchainsToRelease.end());
        outSwapchainsToRelease = std::move(swapchainsToRelease);
        // Checkpoint flush: all layers processed and patched.
        log::Flush();
        return XR_SUCCESS;
    }

    void LayerComposer::compositeViewContent(uint32_t viewIndex,
                                              const XrCompositionLayerProjectionView& stereoView,
                                              SwapchainManager::Swapchain& swapchainForStereoView,
                                              const XrCompositionLayerProjectionView& focusView,
                                              SwapchainManager::Swapchain& swapchainForFocusView,
                                              XrCompositionLayerFlags layerFlags,
                                              bool useQuadViews) {
        // Lazy initialization of the compositor resources.
        if (!m_graphicsContext.getCompositor()->isInitialized()) {
            LogDebug("Initializing compositor resources (format={})\n",
                            swapchainForStereoView.createInfo.format);

            try {
                bool initSuccess = m_graphicsContext.getCompositor()->initialize(static_cast<int32_t>(swapchainForStereoView.createInfo.format));
                if (!initSuccess) {
                    throw std::runtime_error("Compositor initialize() returned false");
                }
                LogDebug("Compositor resources initialized\n");
            } catch (const std::exception& e) {
                LogError("Compositor initialization failed: {}\n", e.what());
                m_compositorFailed = true;
                return;
            } catch (...) {
                LogError("Compositor initialization failed with unknown exception\n");
                m_compositorFailed = true;
                return;
            }
        }

        // Re-check isInitialized() as a safety net: if initialization threw and was caught
        // upstream, the compositor will be in an uninitialized state. Skip composition to
        // prevent null dereference crash.
        if (!m_graphicsContext.getCompositor()->isInitialized()) {
            LogError("Compositor initialization failed, skipping composition for this frame.\n");
            m_compositorFailed = true;
            return;
        }

        // Build compositor parameters
        CompositorParams params;
        params.viewIndex = viewIndex;
        params.cachedEyeFov = m_viewManager.m_cachedEyeFov[viewIndex];
        params.fullFovResolution = m_viewManager.m_fullFovResolution;
        params.useQuadViews = useQuadViews;
        params.smoothenFocusViewEdges = m_config.m_smoothenFocusViewEdges;
        params.sharpenFocusView = m_config.m_sharpenFocusView;
        params.featherFocusEdges = m_config.m_featherFocusEdges;
        params.debugFocusView = m_config.m_debugFocusView;
        params.debugEyeGaze = m_config.m_debugEyeGaze;
        params.eyeGaze = m_viewManager.m_eyeGaze[viewIndex];
        params.layerFlags = layerFlags;
        params.transitionDitherAmount = m_config.m_transitionDitherAmount;
        static uint32_t s_frameCount = 0;
        params.frameCount = s_frameCount++;
        params.useFSR1EASU = m_config.m_useFSR1EASU;
        params.skipMipGen = !m_config.m_useEasuMipGen;
        params.peripheralLodBias = m_config.m_peripheralLodBias;
        params.peripheralAnisotropy = m_config.m_peripheralAnisotropy;
        params.peripheralEdgeBlur = m_config.m_peripheralEdgeBlur;
        params.boundaryDesaturation = m_config.m_boundaryDesaturation;
        params.radialLodStart = m_config.m_radialLodStart;
        params.radialLodEnd = m_config.m_radialLodEnd;
        params.radialLodMaxBoost = m_config.m_radialLodMaxBoost;
        params.focusAspect = m_config.m_focusAspect;
        // Compute the per-frame blue-noise dither offset. When the
        // feature is disabled, fall back to the legacy integer rotation so
        // existing visual behavior is preserved.
        if (m_config.m_useBlueNoiseDither) {
            params.blueNoiseOffset = DitherOffsetProvider::offset(params.frameCount);
        } else {
            params.blueNoiseOffset = DirectX::XMFLOAT2(
                static_cast<float>(params.frameCount & 7),
                static_cast<float>((params.frameCount >> 3) & 7));
        }

        // Build swapchain info
        SwapchainInfo stereoSwapchainInfo;
        stereoSwapchainInfo.handle = stereoView.subImage.swapchain;
        stereoSwapchainInfo.createInfo = swapchainForStereoView.createInfo;
        stereoSwapchainInfo.fullFovSwapchain = swapchainForStereoView.fullFovSwapchain;
        stereoSwapchainInfo.lastReleasedIndex = swapchainForStereoView.lastReleasedIndex;

        SwapchainInfo focusSwapchainInfo;
        focusSwapchainInfo.handle = focusView.subImage.swapchain;
        focusSwapchainInfo.createInfo = swapchainForFocusView.createInfo;
        focusSwapchainInfo.fullFovSwapchain = swapchainForFocusView.fullFovSwapchain;
        focusSwapchainInfo.lastReleasedIndex = swapchainForFocusView.lastReleasedIndex;

        // Defensive validation: a null swapchain handle here means the game passed
        // an uninitialized composition layer. Skip composition rather than crash.
        if (!stereoSwapchainInfo.handle || (useQuadViews && !focusSwapchainInfo.handle)) {
            LogError(
                "compositeViewContent: null swapchain handle detected "
                "(stereo={} focus={}) — skipping composition\n",
                reinterpret_cast<void*>(stereoSwapchainInfo.handle),
                reinterpret_cast<void*>(focusSwapchainInfo.handle));
            m_compositorFailed = true;
            return;
        }

        // Delegate to compositor. An exception escaping here would terminate the
        // application and lose buffered log lines, so catch, log, flush, and
        // fall back to passing the application's original layers through
        // unchanged.
        void* result = nullptr;
        try {
            result = m_graphicsContext.getCompositor()->compositeView(params,
                                       stereoSwapchainInfo,
                                       stereoView,
                                       focusSwapchainInfo,
                                       focusView);
        } catch (const std::exception& e) {
            LogError("Compositor compositeView threw: {}\n", e.what());
            log::Flush();
            m_compositorFailed = true;
            return;
        } catch (...) {
            LogError("Compositor compositeView threw an unknown exception\n");
            log::Flush();
            m_compositorFailed = true;
            return;
        }
        if (!result) {
            LogError("Compositor returned null destination — aborting composition.\n");
            log::Flush();
            m_compositorFailed = true;
            return;
        }
    }

} // namespace openxr_api_layer
