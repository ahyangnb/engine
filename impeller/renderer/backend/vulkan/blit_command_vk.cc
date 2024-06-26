// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/vulkan/blit_command_vk.h"

#include <cstdint>

#include "impeller/renderer/backend/vulkan/barrier_vk.h"
#include "impeller/renderer/backend/vulkan/command_encoder_vk.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "vulkan/vulkan_core.h"
#include "vulkan/vulkan_enums.hpp"
#include "vulkan/vulkan_structs.hpp"

namespace impeller {

static void InsertImageMemoryBarrier(const vk::CommandBuffer& cmd,
                                     const vk::Image& image,
                                     vk::AccessFlags src_access_mask,
                                     vk::AccessFlags dst_access_mask,
                                     vk::ImageLayout old_layout,
                                     vk::ImageLayout new_layout,
                                     vk::PipelineStageFlags src_stage,
                                     vk::PipelineStageFlags dst_stage,
                                     uint32_t base_mip_level,
                                     uint32_t mip_level_count = 1u) {
  if (old_layout == new_layout) {
    return;
  }

  vk::ImageMemoryBarrier barrier;
  barrier.srcAccessMask = src_access_mask;
  barrier.dstAccessMask = dst_access_mask;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.baseMipLevel = base_mip_level;
  barrier.subresourceRange.levelCount = mip_level_count;
  barrier.subresourceRange.baseArrayLayer = 0u;
  barrier.subresourceRange.layerCount = 1u;

  cmd.pipelineBarrier(src_stage, dst_stage, {}, nullptr, nullptr, barrier);
}

BlitEncodeVK::~BlitEncodeVK() = default;

//------------------------------------------------------------------------------
/// BlitCopyTextureToTextureCommandVK
///

BlitCopyTextureToTextureCommandVK::~BlitCopyTextureToTextureCommandVK() =
    default;

std::string BlitCopyTextureToTextureCommandVK::GetLabel() const {
  return label;
}

bool BlitCopyTextureToTextureCommandVK::Encode(
    CommandEncoderVK& encoder) const {
  const auto& cmd_buffer = encoder.GetCommandBuffer();

  const auto& src = TextureVK::Cast(*source);
  const auto& dst = TextureVK::Cast(*destination);

  if (!encoder.Track(source) || !encoder.Track(destination)) {
    return false;
  }

  BarrierVK src_barrier;
  src_barrier.cmd_buffer = cmd_buffer;
  src_barrier.new_layout = vk::ImageLayout::eTransferSrcOptimal;
  src_barrier.src_access = vk::AccessFlagBits::eTransferWrite |
                           vk::AccessFlagBits::eShaderWrite |
                           vk::AccessFlagBits::eColorAttachmentWrite;
  src_barrier.src_stage = vk::PipelineStageFlagBits::eTransfer |
                          vk::PipelineStageFlagBits::eFragmentShader |
                          vk::PipelineStageFlagBits::eColorAttachmentOutput;
  src_barrier.dst_access = vk::AccessFlagBits::eTransferRead;
  src_barrier.dst_stage = vk::PipelineStageFlagBits::eTransfer;

  BarrierVK dst_barrier;
  dst_barrier.cmd_buffer = cmd_buffer;
  dst_barrier.new_layout = vk::ImageLayout::eTransferDstOptimal;
  dst_barrier.src_access = {};
  dst_barrier.src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
  dst_barrier.dst_access =
      vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferWrite;
  dst_barrier.dst_stage = vk::PipelineStageFlagBits::eFragmentShader |
                          vk::PipelineStageFlagBits::eTransfer;

  if (!src.SetLayout(src_barrier) || !dst.SetLayout(dst_barrier)) {
    VALIDATION_LOG << "Could not complete layout transitions.";
    return false;
  }

  vk::ImageCopy image_copy;

  image_copy.setSrcSubresource(
      vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1));
  image_copy.setDstSubresource(
      vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1));

  image_copy.srcOffset =
      vk::Offset3D(source_region.GetX(), source_region.GetY(), 0);
  image_copy.dstOffset =
      vk::Offset3D(destination_origin.x, destination_origin.y, 0);
  image_copy.extent =
      vk::Extent3D(source_region.GetWidth(), source_region.GetHeight(), 1);

  // Issue the copy command now that the images are already in the right
  // layouts.
  cmd_buffer.copyImage(src.GetImage(),          //
                       src_barrier.new_layout,  //
                       dst.GetImage(),          //
                       dst_barrier.new_layout,  //
                       image_copy               //
  );

  // If this is an onscreen texture, do not transition the layout
  // back to shader read.
  if (dst.IsSwapchainImage()) {
    return true;
  }

  BarrierVK barrier;
  barrier.cmd_buffer = cmd_buffer;
  barrier.new_layout = vk::ImageLayout::eShaderReadOnlyOptimal;
  barrier.src_access = {};
  barrier.src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
  barrier.dst_access = vk::AccessFlagBits::eShaderRead;
  barrier.dst_stage = vk::PipelineStageFlagBits::eFragmentShader;

  return dst.SetLayout(barrier);
}

//------------------------------------------------------------------------------
/// BlitCopyTextureToBufferCommandVK
///

BlitCopyTextureToBufferCommandVK::~BlitCopyTextureToBufferCommandVK() = default;

std::string BlitCopyTextureToBufferCommandVK::GetLabel() const {
  return label;
}

bool BlitCopyTextureToBufferCommandVK::Encode(CommandEncoderVK& encoder) const {
  const auto& cmd_buffer = encoder.GetCommandBuffer();

  // cast source and destination to TextureVK
  const auto& src = TextureVK::Cast(*source);

  if (!encoder.Track(source) || !encoder.Track(destination)) {
    return false;
  }

  BarrierVK barrier;
  barrier.cmd_buffer = cmd_buffer;
  barrier.new_layout = vk::ImageLayout::eTransferSrcOptimal;
  barrier.src_access = vk::AccessFlagBits::eShaderWrite |
                       vk::AccessFlagBits::eTransferWrite |
                       vk::AccessFlagBits::eColorAttachmentWrite;
  barrier.src_stage = vk::PipelineStageFlagBits::eFragmentShader |
                      vk::PipelineStageFlagBits::eTransfer |
                      vk::PipelineStageFlagBits::eColorAttachmentOutput;
  barrier.dst_access = vk::AccessFlagBits::eShaderRead;
  barrier.dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                      vk::PipelineStageFlagBits::eFragmentShader;

  const auto& dst = DeviceBufferVK::Cast(*destination);

  vk::BufferImageCopy image_copy;
  image_copy.setBufferOffset(destination_offset);
  image_copy.setBufferRowLength(0);
  image_copy.setBufferImageHeight(0);
  image_copy.setImageSubresource(
      vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1));
  image_copy.setImageOffset(
      vk::Offset3D(source_region.GetX(), source_region.GetY(), 0));
  image_copy.setImageExtent(
      vk::Extent3D(source_region.GetWidth(), source_region.GetHeight(), 1));

  if (!src.SetLayout(barrier)) {
    VALIDATION_LOG << "Could not encode layout transition.";
    return false;
  }

  cmd_buffer.copyImageToBuffer(src.GetImage(),      //
                               barrier.new_layout,  //
                               dst.GetBuffer(),     //
                               image_copy           //
  );

  // If the buffer is used for readback, then apply a transfer -> host memory
  // barrier.
  if (destination->GetDeviceBufferDescriptor().readback) {
    vk::MemoryBarrier barrier;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;

    cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eHost, {}, 1,
                               &barrier, 0, {}, 0, {});
  }

  return true;
}

//------------------------------------------------------------------------------
/// BlitCopyBufferToTextureCommandVK
///

BlitCopyBufferToTextureCommandVK::~BlitCopyBufferToTextureCommandVK() = default;

std::string BlitCopyBufferToTextureCommandVK::GetLabel() const {
  return label;
}

bool BlitCopyBufferToTextureCommandVK::Encode(CommandEncoderVK& encoder) const {
  const auto& cmd_buffer = encoder.GetCommandBuffer();

  // cast destination to TextureVK
  const auto& dst = TextureVK::Cast(*destination);
  const auto& src = DeviceBufferVK::Cast(*source.buffer);

  if (!encoder.Track(source.buffer) || !encoder.Track(destination)) {
    return false;
  }

  BarrierVK dst_barrier;
  dst_barrier.cmd_buffer = cmd_buffer;
  dst_barrier.new_layout = vk::ImageLayout::eTransferDstOptimal;
  dst_barrier.src_access = {};
  dst_barrier.src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
  dst_barrier.dst_access =
      vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferWrite;
  dst_barrier.dst_stage = vk::PipelineStageFlagBits::eFragmentShader |
                          vk::PipelineStageFlagBits::eTransfer;

  vk::BufferImageCopy image_copy;
  image_copy.setBufferOffset(source.range.offset);
  image_copy.setBufferRowLength(0);
  image_copy.setBufferImageHeight(0);
  image_copy.setImageSubresource(
      vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1));
  image_copy.setImageOffset(
      vk::Offset3D(destination_origin.x, destination_origin.y, 0));
  image_copy.setImageExtent(vk::Extent3D(destination->GetSize().width,
                                         destination->GetSize().height, 1));

  if (!dst.SetLayout(dst_barrier)) {
    VALIDATION_LOG << "Could not encode layout transition.";
    return false;
  }

  cmd_buffer.copyBufferToImage(src.GetBuffer(),         //
                               dst.GetImage(),          //
                               dst_barrier.new_layout,  //
                               image_copy               //
  );

  return true;
}

//------------------------------------------------------------------------------
/// BlitGenerateMipmapCommandVK
///

BlitGenerateMipmapCommandVK::~BlitGenerateMipmapCommandVK() = default;

std::string BlitGenerateMipmapCommandVK::GetLabel() const {
  return label;
}

bool BlitGenerateMipmapCommandVK::Encode(CommandEncoderVK& encoder) const {
  auto& src = TextureVK::Cast(*texture);

  const auto size = src.GetTextureDescriptor().size;
  uint32_t mip_count = src.GetTextureDescriptor().mip_count;

  if (mip_count < 2u) {
    return true;
  }

  const auto& image = src.GetImage();
  const auto& cmd = encoder.GetCommandBuffer();

  if (!encoder.Track(texture)) {
    return false;
  }

  // Initialize all mip levels to be in TransferDst mode. Later, in a loop,
  // after writing to that mip level, we'll first switch its layout to
  // TransferSrc to prepare the mip level after it, use the image as the source
  // of the blit, before finally switching it to ShaderReadOnly so its available
  // for sampling in a shader.
  InsertImageMemoryBarrier(
      cmd,                                   // command buffer
      image,                                 // image
      vk::AccessFlagBits::eTransferWrite,    // src access mask
      vk::AccessFlagBits::eTransferRead,     // dst access mask
      src.GetLayout(),                       // old layout
      vk::ImageLayout::eTransferDstOptimal,  // new layout
      vk::PipelineStageFlagBits::eTransfer,  // src stage
      vk::PipelineStageFlagBits::eTransfer,  // dst stage
      0u,                                    // mip level
      mip_count                              // mip level count
  );

  vk::ImageMemoryBarrier barrier;
  barrier.image = image;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.subresourceRange.levelCount = 1;

  // Blit from the mip level N - 1 to mip level N.
  size_t width = size.width;
  size_t height = size.height;
  for (size_t mip_level = 1u; mip_level < mip_count; mip_level++) {
    barrier.subresourceRange.baseMipLevel = mip_level - 1;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;

    // We just finished writing to the previous (N-1) mip level or it was the
    // base mip level. These were initialized to TransferDst earler. We are now
    // going to read from it to write to the current level (N) . So it must be
    // converted to TransferSrc.
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                        {barrier});

    vk::ImageBlit blit;
    blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.srcSubresource.baseArrayLayer = 0u;
    blit.srcSubresource.layerCount = 1u;
    blit.srcSubresource.mipLevel = mip_level - 1;

    blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.dstSubresource.baseArrayLayer = 0u;
    blit.dstSubresource.layerCount = 1u;
    blit.dstSubresource.mipLevel = mip_level;

    // offsets[0] is origin.
    blit.srcOffsets[1].x = std::max<int32_t>(width, 1u);
    blit.srcOffsets[1].y = std::max<int32_t>(height, 1u);
    blit.srcOffsets[1].z = 1u;

    width = width / 2;
    height = height / 2;

    // offsets[0] is origin.
    blit.dstOffsets[1].x = std::max<int32_t>(width, 1u);
    blit.dstOffsets[1].y = std::max<int32_t>(height, 1u);
    blit.dstOffsets[1].z = 1u;

    cmd.blitImage(image,                                 // src image
                  vk::ImageLayout::eTransferSrcOptimal,  // src layout
                  image,                                 // dst image
                  vk::ImageLayout::eTransferDstOptimal,  // dst layout
                  1u,                                    // region count
                  &blit,                                 // regions
                  vk::Filter::eLinear                    // filter
    );

    barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

    // Now that the blit is done, the image at the previous level (N-1)
    // is done reading from (TransferSrc)/ Now we must prepare it to be read
    // from a shader (ShaderReadOnly).
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
                        {barrier});
  }

  barrier.subresourceRange.baseMipLevel = mip_count - 1;
  barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
  barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
  barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;

  cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                      vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
                      {barrier});

  // We modified the layouts of this image from underneath it. Tell it its new
  // state so it doesn't try to perform redundant transitions under the hood.
  src.SetLayoutWithoutEncoding(vk::ImageLayout::eShaderReadOnlyOptimal);
  src.SetMipMapGenerated();

  return true;
}

}  // namespace impeller
