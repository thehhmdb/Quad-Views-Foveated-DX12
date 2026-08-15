// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
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

#pragma once

#include "compositor_base.h"
#include "d3d12_descriptor_layout.h"
#include "d3d12_constant_buffer.h"
#include <memory>
#include <unordered_map>

namespace openxr_api_layer {

    // RTV cache key for D3D12 destination images.
    struct RtvCacheKey {
        ID3D12Resource* resource;
        uint32_t arraySlice;
        bool operator==(const RtvCacheKey& o) const {
            return resource == o.resource && arraySlice == o.arraySlice;
        }
    };
    struct RtvCacheKeyHash {
        size_t operator()(const RtvCacheKey& k) const {
            return std::hash<ID3D12Resource*>{}(k.resource) ^ (std::hash<uint32_t>{}(k.arraySlice) << 1);
        }
    };

    // Per-swapchain graphics state for D3D12.
    // Inherits full-FOV lifecycle fields from SwapchainGraphicsStateBase.
    struct D3D12SwapchainGraphicsState : public SwapchainGraphicsStateBase {
        // Cached swapchain images (ComPtr ensures resources stay alive while cached)
        std::vector<ComPtr<ID3D12Resource>> images;
        std::vector<ComPtr<ID3D12Resource>> fullFovSwapchainImages;

        // Flat images for quad views (4 views: 2 stereo + 2 focus)
        ComPtr<ID3D12Resource> flatImage[xr::QuadView::Count];

        // Sharpened images (2 views: left + right)
        ComPtr<ID3D12Resource> sharpenedImage[xr::StereoView::Count];

        // EASU-upscaled peripheral images (2 views: left + right)
        ComPtr<ID3D12Resource> easuImage[xr::StereoView::Count];
        // Per-state CPU descriptor heap for caching EASU SRV/UAV descriptors
        ComPtr<ID3D12DescriptorHeap> cpuEasuSrvUavHeap;
        bool easuSrvUavCached{false};
        uint64_t cachedEasuSrcAddr{0};
        uint64_t cachedEasuDstAddr{0};
        // Bitmask tracking which (frameIndex,viewIndex) GPU heaps have
        // already received the EASU descriptor copy. Bit = frameIndex*2+viewIndex.
        // Cleared whenever the CPU cache is rebuilt (easuSrvUavCached set true).
        uint32_t easuGpuCopyMask{0};
        // Number of mip levels in easuImage (set at allocation time).
        // Initialize to 1 (not 0) for all eyes — a value of 0 would create an
        // invalid SRV (MipLevels=0) if an eye's texture is reused without
        // reallocation.
        uint32_t easuMipCount[xr::StereoView::Count]{1, 1};

        // Per-state CPU descriptor heap for caching SRV/UAV descriptors
        // (avoids per-frame CreateShaderResourceView / CreateUnorderedAccessView overhead)
        ComPtr<ID3D12DescriptorHeap> cpuSrvUavHeap;
        bool srvUavCached{false};
        uint64_t cachedFlatFocusAddr{0};
        uint64_t cachedSharpenedAddr{0};
        // Bitmask for sharpen descriptor GPU copies (per frame/view heap).
        uint32_t sharpenGpuCopyMask{0};

        // Projection SRV cache: avoids per-frame CreateShaderResourceView +
        // CopyDescriptorsSimple in bindProjectionSRVs. Keyed per view because
        // the stereo/focus textures differ between eyes; a shared cache would
        // thrash (rebuild every frame) as left/right alternate.
        struct ProjectionSrvCache {
            ComPtr<ID3D12DescriptorHeap> cpuHeap;  // 2 slots: stereo SRV, focus SRV
            bool cached{false};
            uint64_t cachedStereoAddr{0};
            uint64_t cachedFocusAddr{0};
            uint32_t cachedStereoMipCount{0};
            DXGI_FORMAT cachedStereoFormat{};
            DXGI_FORMAT cachedFocusFormat{};
            // Bitmask tracking which (frameIndex,viewIndex) GPU heaps have
            // received the copy. Cleared on rebuild.
            uint32_t gpuCopyMask{0};
        };
        ProjectionSrvCache projectionSrvCache[xr::StereoView::Count];

        // RTV cache for destination images (keyed by resource + array slice)
        std::unordered_map<RtvCacheKey, D3D12_CPU_DESCRIPTOR_HANDLE, RtvCacheKeyHash> rtvCache;
    };

    // D3D12 implementation of the compositor interface.
    class D3D12Compositor : public BaseCompositor<D3D12Compositor, D3D12SwapchainGraphicsState> {
    public:
        D3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi);
        // Call destroy() in the destructor so resources are released even if destroy() was not
        // called explicitly (e.g. due to an exception). destroy() is idempotent.
        ~D3D12Compositor() override {
            destroy();
        }

        bool initialize(int32_t swapchainFormat) override;
        void destroy() override;
        bool isInitialized() const override;
        void waitForGpuIdle() override;

        // Returns true if the D3D12 device is still alive (not removed). Use this
        // before any GPU-sync or resource-release operation to avoid crashing
        // when the app's device has been torn down out from under us.
        bool IsDeviceValid() const;

        // compositeView is inherited from BaseCompositor — it calls CRTP hooks below.

        // Called by BaseCompositor::NeedsReallocate via CRTP
        static void GetTextureDesc(const void* texture,
                                    uint32_t& outWidth,
                                    uint32_t& outHeight,
                                    uint32_t& outFormat) {
            auto* d3d12Res = static_cast<ID3D12Resource*>(const_cast<void*>(texture));
            auto desc = d3d12Res->GetDesc();
            outWidth = static_cast<uint32_t>(desc.Width);
            outHeight = static_cast<uint32_t>(desc.Height);
            outFormat = static_cast<uint32_t>(desc.Format);
        }

        // Called by BaseCompositor::NeedsReallocateWithMips via CRTP
        static void GetTextureDescWithMips(const void* texture,
                                            uint32_t& outWidth,
                                            uint32_t& outHeight,
                                            uint32_t& outFormat,
                                            uint32_t& outMipCount) {
            auto* d3d12Res = static_cast<ID3D12Resource*>(const_cast<void*>(texture));
            auto desc = d3d12Res->GetDesc();
            outWidth = static_cast<uint32_t>(desc.Width);
            outHeight = static_cast<uint32_t>(desc.Height);
            outFormat = static_cast<uint32_t>(desc.Format);
            outMipCount = static_cast<uint32_t>(desc.MipLevels);
        }

    private:
        // Allow the CRTP base class to access private hooks
        friend BaseCompositor<D3D12Compositor, D3D12SwapchainGraphicsState>;

        // --- CRTP hooks called by BaseCompositor::compositeView ---

        void populateSwapchainImagesCache(D3D12SwapchainGraphicsState& state, XrSwapchain swapchain, bool isFullFov);

        bool acquireAndResolveImages(
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            void*& outSourceStereo,
            void*& outSourceFocus,
            void*& outDestination);

        void BindDirectSource(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, uint32_t format);
        bool NeedsFlatReallocate(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format);
        void CreateFlatImage(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format);
        void CopySubImage(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, const XrCompositionLayerProjectionView& view);

        void sharpenFocusView(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& focusState);

        // Stage 2.5: EASU upscale peripheral texture
        void upscalePeripheralEASU(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& stereoView,
            const SwapchainInfo& stereoSwapchain,
            D3D12SwapchainGraphicsState& stereoState);

        void renderProjection(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            void* destination);

        void cleanupAndRelease(
            const CompositorParams& params,
            D3D12SwapchainGraphicsState& stereoState);

        // --- Internal helpers (not CRTP hooks) ---

        void waitForPreviousFrame(uint32_t viewIndex);
        void submitAndSync(
            ID3D12GraphicsCommandList* cmdList,
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            ID3D12Resource* directBoundStereo,
            ID3D12Resource* directBoundFocus,
            ID3D12Resource* destination);
        void bindProjectionSRVs(
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            uint32_t frameIndex);
        D3D12_CPU_DESCRIPTOR_HANDLE getOrCreateRTV(
            D3D12SwapchainGraphicsState& state,
            ID3D12Resource* destination,
            uint32_t arraySlice,
            DXGI_FORMAT format);
        ComPtr<ID3D12GraphicsCommandList>& resetCommandList(uint32_t viewIndex);

        // --- State tracked across pipeline stages ---
        ID3D12GraphicsCommandList* m_currentCmdList{nullptr};
        ID3D12Resource* m_currentDestination{nullptr};
        ID3D12Resource* m_directBoundStereo{nullptr};
        ID3D12Resource* m_directBoundFocus{nullptr};
        uint32_t m_frameIndex{0};

        ComPtr<ID3D12Device> m_device;
        ComPtr<ID3D12CommandQueue> m_queue;

        // Cached descriptor handle increment sizes (constant for the
        // device's lifetime). Populated once in initialize() to avoid repeated
        // GetDescriptorHandleIncrementSize calls on the per-frame hot path.
        uint32_t m_cbvSrvUavIncSize{0};
        uint32_t m_samplerIncSize{0};
        uint32_t m_rtvIncSize{0};

        // Composition resources
        static constexpr uint32_t kFrameCount = 3; // Triple-buffered for 3-frame pipelining
        // RTV descriptor heap capacity. Headroom for triple-buffered
        // swapchains x 2 array slices x multiple swapchain sets without thrashing.
        static constexpr uint32_t kRtvHeapSize = 32;

        // Triple-buffered CBV/SRV heaps (one per frame per eye)
        ComPtr<ID3D12DescriptorHeap> m_cbvSrvHeap[kFrameCount][xr::StereoView::Count];
        ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        ComPtr<ID3D12DescriptorHeap> m_samplerHeap; // Index 0: peripheral, Index 1: focus
        // Peripheral sampler (index 0) is rebuilt when the config changes.
        bool m_peripheralSamplerDirty{true};
        float m_cachedLodBias{0.0f};
        uint32_t m_cachedAnisotropy{0};
        void updatePeripheralSampler(const CompositorParams& params);
        uint32_t m_currentFrameIndex{0}; // Tracks which frame's resources to use
        ComPtr<ID3D12PipelineState> m_projectionPSO;
        ComPtr<ID3D12PipelineState> m_sharpeningPSO;
        ComPtr<ID3D12PipelineState> m_easuPSO;  // EASU
        ComPtr<ID3D12PipelineState> m_mipGenPSO;  // Mip gen
        ComPtr<ID3D12RootSignature> m_projectionRootSignature;
        ComPtr<ID3D12RootSignature> m_sharpeningRootSignature;
        ComPtr<ID3D12RootSignature> m_easuRootSignature;  // EASU
        ComPtr<ID3D12RootSignature> m_mipGenRootSignature;  // Mip gen

        // Per-frame upload heaps for constant buffers
        ComPtr<ID3D12Resource> m_projectionVSConstants[kFrameCount];
        ComPtr<ID3D12Resource> m_projectionPSConstants[kFrameCount];
        ComPtr<ID3D12Resource> m_sharpeningCSConstants[kFrameCount];
        ComPtr<ID3D12Resource> m_easuConstants[kFrameCount];  // EASU — FSR1Constants
        ComPtr<ID3D12Resource> m_mipGenConstants[kFrameCount];  // Mip gen

        // Typed constant buffer writers (wraps persistently-mapped CPU pointers)
        ConstantBufferWriter m_vsConstantWriters[kFrameCount]{};
        ConstantBufferWriter m_psConstantWriters[kFrameCount]{};

        // Persistently-mapped CPU pointers for EASU + sharpening upload
        // buffers. Mapped once in initialize(), unmapped in destroy(). Written
        // via memcpy each frame (avoids per-frame Map/Unmap kernel transitions).
        uint8_t* m_easuConstantsMapped[kFrameCount]{};
        uint8_t* m_sharpeningConstantsMapped[kFrameCount]{};

        ComPtr<ID3D12Resource> m_blankTexture;
        ComPtr<ID3D12CommandAllocator> m_compositionAllocator[xr::StereoView::Count];
        ComPtr<ID3D12Fence> m_compositionFence;
        UINT64 m_fenceValue{0};
        // Cached fence event reused across all GPU wait sites (avoids per-frame
        // CreateEvent/CloseHandle overhead). Created in initialize(), closed in destroy().
        HANDLE m_fenceEvent{nullptr};

        // Double-buffered command lists to avoid per-frame CreateCommandList overhead
        ComPtr<ID3D12GraphicsCommandList> m_cmdList[xr::StereoView::Count][2];
        uint32_t m_cmdListIndex{0};

        // NOTE: m_swapchainStates and m_swapchainStatesMutex are now inherited
        //       from BaseCompositor.
    };

} // namespace openxr_api_layer
