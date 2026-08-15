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

#include "pch.h"

namespace openxr_api_layer {

/// Compile-time descriptor heap layout for the 16-slot CBV/SRV heap.
/// Each eye gets its own heap instance (per-frame, per-eye), so indices
/// are relative to a single heap's start.
///
/// Layout (16 descriptors):
///   [0-3]  Reserved (formerly VS/PS CBVs — now root CBVs, no heap slot needed)
///   [4]  Stereo texture SRV
///   [5]  Focus texture SRV
///   [6]  Blank texture SRV (shared across eyes)
///   [7]  Reserved
///   [8]  Sharpening input SRV (flat focus)
///   [9]  Sharpening output UAV (sharpened focus)
///   [10-11] Reserved
///   [12] Sharpening CS CBV (static)
///   [13-15] Reserved
struct DescriptorLayout {
    // VS/PS CBVs are now root CBVs (SetGraphicsRootConstantBufferView) and no
    // longer occupy descriptor heap slots. The VsCbv/PsCbv helpers are kept
    // for reference but are not used by the projection pass.
    static constexpr uint32_t VsCbv(uint32_t viewIndex) { return viewIndex * 2; }
    static constexpr uint32_t PsCbv(uint32_t viewIndex) { return viewIndex * 2 + 1; }

    // Per-frame SRV bindings for the projection pass
    static constexpr uint32_t kStereoSrv = 4;
    static constexpr uint32_t kFocusSrv  = 5;
    static constexpr uint32_t kBlankSrv  = 6;

    // Sharpening compute pass bindings
    static constexpr uint32_t kSharpenSrv = 8;
    static constexpr uint32_t kSharpenUav = 9;
    static constexpr uint32_t kSharpenCbv = 12;

    // EASU compute pass bindings
    static constexpr uint32_t kEasuSrv = 10;  // peripheral flat image (input)
    static constexpr uint32_t kEasuUav = 11;  // EASU upscaled image (output)
    static constexpr uint32_t kEasuCbv = 13;  // FSR1Constants

    // Mip-gen compute pass bindings
    // D3D12 GPUs read shader-visible descriptor heap entries at EXECUTION time,
    // not recording time. The mip-gen loop dispatches once per mip level; if all
    // iterations shared a single SRV/UAV slot, every dispatch would read the
    // LAST iteration's descriptors (reading a sub-mip and writing the wrong
    // mip), leaving easuImage mip 0 unwritten -> black periphery. To fix this,
    // each mip level gets its OWN pre-baked slot, created once at allocation
    // time. This also eliminates the per-frame CopyDescriptorsSimple calls.
    //
    // CappedPeripheralMipCount() caps the chain at 6 levels total (mips 0..5),
    // so mips 1..5 need descriptors -> 5 slots each for SRV and UAV.
    static constexpr uint32_t kMaxMipLevels = 5;  // mips 1..5 (mip 0 is the EASU output)
    static constexpr uint32_t kMipGenSrvBase = 16;  // start of per-mip SRV array
    static constexpr uint32_t kMipGenUavBase = kMipGenSrvBase + kMaxMipLevels;  // 21

    static constexpr uint32_t kHeapSize = kMipGenUavBase + kMaxMipLevels;  // 26

    /// Compute the GPU descriptor handle for a given slot.
    static CD3DX12_GPU_DESCRIPTOR_HANDLE GpuHandle(
        ID3D12DescriptorHeap* heap, uint32_t slot, uint32_t incrementSize) {
        CD3DX12_GPU_DESCRIPTOR_HANDLE handle(heap->GetGPUDescriptorHandleForHeapStart());
        handle.Offset(slot * incrementSize);
        return handle;
    }

    /// Compute the CPU descriptor handle for a given slot.
    static CD3DX12_CPU_DESCRIPTOR_HANDLE CpuHandle(
        ID3D12DescriptorHeap* heap, uint32_t slot, uint32_t incrementSize) {
        CD3DX12_CPU_DESCRIPTOR_HANDLE handle(heap->GetCPUDescriptorHandleForHeapStart());
        handle.Offset(slot * incrementSize);
        return handle;
    }
};

// Compile-time validation of descriptor slot assignments
static_assert(DescriptorLayout::VsCbv(0) == 0, "Left VS CBV must be at slot 0");
static_assert(DescriptorLayout::PsCbv(0) == 1, "Left PS CBV must be at slot 1");
static_assert(DescriptorLayout::VsCbv(1) == 2, "Right VS CBV must be at slot 2");
static_assert(DescriptorLayout::PsCbv(1) == 3, "Right PS CBV must be at slot 3");
static_assert(DescriptorLayout::kStereoSrv == 4, "Stereo SRV must be at slot 4");
static_assert(DescriptorLayout::kFocusSrv == 5, "Focus SRV must be at slot 5");
static_assert(DescriptorLayout::kBlankSrv == 6, "Blank SRV must be at slot 6");
static_assert(DescriptorLayout::kSharpenSrv == 8, "Sharpen SRV must be at slot 8");
static_assert(DescriptorLayout::kSharpenUav == 9, "Sharpen UAV must be at slot 9");
static_assert(DescriptorLayout::kSharpenCbv == 12, "Sharpen CBV must be at slot 12");
static_assert(DescriptorLayout::kMipGenSrvBase == 16, "Mip-gen SRV array must start at slot 16");
static_assert(DescriptorLayout::kMipGenUavBase == 21, "Mip-gen UAV array must start at slot 21");
static_assert(DescriptorLayout::kHeapSize == 26, "Heap must have 26 slots");

} // namespace openxr_api_layer
