/*
 * OpenKneeboard
 *
 * Copyright (C) 2022 Fred Emmott <fred@fredemmott.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include "viewer-d3d12.h"

#include <OpenKneeboard/D3D12.h>

#include <directxtk12/ScreenGrab.h>

namespace OpenKneeboard::Viewer {

D3D12Renderer::D3D12Renderer(IDXGIAdapter* dxgiAdapter) {
  dprint(__FUNCSIG__);

#ifdef DEBUG
  dprint("Enabling D3D12 debug features");
  winrt::com_ptr<ID3D12Debug5> debug;
  winrt::check_hresult(D3D12GetDebugInterface(IID_PPV_ARGS(debug.put())));
  debug->EnableDebugLayer();
  debug->SetEnableAutoName(true);
  debug->SetEnableGPUBasedValidation(true);
#endif

  mDevice.capture(D3D12CreateDevice, dxgiAdapter, D3D_FEATURE_LEVEL_12_1);

  D3D12_COMMAND_QUEUE_DESC queueDesc {D3D12_COMMAND_LIST_TYPE_DIRECT};
  mCommandQueue.capture(mDevice, &ID3D12Device::CreateCommandQueue, &queueDesc);
  mCommandAllocator.capture(
    mDevice,
    &ID3D12Device::CreateCommandAllocator,
    D3D12_COMMAND_LIST_TYPE_DIRECT);

  mSpriteBatch = std::make_unique<D3D12::SpriteBatch>(
    mDevice.get(), mCommandQueue.get(), DXGI_FORMAT_B8G8R8A8_UNORM);

  mDestRTVHeap = std::make_unique<DirectX::DescriptorHeap>(
    mDevice.get(),
    D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
    D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    1);
}

D3D12Renderer::~D3D12Renderer() = default;

std::wstring_view D3D12Renderer::GetName() const noexcept {
  return {L"D3D12"};
}

SHM::CachedReader* D3D12Renderer::GetSHM() {
  return &mSHM;
}

SHM::Snapshot D3D12Renderer::MaybeGetSnapshot() {
  return mSHM.MaybeGet();
}

void D3D12Renderer::Initialize(uint8_t swapchainLength) {
  mSHM.InitializeCache(mDevice.get(), mCommandQueue.get(), swapchainLength);
}

uint64_t D3D12Renderer::Render(
  SHM::IPCClientTexture* sourceTexture,
  const PixelRect& sourceRect,
  HANDLE destTextureHandle,
  const PixelSize& destTextureDimensions,
  const PixelRect& destRect,
  HANDLE fenceHandle,
  uint64_t fenceValueIn) {
  OPENKNEEBOARD_TraceLoggingScope("Viewer::D3D12Renderer::Render");

  if (destTextureHandle != mDestHandle) {
    mDestTexture = nullptr;
    mDestTexture.capture(
      mDevice, &ID3D12Device::OpenSharedHandle, destTextureHandle);
    mDestHandle = destTextureHandle;
  }

  if (fenceHandle != mFenceHandle) {
    mFence.capture(mDevice, &ID3D12Device::OpenSharedHandle, fenceHandle);
    mFenceHandle = fenceHandle;
  }

  auto source = reinterpret_cast<SHM::D3D12::Texture*>(sourceTexture);
  auto dest = mDestRTVHeap->GetFirstCpuHandle();
  mDevice->CreateRenderTargetView(mDestTexture.get(), nullptr, dest);

  if (mCommandList) {
    winrt::check_hresult(mCommandList->Reset(mCommandAllocator.get(), nullptr));
  } else {
    mCommandList.capture(
      mDevice,
      &ID3D12Device::CreateCommandList,
      0,
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      mCommandAllocator.get(),
      nullptr);
  }

  ID3D12DescriptorHeap* heaps[] {source->GetD3D12ShaderResourceViewHeap()};
  mCommandList->SetDescriptorHeaps(std::size(heaps), heaps);

  mSpriteBatch->Begin(mCommandList.get(), dest, destTextureDimensions);
  mSpriteBatch->Draw(
    source->GetD3D12ShaderResourceViewGPUHandle(),
    source->GetDimensions(),
    sourceRect,
    destRect);
  mSpriteBatch->End();

  winrt::check_hresult(mCommandList->Close());

  winrt::check_hresult(mCommandQueue->Wait(mFence.get(), fenceValueIn));

  ID3D12CommandList* lists[] {mCommandList.get()};
  mCommandQueue->ExecuteCommandLists(std::size(lists), lists);

  const auto fenceValueOut = fenceValueIn + 1;
  winrt::check_hresult(mCommandQueue->Signal(mFence.get(), fenceValueOut));
  return fenceValueOut;
}

void D3D12Renderer::SaveTextureToFile(
  SHM::IPCClientTexture* texture,
  const std::filesystem::path& path) {
  SaveTextureToFile(
    reinterpret_cast<SHM::D3D12::Texture*>(texture)->GetD3D12Texture(), path);
}
void D3D12Renderer::SaveTextureToFile(
  ID3D12Resource* texture,
  const std::filesystem::path& path) {
  winrt::check_hresult(DirectX::SaveDDSTextureToFile(
    mCommandQueue.get(), texture, path.wstring().c_str()));
}

}// namespace OpenKneeboard::Viewer