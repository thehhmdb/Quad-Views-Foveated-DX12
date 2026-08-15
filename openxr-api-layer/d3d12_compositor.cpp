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

#include "pch.h"
#include "d3d12_compositor.h"

#include "log.h"
#include "util.h"
#include "views.h"
#include "framework/dispatch.gen.h"
#include "logic/compositor_shared.h"
#include "logic/easu_params.h"
#include "utils/d3d12_helpers.h"

#include <ProjectionVS.h>
#include <ProjectionPS.h>
#include <SharpeningCS.h>
#include <FSR1EASU_CS.h>
#include <MipGenCS.h>

namespace openxr_api_layer {

    using namespace log;
    using namespace xr::math;

    // Compute the bitmask bit for a (frameIndex, viewIndex) GPU heap slot.
    // m_cbvSrvHeap is triple-buffered [kFrameCount][StereoView::Count]; a descriptor
    // copied into one heap slot is NOT present in the others, so the copy-cache mask
    // must be keyed per slot. kFrameCount=3, StereoView::Count=2 -> 6 bits used.
    static inline uint32_t gpuCopyBit(uint32_t frameIndex, uint32_t viewIndex) {
        return 1u << (frameIndex * xr::StereoView::Count + viewIndex);
    }

    // Dump all pending D3D12 debug-layer (info queue) messages to the log.
    // When the device is removed with DXGI_ERROR_INVALID_CALL (0x887A0001), the
    // debug layer is active and has queued the exact validation error that
    // caused the removal. Draining the info queue tells us precisely which
    // command/barrier/descriptor was invalid instead of guessing.
    static void DumpD3D12InfoQueue(ID3D12Device* device, const char* contextTag) {
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(infoQueue.ReleaseAndGetAddressOf()))) || !infoQueue) {
            LogDebug("D3D12 InfoQueue [{}]: not available (debug layer not active)\n", contextTag);
            return;
        }
        const UINT64 numMessages = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
        LogDebug("D3D12 InfoQueue [{}]: {} message(s)\n", contextTag, (uint32_t)numMessages);
        for (UINT64 i = 0; i < numMessages; i++) {
            SIZE_T messageLength = 0;
            if (FAILED(infoQueue->GetMessage(i, nullptr, &messageLength)))
                continue;
            std::vector<uint8_t> buffer(messageLength);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (FAILED(infoQueue->GetMessage(i, message, &messageLength)))
                continue;
            LogDebug("D3D12 InfoQueue [{}]: [{}:{}:{}] {}\n",
                     contextTag,
                     (uint32_t)message->Category,
                     (uint32_t)message->Severity,
                     (uint32_t)message->ID,
                     message->pDescription ? message->pDescription : "(null)");
        }
        infoQueue->ClearStoredMessages();
    }

    // Format used for the sharpened (CAS output) texture.
    // R16G16B16A16_FLOAT halves CAS render-target bandwidth vs 32-bit. The CAS
    // shader now clamps its output to the fp16 range (±65504), so the HDR
    // device-removed crashes that previously forced R32G32B32A32_FLOAT are
    // prevented. This matches the EASU format and the D3D11 path.
    constexpr DXGI_FORMAT kSharpenedFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // Format used for the EASU-upscaled peripheral intermediate texture and its
    // mip chain. R16G16B16A16_FLOAT halves memory bandwidth vs 32-bit (EASU is
    // memory-bound) and is proven safe — the D3D11 path already uses it. The
    // EASU shader clamps its output to the 16-bit range to avoid the HDR
    // overflow crashes seen in the CAS path.
    constexpr DXGI_FORMAT kEasuFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12Compositor::D3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi)
        : BaseCompositor(openXrApi)
        , m_device(device)
        , m_queue(queue) {
        // ComPtr's initializing constructor already AddRefs the device/queue; no explicit AddRef needed.
    }

    bool D3D12Compositor::isInitialized() const {
        return m_initialized;
    }

    void D3D12Compositor::populateSwapchainImagesCache(D3D12SwapchainGraphicsState& state, XrSwapchain swapchain, bool isFullFov) {
        auto& images = isFullFov ? state.fullFovSwapchainImages : state.images;
        if (!images.empty()) {
            return;
        }

        uint32_t count;
        CHECK_XRCMD(m_openXrApi->xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr));
        LogDebug("  D3D12: Populating swapchain images cache: swapchain={:x}, count={}\n", (uint64_t)swapchain, count);

        std::vector<XrSwapchainImageD3D12KHR> d3d12Images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        CHECK_XRCMD(m_openXrApi->xrEnumerateSwapchainImages(
            swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(d3d12Images.data())));

        for (uint32_t i = 0; i < count; i++) {
            LogDebug("    Image {}: texture={:p}\n", i, static_cast<void*>(d3d12Images[i].texture));
            images.push_back(d3d12Images[i].texture);
        }
    }

    bool D3D12Compositor::initialize(int32_t swapchainFormat) {
        QVF_TRACE("InitializeCompositionResources", TLArg("D3D12", "Api"));
        LogDebug("D3D12 initializeCompositionResources: starting... (format={})\n", swapchainFormat);

        auto device = m_device.Get();

        // Enable D3D12 debug layer in debug builds for validation
#ifdef _DEBUG
        {
            ComPtr<ID3D12Debug> debugController;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
                debugController->EnableDebugLayer();
                LogDebug("D3D12: Debug layer enabled\n");
            }
        }
#endif

        // Cache descriptor increment sizes once: they are constant for the
        // device's lifetime, and GetDescriptorHandleIncrementSize is a driver
        // call we don't want on the per-frame hot path.
        m_cbvSrvUavIncSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        m_samplerIncSize   = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        m_rtvIncSize       = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // Triple-buffered CBV/SRV heaps for 3-frame pipelining.
        // CBV/SRV heap layout (16 descriptors): see DescriptorLayout for slot assignments.
        for (uint32_t f = 0; f < kFrameCount; f++) {
            for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
                D3D12_DESCRIPTOR_HEAP_DESC desc{};
                desc.NumDescriptors = DescriptorLayout::kHeapSize;
                desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                LogDebug("D3D12: Creating CBV/SRV descriptor heap [{}][{}]...\n", f, i);
                CHECK_HRCMD(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_cbvSrvHeap[f][i].ReleaseAndGetAddressOf())));
            }
        }
        m_currentFrameIndex = 0;

        // Sampler heap
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.NumDescriptors = 2;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            LogDebug("D3D12: Creating sampler descriptor heap...\n");
            CHECK_HRCMD(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_samplerHeap.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Sampler heap created\n");
        }

        // RTV heap: 32 descriptors give headroom for per-image RTV caching
        // across triple-buffered swapchains x 2 array slices x multiple
        // swapchain sets without thrashing. Descriptor heaps are cheap.
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.NumDescriptors = kRtvHeapSize;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            LogDebug("D3D12: Creating RTV descriptor heap...\n");
            CHECK_HRCMD(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_rtvHeap.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: RTV heap created\n");
        }

        // Create default sampler descriptors at both heap indices so neither is
        // ever null. Index 0 (peripheral) is rebuilt with aniso + LOD bias on
        // the first frame by updatePeripheralSampler(); index 1 (focus) stays
        // linear-clamp.
        {
            D3D12_SAMPLER_DESC samplerDesc{};
            samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplerDesc.MipLODBias = 0;
            samplerDesc.MaxAnisotropy = 1;
            samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            samplerDesc.BorderColor[0] = 0;
            samplerDesc.BorderColor[1] = 0;
            samplerDesc.BorderColor[2] = 0;
            samplerDesc.BorderColor[3] = 0;
            samplerDesc.MinLOD = 0;
            samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
            const auto samplerInc = m_samplerIncSize;
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle0(m_samplerHeap->GetCPUDescriptorHandleForHeapStart(), 0, samplerInc);
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle1(m_samplerHeap->GetCPUDescriptorHandleForHeapStart(), 1, samplerInc);
            device->CreateSampler(&samplerDesc, cpuHandle0); // Peripheral default
            device->CreateSampler(&samplerDesc, cpuHandle1); // Focus default
            LogDebug("D3D12: Sampler descriptors created (peripheral + focus)\n");
        }

        // Upload heaps for constant buffers, triple-buffered for 3-frame pipelining.
        {
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Alignment = 0;
            desc.Width = 512;  // 2 eyes x 256-byte CBV stride each
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;

            for (uint32_t f = 0; f < kFrameCount; f++) {
                CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr,
                                                            IID_PPV_ARGS(m_projectionVSConstants[f].ReleaseAndGetAddressOf())));
                CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr,
                                                            IID_PPV_ARGS(m_projectionPSConstants[f].ReleaseAndGetAddressOf())));
                CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr,
                                                            IID_PPV_ARGS(m_sharpeningCSConstants[f].ReleaseAndGetAddressOf())));
            }

            // EASU constant buffers (64 bytes of payload, padded to
            // D3D12's 256-byte minimum CBV alignment).
            {
                CD3DX12_RESOURCE_DESC easuCbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
                for (uint32_t f = 0; f < kFrameCount; f++) {
                    CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &easuCbDesc,
                                                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                                                nullptr,
                                                                IID_PPV_ARGS(m_easuConstants[f].ReleaseAndGetAddressOf())));
                }
            }

            // Mip-gen constant buffers (32 bytes of payload, padded to 256).
            {
                CD3DX12_RESOURCE_DESC mipCbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
                for (uint32_t f = 0; f < kFrameCount; f++) {
                    CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &mipCbDesc,
                                                                D3D12_RESOURCE_STATE_GENERIC_READ,
                                                                nullptr,
                                                                IID_PPV_ARGS(m_mipGenConstants[f].ReleaseAndGetAddressOf())));
                }
            }

            // Persistently map the upload heaps once at init time and keep
            // the CPU pointers valid for the heap's lifetime. This avoids the
            // per-frame Map/Unmap overhead (each is a kernel transition) —
            // the app only ever writes through these pointers.
            D3D12_RANGE readRange{0, 0};
            for (uint32_t f = 0; f < kFrameCount; f++) {
                uint8_t* vsMapped = nullptr;
                uint8_t* psMapped = nullptr;
                CHECK_HRCMD(m_projectionVSConstants[f]->Map(0, &readRange, reinterpret_cast<void**>(&vsMapped)));
                CHECK_HRCMD(m_projectionPSConstants[f]->Map(0, &readRange, reinterpret_cast<void**>(&psMapped)));
                m_vsConstantWriters[f].initialize(vsMapped);
                m_psConstantWriters[f].initialize(psMapped);
                // Same pattern for the EASU + sharpening upload buffers.
                CHECK_HRCMD(m_easuConstants[f]->Map(0, &readRange,
                    reinterpret_cast<void**>(&m_easuConstantsMapped[f])));
                CHECK_HRCMD(m_sharpeningCSConstants[f]->Map(0, &readRange,
                    reinterpret_cast<void**>(&m_sharpeningConstantsMapped[f])));
            }

            // Pre-create the sharpening CS CBV and EASU CS CBV descriptors
            // once at init. These passes still use descriptor tables (they need
            // UAVs, which can't be root descriptors), so their CBVs must live in
            // the descriptor heap. The VS/PS CBVs are no longer pre-created here
            // — the projection pass now uses root CBVs (SetGraphicsRootConstantBufferView),
            // which embed the GPU virtual address directly and bypass the heap.
            const auto cbvInc = m_cbvSrvUavIncSize;
            for (uint32_t f = 0; f < kFrameCount; f++) {
                for (uint32_t v = 0; v < xr::StereoView::Count; v++) {
                    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
                    cbvDesc.SizeInBytes = 256;

                    // Sharpening CS CBV (static per heap)
                    cbvDesc.BufferLocation = m_sharpeningCSConstants[f]->GetGPUVirtualAddress();
                    device->CreateConstantBufferView(&cbvDesc,
                        DescriptorLayout::CpuHandle(m_cbvSrvHeap[f][v].Get(), DescriptorLayout::kSharpenCbv, cbvInc));

                    // EASU CS CBV (static per heap — buffer contents are uploaded each frame,
                    // but the CBV descriptor points at the same buffer for the heap's lifetime).
                    // SizeInBytes must be a multiple of 256 (D3D12 CBV requirement); the buffer
                    // is 256 bytes and only the first 64 (sizeof(FSR1Constants)) are written.
                    cbvDesc.BufferLocation = m_easuConstants[f]->GetGPUVirtualAddress();
                    cbvDesc.SizeInBytes = 256;
                    device->CreateConstantBufferView(&cbvDesc,
                        DescriptorLayout::CpuHandle(m_cbvSrvHeap[f][v].Get(), DescriptorLayout::kEasuCbv, cbvInc));
                }
            }
        }

        {
            const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
            const uint32_t width = 32;
            const uint32_t height = 32;

            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, D3D12_RESOURCE_FLAG_NONE);

            CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                        nullptr,
                                                        IID_PPV_ARGS(m_blankTexture.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Blank texture created successfully\n");
        }

        // Create blank texture SRV at DescriptorLayout::kBlankSrv slot
        {
            const auto incrementSize = m_cbvSrvUavIncSize;
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;

            for (uint32_t f = 0; f < kFrameCount; f++) {
                for (uint32_t h = 0; h < xr::StereoView::Count; h++) {
                    device->CreateShaderResourceView(m_blankTexture.Get(), &srvDesc,
                        DescriptorLayout::CpuHandle(m_cbvSrvHeap[f][h].Get(), DescriptorLayout::kBlankSrv, incrementSize));
                }
            }
            LogDebug("D3D12: Blank texture SRV created\n");
        }

        for (uint32_t i = 0; i < xr::StereoView::Count; i++) {
            CHECK_HRCMD(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(m_compositionAllocator[i].ReleaseAndGetAddressOf())));
        }

        CHECK_HRCMD(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_compositionFence.ReleaseAndGetAddressOf())));
        m_fenceValue = 0;

        // Single reusable fence event (avoids per-frame CreateEvent/CloseHandle).
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent) {
            CHECK_HRCMD(HRESULT_FROM_WIN32(GetLastError()));
        }

        // Projection PSO
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
            psoDesc.VS = {g_ProjectionVS, sizeof(g_ProjectionVS)};
            psoDesc.PS = {g_ProjectionPS, sizeof(g_ProjectionPS)};
            psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
            psoDesc.RasterizerState.DepthClipEnable = TRUE;
            psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
            psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0x0F;
            psoDesc.RTVFormats[0] = (DXGI_FORMAT)swapchainFormat;
            psoDesc.SampleMask = UINT_MAX;
            psoDesc.NumRenderTargets = 1;
            psoDesc.SampleDesc.Count = 1;

            // Root signature: root CBVs (no descriptor heap indirection) for the
            // per-eye constant buffers, plus descriptor tables for samplers and SRVs.
            // Root CBVs embed the GPU virtual address directly in the command buffer,
            // avoiding the descriptor-table double indirection (heap ptr → descriptor
            // → resource). Root SRVs only support buffers, not Texture2D, so the
            // stereo/focus SRVs must stay as descriptor tables.
            // 2 samplers: s0 = peripheral (aniso + LOD bias), s1 = focus (linear).
            CD3DX12_DESCRIPTOR_RANGE samplerRange(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2, 0);
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);

            CD3DX12_ROOT_PARAMETER rootParams[4];
            // Root CBV: b0 (VS) — embeds GPU virtual address directly (2 DWORDs)
            rootParams[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
            // Root CBV: b0 (PS) — embeds GPU virtual address directly (2 DWORDs)
            // Both VS and PS use b0 for their cbuffer; visibility separates them.
            rootParams[1].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
            // Sampler table: s0–s1 (1 DWORD)
            rootParams[2].InitAsDescriptorTable(1, &samplerRange, D3D12_SHADER_VISIBILITY_PIXEL);
            // SRV table: t0–t1 (stereo + focus textures, 1 DWORD)
            rootParams[3].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_PIXEL);

            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 4;
            rsDesc.pParameters = rootParams;
            rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ComPtr<ID3DBlob> signature, error;
            CHECK_HRCMD(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf()));
            CHECK_HRCMD(device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_projectionRootSignature.ReleaseAndGetAddressOf())));

            psoDesc.pRootSignature = m_projectionRootSignature.Get();
            CHECK_HRCMD(device->CreateGraphicsPipelineState(&psoDesc,
                                                     IID_PPV_ARGS(m_projectionPSO.ReleaseAndGetAddressOf())));
        }

        // Sharpening PSO
        {
            CD3DX12_DESCRIPTOR_RANGE cbvRange(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            CD3DX12_ROOT_PARAMETER rootParams[3];
            rootParams[0].InitAsDescriptorTable(1, &cbvRange, D3D12_SHADER_VISIBILITY_ALL);
            rootParams[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
            rootParams[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 3;
            rsDesc.pParameters = rootParams;
            ComPtr<ID3DBlob> signature, error;
            CHECK_HRCMD(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf()));
            CHECK_HRCMD(device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_sharpeningRootSignature.ReleaseAndGetAddressOf())));

            D3D12_COMPUTE_PIPELINE_STATE_DESC csDesc{};
            csDesc.CS = {g_SharpeningCS, sizeof(g_SharpeningCS)};
            csDesc.pRootSignature = m_sharpeningRootSignature.Get();
            csDesc.NodeMask = 0;
            CHECK_HRCMD(device->CreateComputePipelineState(&csDesc,
                                                    IID_PPV_ARGS(m_sharpeningPSO.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Sharpening PSO created\n");
        }

        // Create EASU PSO
        {
            LogDebug("D3D12: Creating EASU PSO...\n");
            CD3DX12_DESCRIPTOR_RANGE cbvRange(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            CD3DX12_ROOT_PARAMETER rootParams[3];
            rootParams[0].InitAsDescriptorTable(1, &cbvRange, D3D12_SHADER_VISIBILITY_ALL);
            rootParams[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
            rootParams[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

            // EASU shader uses a sampler at s0 — declare it as a static sampler.
            D3D12_STATIC_SAMPLER_DESC staticSampler{};
            staticSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.ShaderRegister = 0;
            staticSampler.RegisterSpace = 0;
            staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 3;
            rsDesc.pParameters = rootParams;
            rsDesc.NumStaticSamplers = 1;
            rsDesc.pStaticSamplers = &staticSampler;
            ComPtr<ID3DBlob> signature, error;
            CHECK_HRCMD(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf()));
            CHECK_HRCMD(device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_easuRootSignature.ReleaseAndGetAddressOf())));

            D3D12_COMPUTE_PIPELINE_STATE_DESC csDesc{};
            csDesc.CS = {g_FSR1EASU_CS, sizeof(g_FSR1EASU_CS)};
            csDesc.pRootSignature = m_easuRootSignature.Get();
            csDesc.NodeMask = 0;
            CHECK_HRCMD(device->CreateComputePipelineState(&csDesc,
                                                    IID_PPV_ARGS(m_easuPSO.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: EASU PSO created\n");
        }

        // Create mip-generation CS PSO
        {
            LogDebug("D3D12: Creating mip-gen PSO...\n");
            CD3DX12_DESCRIPTOR_RANGE srvRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
            CD3DX12_DESCRIPTOR_RANGE uavRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
            CD3DX12_ROOT_PARAMETER rootParams[3];
            // Use root constants (8 DWORDs = 32 bytes) instead of a CBV to
            // avoid the per-iteration Map/Unmap race: all 12 mip-gen dispatches
            // are recorded into the same command list before execution, so a
            // shared upload buffer would be overwritten 12 times before the
            // GPU reads it — every dispatch would see the last iteration's
            // constants, causing out-of-bounds UAV writes and GPU device
            // removal. Root constants are baked into the command list at
            // record time, so each dispatch gets its own copy.
            rootParams[0].InitAsConstants(8, 0);  // 8 DWORDs = 32 bytes at b0
            rootParams[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);
            rootParams[2].InitAsDescriptorTable(1, &uavRange, D3D12_SHADER_VISIBILITY_ALL);

            D3D12_STATIC_SAMPLER_DESC staticSampler{};
            staticSampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            staticSampler.ShaderRegister = 0;
            staticSampler.RegisterSpace = 0;
            staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 3;
            rsDesc.pParameters = rootParams;
            rsDesc.NumStaticSamplers = 1;
            rsDesc.pStaticSamplers = &staticSampler;
            ComPtr<ID3DBlob> signature, error;
            CHECK_HRCMD(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, signature.GetAddressOf(), error.GetAddressOf()));
            CHECK_HRCMD(device->CreateRootSignature(0,
                                             signature->GetBufferPointer(),
                                             signature->GetBufferSize(),
                                             IID_PPV_ARGS(m_mipGenRootSignature.ReleaseAndGetAddressOf())));

            D3D12_COMPUTE_PIPELINE_STATE_DESC csDesc{};
            csDesc.CS = {g_MipGenCS, sizeof(g_MipGenCS)};
            csDesc.pRootSignature = m_mipGenRootSignature.Get();
            csDesc.NodeMask = 0;
            CHECK_HRCMD(device->CreateComputePipelineState(&csDesc,
                                                    IID_PPV_ARGS(m_mipGenPSO.ReleaseAndGetAddressOf())));
            LogDebug("D3D12: Mip-gen PSO created\n");
        }

        LogDebug("D3D12: Composition resources initialized successfully!\n");
        m_initialized = true;
        return true;
    }

    // =======================================================================
    // CRTP Hook: Stage 1 - Acquire destination image and resolve source textures
    // Also handles D3D12-specific GPU sync and command list reset
    // =======================================================================
    bool D3D12Compositor::acquireAndResolveImages(
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            void*& outSourceStereo,
            void*& outSourceFocus,
            void*& outDestination) {
        const uint32_t viewIndex = params.viewIndex;

        // The stereo/focus swapchain image caches were already populated by
        // BaseCompositor::compositeView before this hook ran, so there is nothing
        // to repeat here. (The full-FOV cache below IS still needed; the base class does not do it.)

        if (params.useQuadViews) {
            outSourceStereo = stereoState.images[stereoSwapchain.lastReleasedIndex].Get();
#ifdef _DEBUG
            LogDebug("  sourceImage={:p}, lastReleasedIndex={}\n",
                outSourceStereo, stereoSwapchain.lastReleasedIndex);
#endif
        }
        outSourceFocus = focusState.images[focusSwapchain.lastReleasedIndex].Get();
#ifdef _DEBUG
        LogDebug("  sourceFocusImage={:p}, lastReleasedIndex={}\n",
            outSourceFocus, focusSwapchain.lastReleasedIndex);
#endif

        // Acquire/release full FOV swapchain image.
        // Call base class virtual method directly to bypass layer's deferred release quirk,
        // because the full FOV swapchain is not tracked in m_swapchains.
        if (viewIndex == 0) {
            const uint32_t idx = acquireFullFovImage(stereoSwapchain.fullFovSwapchain, stereoState);
            if (idx == UINT32_MAX) {
                return false; // acquisition failed — skip this view
            }
#ifdef _DEBUG
            LogDebug("  Acquired full-FOV swapchain index={}\n", idx);
#endif
        }

        populateSwapchainImagesCache(stereoState, stereoSwapchain.fullFovSwapchain, true);
        outDestination = stereoState.fullFovSwapchainImages[stereoState.acquiredFullFovImageIndex].Get();
#ifdef _DEBUG
        LogDebug("  destinationImage={:p}, acquiredIndex={}\n", outDestination,
                        stereoState.acquiredFullFovImageIndex);
#endif

        // D3D12-specific: GPU sync and command list reset
        waitForPreviousFrame(viewIndex);
        auto& cmdList = resetCommandList(viewIndex);
        m_currentCmdList = cmdList.Get();
        m_currentDestination = static_cast<ID3D12Resource*>(outDestination);
        m_frameIndex = m_currentFrameIndex;
        m_directBoundStereo = nullptr;
        m_directBoundFocus = nullptr;

        return true;
    }

    // =======================================================================
    // GPU sync: wait for previous frame (view 0 only)
    // =======================================================================
    void D3D12Compositor::waitForPreviousFrame(uint32_t viewIndex) {
        // Wait for the previous frame's fence on BOTH views. Each eye has its
        // own command allocator (m_compositionAllocator[viewIndex]); waiting
        // here protects both from being reset while the GPU is still executing
        // the prior frame's work. This replaces the old immediate wait at the
        // end of view 1, which fully serialized CPU and GPU.
        //
        // The fence is signaled once per frame (at the end of view 1), so both
        // views wait on the same value. This yields ~1 frame of CPU/GPU overlap
        // (double-buffering) with the existing single allocator per eye.
        if (m_compositionFence) {
            UINT64 fenceValueToWait = m_fenceValue;
            if (m_compositionFence->GetCompletedValue() < fenceValueToWait) {
                // Reuse the cached fence event (auto-reset) instead of CreateEvent/CloseHandle per frame.
                m_compositionFence->SetEventOnCompletion(fenceValueToWait, m_fenceEvent);
                WaitForSingleObject(m_fenceEvent, INFINITE);
            }
        }
    }

    // =======================================================================
    // CRTP Hook: Bind a source image directly (no flattening needed)
    // =======================================================================
    void D3D12Compositor::BindDirectSource(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, uint32_t format) {
        ID3D12Resource* res = static_cast<ID3D12Resource*>(sourceImage);
        state.flatImage[targetSlot] = res;

        // Track which resource is directly bound, so cleanupAndRelease can restore barriers
        if (targetSlot < xr::StereoView::Count) {
            m_directBoundStereo = res;
        } else {
            m_directBoundFocus = res;
        }

        // Transition RENDER_TARGET -> PIXEL_SHADER_RESOURCE (BarrierBatch skips no-op)
        {
            utils::d3d12::BarrierBatch barriers(m_currentCmdList);
            barriers.Add(res, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // =======================================================================
    // CRTP Hook: Check if flat image needs reallocation
    // =======================================================================
    bool D3D12Compositor::NeedsFlatReallocate(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format) {
        return NeedsReallocate(state.flatImage[targetSlot].Get(), width, height, format);
    }

    // =======================================================================
    // CRTP Hook: Create flat image with correct dimensions
    // =======================================================================
    void D3D12Compositor::CreateFlatImage(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, uint32_t width, uint32_t height, uint32_t format) {
        auto device = m_device.Get();

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = (DXGI_FORMAT)format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                    D3D12_HEAP_FLAG_NONE,
                                                    &desc,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    nullptr,
                                                    IID_PPV_ARGS(state.flatImage[targetSlot].ReleaseAndGetAddressOf())));
    }

    // =======================================================================
    // CRTP Hook: Copy sub-image region from source to flat image
    // =======================================================================
    void D3D12Compositor::CopySubImage(D3D12SwapchainGraphicsState& state, uint32_t targetSlot, void* sourceImage, const XrCompositionLayerProjectionView& view) {
        ID3D12Resource* src = static_cast<ID3D12Resource*>(sourceImage);
        ID3D12Resource* dst = state.flatImage[targetSlot].Get();

        // Barriers: source RT->COPY_SOURCE, dest PSR->COPY_DEST
        {
            utils::d3d12::BarrierBatch barriers(m_currentCmdList);
            barriers.Add(src, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barriers.Add(dst, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }

        // Copy subresource region
        D3D12_TEXTURE_COPY_LOCATION srcLocation{};
        srcLocation.pResource = src;
        srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLocation.SubresourceIndex = view.subImage.imageArrayIndex;

        D3D12_TEXTURE_COPY_LOCATION dstLocation{};
        dstLocation.pResource = dst;
        dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLocation.SubresourceIndex = 0;

        D3D12_BOX srcBox{};
        srcBox.left = view.subImage.imageRect.offset.x;
        srcBox.top = view.subImage.imageRect.offset.y;
        srcBox.front = 0;
        srcBox.right = srcBox.left + view.subImage.imageRect.extent.width;
        srcBox.bottom = srcBox.top + view.subImage.imageRect.extent.height;
        srcBox.back = 1;

        m_currentCmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, &srcBox);

        // Restore: source COPY_SOURCE->RT, dest COPY_DEST->PSR
        {
            utils::d3d12::BarrierBatch barriers(m_currentCmdList);
            barriers.Add(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            barriers.Add(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // =======================================================================
    // CRTP Hook: Stage 2.5 - EASU upscale peripheral texture
    // =======================================================================
    void D3D12Compositor::upscalePeripheralEASU(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& stereoView,
            const SwapchainInfo& stereoSwapchain,
            D3D12SwapchainGraphicsState& stereoState) {
        auto device = m_device.Get();
        const uint32_t viewIndex = params.viewIndex;
        auto cmdList = m_currentCmdList;
        const uint32_t frameIndex = m_frameIndex;

        // EASU output is the full-FOV composite resolution.
        const uint32_t dstW = (uint32_t)params.fullFovResolution.width;
        const uint32_t dstH = (uint32_t)params.fullFovResolution.height;
        const uint32_t srcW = (uint32_t)stereoView.subImage.imageRect.extent.width;
        const uint32_t srcH = (uint32_t)stereoView.subImage.imageRect.extent.height;
        const uint32_t easuFormat = (uint32_t)kEasuFormat;

        // --- (Re)allocate EASU output texture ---
        const uint32_t mipCount = CappedPeripheralMipCount(dstW, dstH, params.peripheralLodBias);
        LogDebug("D3D12 EASU: view={} dstWxH={}x{} mipCount={} cachedMipCount={}\n",
                 viewIndex, dstW, dstH, mipCount, stereoState.easuMipCount[viewIndex]);
        if (NeedsReallocateWithMips(stereoState.easuImage[viewIndex].Get(), dstW, dstH, easuFormat, mipCount)) {
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = dstW;
            desc.Height = dstH;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = mipCount;  // Capped mip chain
            desc.Format = kEasuFormat;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            LogDebug("D3D12 EASU: allocating texture {}x{} mips={} format={}\n",
                     dstW, dstH, mipCount, (uint32_t)kEasuFormat);
            // Allocate with an initial state of PIXEL_SHADER_RESOURCE (not
            // UNORDERED_ACCESS) so that the EASU dispatch's subsequent
            // Transition(PS -> UAV) barrier records an honest BeforeState on
            // the first frame. Lying about BeforeState (claiming PS when the
            // resource is actually in UAV) causes GPU device-removed
            // (DXGI_ERROR_DEVICE_REMOVED / 0x887A0001) on strict runtimes
            // like SteamVR's D3D12. The resource is never read before the
            // EASU write, so a read-only initial state is safe and is the
            // conventional D3D12 pattern for write-before-read resources.
            CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                        nullptr,
                                                        IID_PPV_ARGS(stereoState.easuImage[viewIndex].ReleaseAndGetAddressOf())));
            stereoState.easuSrvUavCached = false;
            stereoState.easuMipCount[viewIndex] = mipCount;

            // --- Pre-create all per-mip SRV/UAV descriptors directly into the
            // 6 GPU-visible heaps (3 frames x 2 eyes) ---
            // D3D12 reads shader-visible descriptor heaps at EXECUTION time, not
            // recording time. The mip-gen loop dispatches once per mip level; if
            // all iterations shared a single SRV/UAV slot, every dispatch would
            // read the LAST iteration's descriptors (reading a sub-mip and
            // writing the wrong mip), leaving easuImage mip 0 unwritten -> black
            // periphery. To fix this, each mip level gets its OWN pre-baked slot
            // (kMipGenSrvBase/UavBase + mip-1), created once here. This also
            // eliminates the per-frame CopyDescriptorsSimple calls entirely.
            //
            // mipCount is capped at 6 (CappedPeripheralMipCount), so mips 1..5
            // need descriptors -> kMaxMipLevels = 5 slots each.
            const auto incSize = m_cbvSrvUavIncSize;
            for (uint32_t f = 0; f < kFrameCount; ++f) {
                for (uint32_t h = 0; h < xr::StereoView::Count; ++h) {
                    ID3D12DescriptorHeap* targetHeap = m_cbvSrvHeap[f][h].Get();

                    for (uint32_t mip = 1; mip < mipCount; ++mip) {
                        // SRV for source mip (mip - 1)
                        D3D12_SHADER_RESOURCE_VIEW_DESC mipSrvDesc{};
                        mipSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        mipSrvDesc.Format = kEasuFormat;
                        mipSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        mipSrvDesc.Texture2D.MipLevels = 1;
                        mipSrvDesc.Texture2D.MostDetailedMip = mip - 1;
                        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandle(
                            targetHeap->GetCPUDescriptorHandleForHeapStart(),
                            DescriptorLayout::kMipGenSrvBase + (mip - 1), incSize);
                        device->CreateShaderResourceView(
                            stereoState.easuImage[viewIndex].Get(), &mipSrvDesc, srvHandle);

                        // UAV for destination mip
                        D3D12_UNORDERED_ACCESS_VIEW_DESC mipUavDesc{};
                        mipUavDesc.Format = kEasuFormat;
                        mipUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                        mipUavDesc.Texture2D.MipSlice = mip;
                        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandle(
                            targetHeap->GetCPUDescriptorHandleForHeapStart(),
                            DescriptorLayout::kMipGenUavBase + (mip - 1), incSize);
                        device->CreateUnorderedAccessView(
                            stereoState.easuImage[viewIndex].Get(), nullptr, &mipUavDesc, uavHandle);
                    }
                }
            }
        }

        // --- Compute EASU constants ---
        // FsrEasuCon() output depends only on the input/output dimensions, which
        // are stable per resolution. Cache it keyed by (srcW, srcH, dstW, dstH)
        // to avoid recomputing the packed con0..con3 every frame.
        // thread_local: compositors run on the app's render thread; no locking.
        struct EasuConstCache {
            uint32_t srcW{0}, srcH{0}, dstW{0}, dstH{0};
            bool valid{false};
            FSR1Constants constants;
        };
        static thread_local EasuConstCache s_easuCache;

        if (!s_easuCache.valid ||
            s_easuCache.srcW != srcW || s_easuCache.srcH != srcH ||
            s_easuCache.dstW != dstW || s_easuCache.dstH != dstH) {
            EASUParamCalculator::Input easuIn{};
            easuIn.inputViewportW = srcW;
            easuIn.inputViewportH = srcH;
            easuIn.inputResourceW = srcW;
            easuIn.inputResourceH = srcH;
            easuIn.outputW = dstW;
            easuIn.outputH = dstH;

            if (!EASUParamCalculator::compute(easuIn, s_easuCache.constants)) {
                LogWarning("D3D12 EASU: invalid inputs, skipping pass\n");
                return;
            }
            s_easuCache.srcW = srcW;
            s_easuCache.srcH = srcH;
            s_easuCache.dstW = dstW;
            s_easuCache.dstH = dstH;
            s_easuCache.valid = true;
        }

        LogDebug("D3D12 EASU: view={} srcWxH={}x{} dstWxH={}x{} srcRes={}\n",
                 viewIndex, srcW, srcH, dstW, dstH, (void*)stereoState.flatImage[viewIndex].Get());

        const FSR1Constants& easuConst = s_easuCache.constants;

        LogDebug("D3D12 EASU: view={} con0={:#x} con1={:#x} con2={:#x} con3={:#x}\n",
                 viewIndex, easuConst.con0[0], easuConst.con1[0], easuConst.con2[0], easuConst.con3[0]);

        // --- Upload constants ---
        // Write constants into the persistently-mapped pointer (no Map/Unmap).
        memcpy(m_easuConstantsMapped[frameIndex], &easuConst, sizeof(easuConst));

        // --- Bind PSO + root signature ---
        cmdList->SetPipelineState(m_easuPSO.Get());
        cmdList->SetComputeRootSignature(m_easuRootSignature.Get());
        cmdList->SetDescriptorHeaps(1, m_cbvSrvHeap[frameIndex][viewIndex].GetAddressOf());

        // --- Cache + copy descriptors (mirrors sharpenFocusView pattern) ---
        ID3D12Resource* srcRes = stereoState.flatImage[viewIndex].Get();
        ID3D12Resource* dstRes = stereoState.easuImage[viewIndex].Get();

        // Diagnostic: log flatImage dimensions. Gated on log level so the
        // GetDesc() driver call is skipped in production.
        if (srcRes && log::GetLogLevel() <= log::LogLevel::Debug) {
            D3D12_RESOURCE_DESC srcDesc = srcRes->GetDesc();
            LogDebug("D3D12 EASU: view={} flatImage dims={}x{} fmt={:#x} mips={}\n",
                     viewIndex, srcDesc.Width, srcDesc.Height, (uint32_t)srcDesc.Format, srcDesc.MipLevels);
        }

        if (!stereoState.easuSrvUavCached ||
            stereoState.cachedEasuSrcAddr != (uint64_t)srcRes ||
            stereoState.cachedEasuDstAddr != (uint64_t)dstRes) {

            if (!stereoState.cpuEasuSrvUavHeap) {
                D3D12_DESCRIPTOR_HEAP_DESC cpuHeapDesc{};
                cpuHeapDesc.NumDescriptors = 2;
                cpuHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                cpuHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                CHECK_HRCMD(device->CreateDescriptorHeap(&cpuHeapDesc,
                    IID_PPV_ARGS(stereoState.cpuEasuSrvUavHeap.ReleaseAndGetAddressOf())));
            }

            const auto incSize = m_cbvSrvUavIncSize;
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuSrv(stereoState.cpuEasuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, incSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuUav(stereoState.cpuEasuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, incSize);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = (DXGI_FORMAT)stereoSwapchain.createInfo.format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(srcRes, &srvDesc, cpuSrv);
            LogDebug("D3D12 EASU: view={} SRV res={} fmt={:#x} mips=1\n",
                     viewIndex, (void*)srcRes, (uint32_t)srvDesc.Format);

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = kEasuFormat;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(dstRes, nullptr, &uavDesc, cpuUav);
            LogDebug("D3D12 EASU: view={} UAV res={} fmt={:#x}\n",
                     viewIndex, (void*)dstRes, (uint32_t)uavDesc.Format);

            stereoState.cachedEasuSrcAddr = (uint64_t)srcRes;
            stereoState.cachedEasuDstAddr = (uint64_t)dstRes;
            stereoState.easuSrvUavCached = true;
            // Force re-copy into all 6 GPU heaps.
            stereoState.easuGpuCopyMask = 0;
        }

        // Only copy descriptors into this frame's GPU heap if not already done.
        const uint32_t easuCopyBit = gpuCopyBit(frameIndex, viewIndex);
        if ((stereoState.easuGpuCopyMask & easuCopyBit) == 0) {
            const auto descInc = m_cbvSrvUavIncSize;
            device->CopyDescriptorsSimple(1,
                DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kEasuSrv, descInc),
                CD3DX12_CPU_DESCRIPTOR_HANDLE(stereoState.cpuEasuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, descInc),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            device->CopyDescriptorsSimple(1,
                DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kEasuUav, descInc),
                CD3DX12_CPU_DESCRIPTOR_HANDLE(stereoState.cpuEasuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, descInc),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            stereoState.easuGpuCopyMask |= easuCopyBit;
        }
        const auto descInc = m_cbvSrvUavIncSize;

        cmdList->SetComputeRootDescriptorTable(0,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kEasuCbv, descInc));
        cmdList->SetComputeRootDescriptorTable(1,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kEasuSrv, descInc));
        cmdList->SetComputeRootDescriptorTable(2,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kEasuUav, descInc));

        // --- Barriers: flat image PS→CS, easuImage mip 0 PS→UAV ---
        // NOTE: Only mip 0 is written by EASU. Using ALL_SUBRESOURCES here
        // would claim all 13 mips transition to UAV, but only mip 0 is
        // actually written. When the subsequent mip-gen loop issues
        // subresource-specific barriers claiming mips 1-12 are in PSR, a
        // strict D3D12 runtime may detect a BeforeState mismatch (the
        // runtime believes they're in UAV from the whole-resource barrier).
        // Using subresource-specific barriers for only mip 0 avoids this.
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(stereoState.flatImage[viewIndex].Get(),
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barriers.Add(stereoState.easuImage[viewIndex].Get(),
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         0);  // only mip 0
        }

        // --- Dispatch: 16x16 tiles, 64 threads per group ---
        cmdList->Dispatch((dstW + 15) / 16, (dstH + 15) / 16, 1);

        // --- Restore barriers: mip 0 UAV→PSR ---
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(stereoState.flatImage[viewIndex].Get(),
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barriers.Add(stereoState.easuImage[viewIndex].Get(),
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         0);  // only mip 0
        }

        // Diagnostic: check device health after EASU pass. Gated on log level
        // so the driver call is skipped in production.
        if (log::GetLogLevel() <= log::LogLevel::Debug) {
            HRESULT hr = device->GetDeviceRemovedReason();
            LogDebug("D3D12 EASU: view={} post-restore DeviceRemovedReason={:#x}\n",
                     viewIndex, (uint32_t)hr);
        }

        // --- Mip chain: generate mip chain 1..N from mip 0 ---
        if (!params.skipMipGen) {
            // Recompute the capped mip count rather than trusting the cached
            // easuMipCount, which may have been forced to 1 by a previous
            // skipMipGen frame. This lets the toggle resume mip-gen immediately
            // without waiting for a resolution/format realloc.
            const uint32_t actualMipCount = CappedPeripheralMipCount(dstW, dstH, params.peripheralLodBias);
            stereoState.easuMipCount[viewIndex] = actualMipCount;
            LogDebug("D3D12 MipGen: view={} actualMipCount={} easuRes={}\n",
                     viewIndex, actualMipCount, (void*)stereoState.easuImage[viewIndex].Get());
            if (actualMipCount > 1) {
                cmdList->SetPipelineState(m_mipGenPSO.Get());
                cmdList->SetComputeRootSignature(m_mipGenRootSignature.Get());
                cmdList->SetDescriptorHeaps(1, m_cbvSrvHeap[frameIndex][viewIndex].GetAddressOf());

                ID3D12Resource* easuRes = stereoState.easuImage[viewIndex].Get();

                // Persistent barrier batch across the whole loop. Post-dispatch
                // barriers of iteration N (dst mip → PSR, src mip → PSR) are
                // deferred and merged with the pre-dispatch barriers of
                // iteration N+1 into a single ResourceBarrier call. D3D12
                // applies barriers in order within one call, so this is safe and
                // reduces the number of driver calls from 2 per mip to 1.
                utils::d3d12::BarrierBatch pendingBarriers(cmdList);

                for (uint32_t mip = 1; mip < actualMipCount; ++mip) {
                    const uint32_t srcWm = std::max(dstW >> (mip - 1), 1u);
                    const uint32_t srcHm = std::max(dstH >> (mip - 1), 1u);
                    const uint32_t dstWm = std::max(dstW >> mip, 1u);
                    const uint32_t dstHm = std::max(dstH >> mip, 1u);

                    // Pack mip-gen constants into root constants (8 DWORDs = 32 bytes).
                    // Root constants are baked into the command list at record time,
                    // avoiding the per-iteration Map/Unmap race that occurs when
                    // all 12 dispatches share a single upload buffer (the GPU would
                    // see only the last iteration's data, causing out-of-bounds UAV
                    // writes and device removal).
                    uint32_t mipConsts[8];
                    mipConsts[0] = mip - 1;        // srcMipLevel
                    mipConsts[1] = mip;            // dstMipLevel
                    mipConsts[2] = srcWm;          // srcWidth
                    mipConsts[3] = srcHm;          // srcHeight
                    float invSrcW = 1.0f / srcWm;  // invSrcSize.x
                    float invSrcH = 1.0f / srcHm;  // invSrcSize.y
                    memcpy(&mipConsts[4], &invSrcW, sizeof(float));
                    memcpy(&mipConsts[5], &invSrcH, sizeof(float));
                    mipConsts[6] = 0;              // pad
                    mipConsts[7] = 0;              // pad

                    // The per-mip SRV/UAV descriptors were pre-baked into the GPU
                    // heaps at allocation time (see upscalePeripheralEASU). Each
                    // mip level has its OWN slot (kMipGenSrvBase/UavBase + mip-1),
                    // so the GPU reads the correct descriptor at execution time.
                    // No per-frame CopyDescriptorsSimple is needed.

                    // Pre-dispatch barriers: src mip → SRV, dst mip → UAV.
                    // These merge with the previous iteration's post-dispatch
                    // barriers (deferred below) into a single ResourceBarrier call.
                    pendingBarriers.Add(easuRes,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        mip - 1);
                    pendingBarriers.Add(easuRes,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        mip);
                    // Flush so the barriers are issued before this dispatch.
                    pendingBarriers.Flush();

                    // Bind root constants and the pre-baked per-mip descriptor
                    // tables by offset.
                    const auto descInc = m_cbvSrvUavIncSize;
                    cmdList->SetComputeRoot32BitConstants(0, 8, mipConsts, 0);
                    cmdList->SetComputeRootDescriptorTable(1,
                        DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(),
                                                    DescriptorLayout::kMipGenSrvBase + (mip - 1), descInc));
                    cmdList->SetComputeRootDescriptorTable(2,
                        DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(),
                                                    DescriptorLayout::kMipGenUavBase + (mip - 1), descInc));

                    // Dispatch: 8x8 threads per group
                    cmdList->Dispatch((dstWm + 7) / 8, (dstHm + 7) / 8, 1);

                    // Post-dispatch barriers: dst mip back to PSR for next
                    // iteration / final projection. Also restore the src mip to
                    // PSR: it was only needed as an SRV for THIS iteration's
                    // dispatch. Leaving src mips in NON_PIXEL_SHADER_RESOURCE
                    // causes a state mismatch when the projection pass samples
                    // the full mip chain via a PS SRV, which the D3D12 runtime
                    // rejects at cmdList->Close() validation (device-removed
                    // 0x887A0001). Restoring both mips here ensures every
                    // subresource is back in PSR after the loop, so no final
                    // whole-resource barrier is needed.
                    //
                    // These are deferred: they merge with the next iteration's
                    // pre-dispatch barriers (or are flushed by the batch
                    // destructor after the loop) into a single call.
                    pendingBarriers.Add(easuRes,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        mip);
                    pendingBarriers.Add(easuRes,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        mip - 1);
                }
                // Final flush: issues the last iteration's post-dispatch barriers
                // (the batch destructor would also do this, but be explicit).
                pendingBarriers.Flush();
                // Single summary log replaces the per-mip LogDebug calls that
                // previously fired 3x per mip level per frame.
                LogDebug("D3D12 MipGen: view={} completed {} mips\n", viewIndex, actualMipCount - 1);
                // Diagnostic: check device health after mip-gen loop. Gated on
                // log level so the driver call is skipped in production.
                if (log::GetLogLevel() <= log::LogLevel::Debug) {
                    HRESULT hr = device->GetDeviceRemovedReason();
                    LogDebug("D3D12 MipGen: view={} post-loop DeviceRemovedReason={:#x}\n",
                             viewIndex, (uint32_t)hr);
                }
            }
        } else {
            // skipMipGen: bind only mip 0 in the projection SRV. The texture
            // still has the full mip chain allocated; we just skip generating
            // mips 1..N. easuMipCount is restored to the full count on the next
            // non-skip frame (recomputed above).
            stereoState.easuMipCount[viewIndex] = 1;
        }
    }

    // =======================================================================
    // CRTP Hook: Stage 3 - Run the CAS sharpening compute pass
    // =======================================================================
    void D3D12Compositor::sharpenFocusView(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& focusState) {
        auto device = m_device.Get();
        const uint32_t viewIndex = params.viewIndex;
        auto cmdList = m_currentCmdList;
        const uint32_t frameIndex = m_frameIndex;

        const uint32_t sharpWidth = (uint32_t)focusView.subImage.imageRect.extent.width;
        const uint32_t sharpHeight = (uint32_t)focusView.subImage.imageRect.extent.height;
        const uint32_t sharpFormat = (uint32_t)kSharpenedFormat;

        if (NeedsReallocate(focusState.sharpenedImage[viewIndex].Get(),
                             sharpWidth, sharpHeight, sharpFormat)) {
            D3D12_HEAP_PROPERTIES heapProps{};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            heapProps.CreationNodeMask = heapProps.VisibleNodeMask = 1;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Alignment = 0;
            desc.Width = focusView.subImage.imageRect.extent.width;
            desc.Height = focusView.subImage.imageRect.extent.height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = kSharpenedFormat;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            CHECK_HRCMD(device->CreateCommittedResource(&heapProps,
                                                        D3D12_HEAP_FLAG_NONE,
                                                        &desc,
                                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        nullptr,
                                                        IID_PPV_ARGS(focusState.sharpenedImage[viewIndex].ReleaseAndGetAddressOf())));
        }

        // Update constant buffer using SharpeningPass (per-frame resource).
        SharpeningCSConstants sharpening{};
        SharpeningPass{}.PrepareConstants(sharpening, params.sharpenFocusView,
                                          focusView.subImage.imageRect.extent.width,
                                          focusView.subImage.imageRect.extent.height);

        // Write constants into the persistently-mapped pointer (no Map/Unmap).
        memcpy(m_sharpeningConstantsMapped[frameIndex], &sharpening, sizeof(sharpening));

        cmdList->SetPipelineState(m_sharpeningPSO.Get());
        cmdList->SetComputeRootSignature(m_sharpeningRootSignature.Get());
        cmdList->SetDescriptorHeaps(1, m_cbvSrvHeap[frameIndex][viewIndex].GetAddressOf());

        // --- DESCRIPTOR CACHING LOGIC ---
        // Cache SRV/UAV descriptors in a per-state CPU heap to avoid per-frame
        // CreateShaderResourceView / CreateUnorderedAccessView overhead.
        ID3D12Resource* flatFocusRes = focusState.flatImage[xr::StereoView::Count + viewIndex].Get();
        ID3D12Resource* sharpenedRes = focusState.sharpenedImage[viewIndex].Get();

        // Check if we need to (re)create the cached descriptors
        if (!focusState.srvUavCached ||
            focusState.cachedFlatFocusAddr != (uint64_t)flatFocusRes ||
            focusState.cachedSharpenedAddr != (uint64_t)sharpenedRes) {

            if (!focusState.cpuSrvUavHeap) {
                D3D12_DESCRIPTOR_HEAP_DESC cpuHeapDesc{};
                cpuHeapDesc.NumDescriptors = 2; // 1 SRV + 1 UAV
                cpuHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                cpuHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                CHECK_HRCMD(device->CreateDescriptorHeap(&cpuHeapDesc,
                    IID_PPV_ARGS(focusState.cpuSrvUavHeap.ReleaseAndGetAddressOf())));
            }

            const auto cpuIncSize = m_cbvSrvUavIncSize;
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuSrvHandle(focusState.cpuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, cpuIncSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuUavHandle(focusState.cpuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, cpuIncSize);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = (DXGI_FORMAT)focusSwapchain.createInfo.format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(flatFocusRes, &srvDesc, cpuSrvHandle);

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = kSharpenedFormat;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(sharpenedRes, nullptr, &uavDesc, cpuUavHandle);

            focusState.cachedFlatFocusAddr = (uint64_t)flatFocusRes;
            focusState.cachedSharpenedAddr = (uint64_t)sharpenedRes;
            focusState.srvUavCached = true;
            // Force re-copy into all 6 GPU heaps.
            focusState.sharpenGpuCopyMask = 0;
        }

        // Only copy descriptors into this frame's GPU heap if not already done.
        const uint32_t sharpenCopyBit = gpuCopyBit(frameIndex, viewIndex);
        if ((focusState.sharpenGpuCopyMask & sharpenCopyBit) == 0) {
            const auto gpuIncSize = m_cbvSrvUavIncSize;

            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuSrcSrv(focusState.cpuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, gpuIncSize);
            CD3DX12_CPU_DESCRIPTOR_HANDLE cpuSrcUav(focusState.cpuSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, gpuIncSize);

            device->CopyDescriptorsSimple(1,
                DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenSrv, gpuIncSize),
                cpuSrcSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            device->CopyDescriptorsSimple(1,
                DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenUav, gpuIncSize),
                cpuSrcUav, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            focusState.sharpenGpuCopyMask |= sharpenCopyBit;
        }

        // Bind root descriptors using DescriptorLayout.
        const auto descInc = m_cbvSrvUavIncSize;
        cmdList->SetComputeRootDescriptorTable(0,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenCbv, descInc));
        cmdList->SetComputeRootDescriptorTable(1,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenSrv, descInc));
        cmdList->SetComputeRootDescriptorTable(2,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kSharpenUav, descInc));

        // Resource barriers for compute shader access.
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(focusState.flatImage[xr::StereoView::Count + viewIndex].Get(),
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barriers.Add(focusState.sharpenedImage[viewIndex].Get(),
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        SharpeningPass sharpeningPass;
        sharpeningPass(params, focusView, focusSwapchain);
        cmdList->Dispatch(sharpeningPass.dispatchX, sharpeningPass.dispatchY, 1);

        // Restore resource barriers after compute.
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(focusState.flatImage[xr::StereoView::Count + viewIndex].Get(),
                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            barriers.Add(focusState.sharpenedImage[viewIndex].Get(),
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    // Rebuild the peripheral sampler (heap index 0) only when the config
    // changes. The LOD bias + anisotropy only help when the peripheral texture
    // has a mip chain (EASU on, mip-gen enabled); otherwise fall back to linear.
    void D3D12Compositor::updatePeripheralSampler(const CompositorParams& params) {
        const bool needsMips = params.useFSR1EASU && !params.skipMipGen;

        if (!m_peripheralSamplerDirty &&
            m_cachedLodBias == params.peripheralLodBias &&
            m_cachedAnisotropy == params.peripheralAnisotropy) {
            return;
        }

        D3D12_SAMPLER_DESC desc{};
        desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        desc.BorderColor[0] = 0;
        desc.BorderColor[1] = 0;
        desc.BorderColor[2] = 0;
        desc.BorderColor[3] = 0;
        desc.MinLOD = 0;
        desc.MaxLOD = D3D12_FLOAT32_MAX;

        if (needsMips) {
            desc.Filter = D3D12_FILTER_ANISOTROPIC;
            desc.MaxAnisotropy = std::clamp(params.peripheralAnisotropy, 1u, 16u);
            desc.MipLODBias = params.peripheralLodBias;
            // Cap MaxLOD to the maximum possible capped mip chain (6 levels
            // for the max clamped bias of 2.0). This prevents sampling beyond
            // the allocated levels regardless of the actual texture size.
            desc.MaxLOD = 5.0f;  // CappedPeripheralMipCount returns at most 6
        } else {
            desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            desc.MaxAnisotropy = 1;
            desc.MipLODBias = 0.0f;
        }

        auto device = m_device.Get();
        const auto samplerInc = m_samplerIncSize;
        CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(m_samplerHeap->GetCPUDescriptorHandleForHeapStart(), 0, samplerInc);
        device->CreateSampler(&desc, cpuHandle);

        m_cachedLodBias = params.peripheralLodBias;
        m_cachedAnisotropy = params.peripheralAnisotropy;
        m_peripheralSamplerDirty = false;
    }

    // =======================================================================
    // CRTP Hook: Stage 4 - Render the projection pass
    // =======================================================================
    void D3D12Compositor::renderProjection(
            const CompositorParams& params,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& stereoSwapchain,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            void* destination) {
        auto device = m_device.Get();
        const uint32_t viewIndex = params.viewIndex;
        auto cmdList = m_currentCmdList;
        const uint32_t frameIndex = m_frameIndex;
        ID3D12Resource* destRes = static_cast<ID3D12Resource*>(destination);

        // Update constant buffers using typed writers
        ProjectionVSConstants projection{};
        // [DEBUG-QVF] Log the FOVs feeding ComposeProjectionMatrix (which throws
        // "Invalid projection specification" when any angle is >= +/-90 deg or a
        // pair is degenerate). Remove this block once the black-render issue is fixed.
        {
            const XrFovf& ce = params.cachedEyeFov;
            const XrFovf& fv = focusView.fov;
            LogDebug("D3D12 renderProjection view={} cachedEyeFov=(L={:.4f} R={:.4f} U={:.4f} D={:.4f}) "
                     "focusViewFov=(L={:.4f} R={:.4f} U={:.4f} D={:.4f})\n",
                     viewIndex,
                     ce.angleLeft, ce.angleRight, ce.angleUp, ce.angleDown,
                     fv.angleLeft, fv.angleRight, fv.angleUp, fv.angleDown);
            // [DEBUG-QVF] Flag the exact angle that would trip ComposeProjectionMatrix's
            // ValidateFovAngle (|angle| >= 90 deg) or its degenerate-pair check.
            const float pid2 = DirectX::XM_PIDIV2;
            auto bad = [&](const char* tag, const XrFovf& f) {
                if (f.angleLeft >= pid2 || f.angleLeft <= -pid2 ||
                    f.angleRight >= pid2 || f.angleRight <= -pid2 ||
                    f.angleUp >= pid2 || f.angleUp <= -pid2 ||
                    f.angleDown >= pid2 || f.angleDown <= -pid2) {
                    LogDebug("D3D12 renderProjection view={} [{}] angle out of +/-90 deg range!\n", viewIndex, tag);
                }
                if (fabsf(f.angleLeft - f.angleRight) < std::numeric_limits<float>::epsilon() ||
                    fabsf(f.angleUp - f.angleDown) < std::numeric_limits<float>::epsilon()) {
                    LogDebug("D3D12 renderProjection view={} [{}] degenerate FOV pair!\n", viewIndex, tag);
                }
            };
            bad("cachedEyeFov", ce);
            bad("focusViewFov", fv);
        }
        ComputeProjectionConstants(projection, params.cachedEyeFov, focusView.fov);
        projection.stereoSubRect = {0, 0, 0, 0};
        projection.focusSubRect = {0, 0, 0, 0};
        projection.stereoSwapchainSize = {0, 0};
        projection.focusSwapchainSize = {0, 0};

        ProjectionPSConstants drawing{};
        ComputePixelShaderConstants(drawing, params);
        drawing.stereoSubRect = {0, 0, 0, 0};
        drawing.focusSubRect = {0, 0, 0, 0};
        drawing.stereoSwapchainSize = {0, 0};
        drawing.focusSwapchainSize = {0, 0};
        drawing.useDirectStereoSampling = false;
        drawing.useDirectFocusSampling = false;

        m_vsConstantWriters[frameIndex].writeVS(viewIndex, projection);
        m_psConstantWriters[frameIndex].writePS(viewIndex, drawing);

        // Transition destination to render target.
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(destRes, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        const auto rtvHandle = getOrCreateRTV(stereoState, destRes, viewIndex,
                                              (DXGI_FORMAT)stereoSwapchain.createInfo.format);
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = 0;
        viewport.TopLeftY = 0;
        viewport.Width = (float)params.fullFovResolution.width;
        viewport.Height = (float)params.fullFovResolution.height;
        viewport.MinDepth = 0.f;
        viewport.MaxDepth = 1.f;
        cmdList->RSSetViewports(1, &viewport);

        D3D12_RECT scissor = {0, 0, (LONG)params.fullFovResolution.width, (LONG)params.fullFovResolution.height};
        cmdList->RSSetScissorRects(1, &scissor);

        // Diagnostic: log destination/RTV/viewport state before draw. Gated on
        // log level so the GetDesc() driver call is skipped in production.
        if (log::GetLogLevel() <= log::LogLevel::Debug) {
            D3D12_RESOURCE_DESC destDesc = destRes->GetDesc();
            LogDebug("D3D12 Projection: view={} destRes={} dims={}x{} fmt={:#x} mips={}\n",
                     viewIndex, (void*)destRes, destDesc.Width, destDesc.Height,
                     (uint32_t)destDesc.Format, destDesc.MipLevels);
            LogDebug("D3D12 Projection: view={} rtvHandle ptr={:#x} viewport={}x{} scissor={}x{}\n",
                     viewIndex, (uint64_t)rtvHandle.ptr,
                     (uint32_t)viewport.Width, (uint32_t)viewport.Height,
                     (uint32_t)(scissor.right - scissor.left), (uint32_t)(scissor.bottom - scissor.top));
        }

        // Bind SRVs for source textures.
        bindProjectionSRVs(params, stereoSwapchain, focusView, focusSwapchain,
                           stereoState, focusState, frameIndex);

        // Rebuild the peripheral sampler if the config changed, then bind.
        updatePeripheralSampler(params);

        // Bind PSO, root signature, and descriptor heaps, then draw.
        cmdList->SetPipelineState(m_projectionPSO.Get());
        cmdList->SetGraphicsRootSignature(m_projectionRootSignature.Get());
        ID3D12DescriptorHeap* heaps[2] = {m_cbvSrvHeap[frameIndex][viewIndex].Get(), m_samplerHeap.Get()};
        cmdList->SetDescriptorHeaps(2, heaps);
        if (log::GetLogLevel() <= log::LogLevel::Verbose) {
            LogVerbose("D3D12 Projection: view={} cbvSrvHeap={} samplerHeap={}\n",
                     viewIndex, (void*)heaps[0], (void*)heaps[1]);
        }

        // Root CBVs: bind the GPU virtual address of the per-eye CBV slot
        // directly into the command buffer (no descriptor heap indirection).
        // The upload heaps are persistently mapped; the address is stable.
        // Per-eye offset is viewIndex * 256 (see ConstantBufferWriter).
        const D3D12_GPU_VIRTUAL_ADDRESS vsCbvAddr =
            m_projectionVSConstants[frameIndex]->GetGPUVirtualAddress() + viewIndex * 256;
        const D3D12_GPU_VIRTUAL_ADDRESS psCbvAddr =
            m_projectionPSConstants[frameIndex]->GetGPUVirtualAddress() + viewIndex * 256;
        cmdList->SetGraphicsRootConstantBufferView(0, vsCbvAddr);
        cmdList->SetGraphicsRootConstantBufferView(1, psCbvAddr);

        // Sampler table (slot 2) and SRV table (slot 3) stay as descriptor tables.
        // Root SRVs only support buffers, not Texture2D, so the stereo/focus
        // textures must be bound via the descriptor heap.
        cmdList->SetGraphicsRootDescriptorTable(2, m_samplerHeap->GetGPUDescriptorHandleForHeapStart());
        cmdList->SetGraphicsRootDescriptorTable(3,
            DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kStereoSrv, m_cbvSrvUavIncSize));

        // Diagnostic: log the GPU descriptor handles for the SRV table slot.
        // Demoted to Verbose: raw descriptor addresses are only useful when
        // actively debugging descriptor corruption.
        if (log::GetLogLevel() <= log::LogLevel::Verbose) {
            auto h3 = DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kStereoSrv, m_cbvSrvUavIncSize);
            auto hFocus = DescriptorLayout::GpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kFocusSrv, m_cbvSrvUavIncSize);
            LogVerbose("D3D12 Projection: view={} vsCbv={:#x} psCbv={:#x} stereoSrv={:#x} focusSrv={:#x}\n",
                     viewIndex, (uint64_t)vsCbvAddr, (uint64_t)psCbvAddr, (uint64_t)h3.ptr, (uint64_t)hFocus.ptr);
        }

        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmdList->DrawInstanced(3, 1, 0, 0);
        // Diagnostic: check device health after draw. Gated on log level so the
        // driver call is skipped in production.
        if (log::GetLogLevel() <= log::LogLevel::Debug) {
            HRESULT hr = device->GetDeviceRemovedReason();
            LogDebug("D3D12 Projection: view={} post-draw DeviceRemovedReason={:#x}\n",
                     viewIndex, (uint32_t)hr);
        }
    }

    // =======================================================================
    // Helper: bind SRVs for the projection pass
    // =======================================================================
    void D3D12Compositor::bindProjectionSRVs(
            const CompositorParams& params,
            const SwapchainInfo& stereoSwapchain,
            const XrCompositionLayerProjectionView& focusView,
            const SwapchainInfo& focusSwapchain,
            D3D12SwapchainGraphicsState& stereoState,
            D3D12SwapchainGraphicsState& focusState,
            uint32_t frameIndex) {
        auto device = m_device.Get();
        const uint32_t viewIndex = params.viewIndex;

        const auto incrementSize = m_cbvSrvUavIncSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        // IMPORTANT: CreateShaderResourceView may not be called directly on a
        // shader-visible (GPU-visible) descriptor heap — doing so is illegal per
        // the D3D12 spec and causes device removal (DXGI_ERROR_DEVICE_REMOVED /
        // DXGI_ERROR_INVALID_CALL). We stage the SRVs on a per-view non-shader-visible
        // CPU heap (ProjectionSrvCache::cpuHeap) and then CopyDescriptorsSimple them
        // into the GPU-visible m_cbvSrvHeap. This mirrors the pattern used in
        // sharpenFocusView() and upscalePeripheralEASU().

        // Resolve which textures/formats to bind for this view.
        ID3D12Resource* stereoTex = nullptr;
        DXGI_FORMAT stereoFormat = (DXGI_FORMAT)stereoSwapchain.createInfo.format;
        uint32_t stereoMips = 1;
        if (params.useFSR1EASU && stereoState.easuImage[viewIndex]) {
            // EASU-upscaled peripheral texture
            // Mip chain: bind the full mip chain for proper minification filtering
            stereoTex = stereoState.easuImage[viewIndex].Get();
            stereoFormat = kEasuFormat;
            stereoMips = stereoState.easuMipCount[viewIndex];
        } else if (params.useQuadViews && stereoState.flatImage[viewIndex]) {
            stereoTex = stereoState.flatImage[viewIndex].Get();
        } else {
            stereoTex = focusState.flatImage[xr::StereoView::Count + viewIndex].Get();
        }

        ID3D12Resource* focusTex = nullptr;
        DXGI_FORMAT focusFormat = (DXGI_FORMAT)focusSwapchain.createInfo.format;
        if (params.sharpenFocusView && focusState.sharpenedImage[viewIndex]) {
            focusTex = focusState.sharpenedImage[viewIndex].Get();
            focusFormat = kSharpenedFormat;
        } else if (params.useQuadViews) {
            focusTex = focusState.flatImage[xr::StereoView::Count + viewIndex].Get();
        } else {
            focusTex = m_blankTexture.Get();
            focusFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        }

        // Per-view cache: rebuild the CPU descriptors only when any of the
        // bound resources/formats change. Otherwise reuse them and just copy
        // into the GPU-visible heap for this (frameIndex, viewIndex) slot.
        auto& cache = stereoState.projectionSrvCache[viewIndex];
        if (!cache.cached ||
            cache.cachedStereoAddr != (uint64_t)stereoTex ||
            cache.cachedFocusAddr != (uint64_t)focusTex ||
            cache.cachedStereoMipCount != stereoMips ||
            cache.cachedStereoFormat != stereoFormat ||
            cache.cachedFocusFormat != focusFormat) {
            if (!cache.cpuHeap) {
                D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
                heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                heapDesc.NumDescriptors = 2;  // slot 0: stereo SRV, slot 1: focus SRV
                heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
                device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&cache.cpuHeap));
            }
            const CD3DX12_CPU_DESCRIPTOR_HANDLE stereoCpuStage(
                cache.cpuHeap->GetCPUDescriptorHandleForHeapStart(), 0, incrementSize);
            srvDesc.Format = stereoFormat;
            srvDesc.Texture2D.MipLevels = stereoMips;
            srvDesc.Texture2D.MostDetailedMip = 0;
            device->CreateShaderResourceView(stereoTex, &srvDesc, stereoCpuStage);
            LogDebug("D3D12 bindProjectionSRVs: view={} stereo SRV res={} fmt={} mips={}\n",
                     viewIndex, (void*)stereoTex, (uint32_t)stereoFormat, stereoMips);

            // CRITICAL: Reset srvDesc fields that may have been modified by the
            // EASU stereo path above (MipLevels=13, MostDetailedMip=0). The focus
            // flatImage has only 1 mip level; an SRV claiming 13 mips on a 1-mip
            // resource causes a GPU fault (device removed 0x887A0001) when the
            // projection pixel shader samples it.
            const CD3DX12_CPU_DESCRIPTOR_HANDLE focusCpuStage(
                cache.cpuHeap->GetCPUDescriptorHandleForHeapStart(), 1, incrementSize);
            srvDesc.Texture2D.MipLevels = 1;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Format = focusFormat;
            device->CreateShaderResourceView(focusTex, &srvDesc, focusCpuStage);
            LogDebug("D3D12 bindProjectionSRVs: view={} focus SRV res={} fmt={}\n",
                     viewIndex, (void*)focusTex, (uint32_t)focusFormat);

            cache.cachedStereoAddr = (uint64_t)stereoTex;
            cache.cachedFocusAddr = (uint64_t)focusTex;
            cache.cachedStereoMipCount = stereoMips;
            cache.cachedStereoFormat = stereoFormat;
            cache.cachedFocusFormat = focusFormat;
            cache.cached = true;
            cache.gpuCopyMask = 0;  // Force re-copy into all GPU heaps
        }

        const uint32_t copyBit = gpuCopyBit(frameIndex, viewIndex);
        if ((cache.gpuCopyMask & copyBit) == 0) {
            const CD3DX12_CPU_DESCRIPTOR_HANDLE stereoCpuStage(
                cache.cpuHeap->GetCPUDescriptorHandleForHeapStart(), 0, incrementSize);
            device->CopyDescriptorsSimple(1,
                DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kStereoSrv, incrementSize),
                stereoCpuStage,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            const CD3DX12_CPU_DESCRIPTOR_HANDLE focusCpuStage(
                cache.cpuHeap->GetCPUDescriptorHandleForHeapStart(), 1, incrementSize);
            device->CopyDescriptorsSimple(1,
                DescriptorLayout::CpuHandle(m_cbvSrvHeap[frameIndex][viewIndex].Get(), DescriptorLayout::kFocusSrv, incrementSize),
                focusCpuStage,
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            cache.gpuCopyMask |= copyBit;
        }
    }

    // =======================================================================
    // Helper: get or create cached RTV handle
    // =======================================================================
    D3D12_CPU_DESCRIPTOR_HANDLE D3D12Compositor::getOrCreateRTV(
            D3D12SwapchainGraphicsState& state,
            ID3D12Resource* destination,
            uint32_t arraySlice,
            DXGI_FORMAT format) {
        auto device = m_device.Get();

        const RtvCacheKey key{destination, arraySlice};
        auto it = state.rtvCache.find(key);
        if (it != state.rtvCache.end()) {
            return it->second;
        }

        // Guard against heap overflow (kRtvHeapSize descriptors).
        if (state.rtvCache.size() >= kRtvHeapSize) {
            LogWarning("D3D12: RTV heap exhausted ({}/{}). Clearing cache.\n",
                       state.rtvCache.size(), kRtvHeapSize);
            state.rtvCache.clear();
        }

        const auto rtvInc = m_rtvIncSize;
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
        rtvHandle.Offset((INT)state.rtvCache.size() * (INT)rtvInc);

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = format;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.ArraySize = 1;
        rtvDesc.Texture2DArray.FirstArraySlice = arraySlice;
        device->CreateRenderTargetView(destination, &rtvDesc, rtvHandle);

        state.rtvCache[key] = rtvHandle;
        return rtvHandle;
    }

    // =======================================================================
    // CRTP Hook: Stage 5 - Cleanup, restore barriers, submit, and sync
    //
    // CRITICAL: ExecuteCommandLists expects an array of pointers
    // (ID3D12CommandList* const*). Passing the command list pointer directly
    // via a reinterpret_cast causes the runtime to read the object's vtable
    // as memory addresses, leading to immediate device removal.
    // =======================================================================
    void D3D12Compositor::cleanupAndRelease(
            const CompositorParams& params,
            D3D12SwapchainGraphicsState& stereoState) {
        auto device = m_device.Get();
        auto queue = m_queue.Get();
        const uint32_t viewIndex = params.viewIndex;
        auto cmdList = m_currentCmdList;
        auto destination = m_currentDestination;

        // Restore directly bound images to RENDER_TARGET before closing
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            if (m_directBoundStereo)
                barriers.Add(m_directBoundStereo, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (m_directBoundFocus)
                barriers.Add(m_directBoundFocus, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }

        // Transition destination to present.
        {
            utils::d3d12::BarrierBatch barriers(cmdList);
            barriers.Add(destination, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }

        // Diagnostic: check device health before Close(). Gated on log level so
        // the driver call is skipped in production.
        if (log::GetLogLevel() <= log::LogLevel::Debug) {
            HRESULT hr = device->GetDeviceRemovedReason();
            LogDebug("D3D12 cleanup: view={} pre-Close DeviceRemovedReason={:#x}\n",
                     viewIndex, (uint32_t)hr);
        }

        try {
            CHECK_HRCMD(cmdList->Close());
        } catch (const std::exception& e) {
            LogError("D3D12 cleanup: view={} Close() threw exception: {}\n", viewIndex, e.what());
            throw;
        } catch (...) {
            LogError("D3D12 cleanup: view={} Close() threw unknown exception\n", viewIndex);
            throw;
        }

        {
            // Kept unconditional: dumps the D3D12 info queue on device removal.
            HRESULT removedReason = device->GetDeviceRemovedReason();
            LogDebug("D3D12 cleanup: view={} post-Close DeviceRemovedReason={:#x}\n",
                     viewIndex, (uint32_t)removedReason);
            if (removedReason != S_OK) {
                DumpD3D12InfoQueue(device, "post-Close");
            }
        }

        // SAFE EXECUTION: Pass a stack array of pointers, NOT a casted pointer.
        ID3D12CommandList* const ppCommandLists[] = { cmdList };

        try {
            queue->ExecuteCommandLists(1, ppCommandLists);
        } catch (const std::exception& e) {
            LogError("D3D12 cleanup: view={} ExecuteCommandLists() threw exception: {}\n", viewIndex, e.what());
            throw;
        } catch (...) {
            LogError("D3D12 cleanup: view={} ExecuteCommandLists() threw unknown exception\n", viewIndex);
            throw;
        }

        // Diagnostic. Gated on log level so the driver call is skipped in production.
        if (log::GetLogLevel() <= log::LogLevel::Debug) {
            HRESULT removedReason = device->GetDeviceRemovedReason();
            LogDebug("D3D12 cleanup: view={} post-Execute DeviceRemovedReason={:#x}\n",
                    viewIndex, (uint32_t)removedReason);
        }

        // Signal fence (view 1 only). The GPU wait is deferred to the next
        // frame's waitForPreviousFrame() on both views, allowing ~1 frame of
        // CPU/GPU overlap instead of blocking here.
        if (viewIndex == xr::StereoView::Right) {
            m_fenceValue++;
            queue->Signal(m_compositionFence.Get(), m_fenceValue);
            m_currentFrameIndex = (m_currentFrameIndex + 1) % kFrameCount;
            m_cmdListIndex++;
#ifdef _DEBUG
            LogDebug("  D3D12 composition complete (fence={})\n", m_fenceValue);
#endif

            // Call base class virtual method directly to bypass layer's deferred release quirk
            releaseFullFovImage(stereoState);
        }

        // Reset tracking members for next frame
        m_currentCmdList = nullptr;
        m_currentDestination = nullptr;
        m_directBoundStereo = nullptr;
        m_directBoundFocus = nullptr;
    }

    // =======================================================================
    // Helper: reset command allocator and acquire command list
    // =======================================================================
    ComPtr<ID3D12GraphicsCommandList>& D3D12Compositor::resetCommandList(uint32_t viewIndex) {
        auto device = m_device.Get();

        CHECK_HRCMD(m_compositionAllocator[viewIndex]->Reset());

        const uint32_t listSlot = m_cmdListIndex % 2;
        ComPtr<ID3D12GraphicsCommandList>& cmdList = m_cmdList[viewIndex][listSlot];

        if (!cmdList) {
            HRESULT hr = device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                m_compositionAllocator[viewIndex].Get(), nullptr,
                IID_PPV_ARGS(cmdList.ReleaseAndGetAddressOf()));
            if (FAILED(hr)) {
                HRESULT removedReason = device->GetDeviceRemovedReason();
                LogError("D3D12: CreateCommandList failed: {:#x}, DeviceRemovedReason: {:#x}\n",
                         (uint32_t)hr, (uint32_t)removedReason);
                DumpD3D12InfoQueue(device, "CreateCommandList");
                // Common reasons:
                //   0x887A0006 = DXGI_ERROR_DEVICE_HUNG (TDR — GPU took too long)
                //   0x887A0005 = DXGI_ERROR_DEVICE_REMOVED (driver crash / resource conflict)
                //   0x887A0020 = DXGI_ERROR_DEVICE_RESET (driver update / TDR)
                //   0x887A0021 = DXGI_ERROR_DEVICE_REMOVED (DXGI_ERROR_INVALID_CALL from debug layer)
                // Throw a standard exception so the dispatcher's catch(std::exception&)
                // can safely catch it and log the error context.
                throw std::runtime_error(fmt::format(
                    "D3D12 CreateCommandList failed: 0x{:08X}, DeviceRemoved: 0x{:08X}",
                    static_cast<uint32_t>(hr),
                    static_cast<uint32_t>(removedReason)));
            }
        } else {
            CHECK_HRCMD(cmdList->Reset(m_compositionAllocator[viewIndex].Get(), nullptr));
        }
        return cmdList;
    }

    void D3D12Compositor::destroy() {
        // Skip all cleanup during DLL unload. The process is exiting and the app's D3D12
        // device/queue may already be dead. Calling Release() or Unmap() would crash.
        // The OS reclaims all memory on exit anyway.
        if (g_isUnloading) {
            return;
        }

        // If the device has been removed, touching any D3D12 object is undefined
        // behavior and will crash. Bail out and let the OS reclaim memory.
        if (!IsDeviceValid()) {
            LogError("D3D12Compositor::destroy: device removed/invalid, skipping cleanup\n");
            return;
        }

        // Wait for the GPU to finish all composition work before releasing any resources
        // that it may still be reading from.
        if (m_queue && m_compositionFence) {
            m_fenceValue++;
            CHECK_HRCMD(m_queue->Signal(m_compositionFence.Get(), m_fenceValue));
            if (m_compositionFence->GetCompletedValue() < m_fenceValue) {
                // Reuse the cached fence event (auto-reset).
                CHECK_HRCMD(m_compositionFence->SetEventOnCompletion(m_fenceValue, m_fenceEvent));
                WaitForSingleObject(m_fenceEvent, 5000); // 5-second timeout to prevent infinite hangs
            }
        }

        // Idempotent: safe to call multiple times (ComPtr::Reset() is a no-op on already-null pointers).
        m_swapchainStates.clear();
        m_rtvHeap.Reset();
        m_samplerHeap.Reset();
        m_peripheralSamplerDirty = true;
        m_projectionPSO.Reset();
        m_sharpeningPSO.Reset();
        m_easuPSO.Reset();  // EASU
        m_mipGenPSO.Reset();  // Mip gen
        m_projectionRootSignature.Reset();
        m_sharpeningRootSignature.Reset();
        m_easuRootSignature.Reset();  // EASU
        m_mipGenRootSignature.Reset();  // Mip gen
        m_blankTexture.Reset();
        m_compositionAllocator[0].Reset();
        m_compositionAllocator[1].Reset();
        m_compositionFence.Reset();

        // Close the cached fence event (created in initialize()).
        if (m_fenceEvent) {
            CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }

        // Unmap persistently-mapped resources before resetting them.
        for (uint32_t f = 0; f < kFrameCount; f++) {
            if (m_projectionVSConstants[f]) {
                m_projectionVSConstants[f]->Unmap(0, nullptr);
            }
            if (m_projectionPSConstants[f]) {
                m_projectionPSConstants[f]->Unmap(0, nullptr);
            }
            // Unmap the EASU + sharpening buffers too.
            if (m_easuConstants[f]) {
                m_easuConstants[f]->Unmap(0, nullptr);
                m_easuConstantsMapped[f] = nullptr;
            }
            if (m_sharpeningCSConstants[f]) {
                m_sharpeningCSConstants[f]->Unmap(0, nullptr);
                m_sharpeningConstantsMapped[f] = nullptr;
            }
            m_projectionVSConstants[f].Reset();
            m_projectionPSConstants[f].Reset();
            m_sharpeningCSConstants[f].Reset();
            m_easuConstants[f].Reset();  // EASU
            m_mipGenConstants[f].Reset();  // Mip gen
            m_cbvSrvHeap[f][0].Reset();
            m_cbvSrvHeap[f][1].Reset();
        }
    }

    void D3D12Compositor::waitForGpuIdle() {
        // Skip GPU sync during DLL unload — the queue/fence may already be dead, and there
        // is no point waiting on a GPU that will die with the process anyway.
        if (g_isUnloading) {
            return;
        }

        // If the device has been removed, issuing fence signals/waits will crash.
        if (!IsDeviceValid()) {
            LogError("D3D12Compositor::waitForGpuIdle: device removed/invalid, skipping GPU sync\n");
            return;
        }

        if (m_queue && m_compositionFence) {
            m_fenceValue++;
            CHECK_HRCMD(m_queue->Signal(m_compositionFence.Get(), m_fenceValue));
            if (m_compositionFence->GetCompletedValue() < m_fenceValue) {
                // Reuse the cached fence event (auto-reset).
                CHECK_HRCMD(m_compositionFence->SetEventOnCompletion(m_fenceValue, m_fenceEvent));
                WaitForSingleObject(m_fenceEvent, 5000); // 5-second timeout
            }
        }
    }

    bool D3D12Compositor::IsDeviceValid() const {
        // A null device means we were never initialized (or already destroyed).
        if (!m_device) {
            return false;
        }
        // GetDeviceRemovedReason returns S_OK while the device is healthy; any
        // other HRESULT (e.g. DXGI_ERROR_DEVICE_REMOVED) means the device is gone.
        HRESULT hr = m_device->GetDeviceRemovedReason();
        if (hr != S_OK) {
            LogError("D3D12Compositor::IsDeviceValid: device removed, hr=0x{:08X}\n", static_cast<unsigned int>(hr));
            return false;
        }
        return true;
    }

    std::unique_ptr<ICompositor> createD3D12Compositor(ID3D12Device* device, ID3D12CommandQueue* queue, OpenXrApi* openXrApi) {
        return std::make_unique<D3D12Compositor>(device, queue, openXrApi);
    }

} // namespace openxr_api_layer
