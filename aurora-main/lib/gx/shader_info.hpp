#pragma once

#include "gx.hpp"
// MaxInterpolatedFrames / FrameInterpolationDrawIdentity, and the interpolation API that build_uniform feeds.
#include "frame_interpolation.hpp"

namespace aurora::gx {
struct UniformReplayLayout {
  uint32_t projectionOffset = 0;
  uint32_t positionOffset = 0;
  uint32_t normalOffset = 0;
  uint32_t positionMatrixMask = 0;
  // The position-memory matrices of positionMatrixMask that only a texgen reads, never the vertex
  // position. NW4R's projection mapping (Rainbow Road's road, Coconut Mall's glass) casts a texture
  // from the camera through GX_PNMTX0, which the compacted layout stages apart from the current
  // matrix. The eyes leave these as recorded: through the eye's view, the texture would be cast
  // from the headset and follow the head. Interpolation still treats them as position matrices.
  // In the absolute layout one staged matrix serves both readers, so nothing is marked there.
  uint32_t texgenMatrixMask = 0;
  uint8_t positionMatrixCount = 0;
  uint8_t normalMatrixCount = 0;
  bool perspective = false;
  bool indexedMatrices = false;
  // A 2D draw compositing the framebuffer back over itself: bloom, blur and the
  // rest of the native post-processing chain. It belongs to the rendered image,
  // not to the game's 2D layer, so it must stay where the game aimed it.
  bool nativeEfbEffect = false;
  float worldHudDepth = 0.0f;
  float worldHudAnchorX = 0.0f;
  float worldHudAnchorY = 0.0f;
  // ShaderInfo::structSize (the WGSL struct's end, before the range is padded to the offset alignment):
  // where a virtual-screen variant's clip block starts (gx::screen_clip_offset).
  uint32_t uniformSize = 0;
};

struct UniformRanges {
  gfx::Range current;
  std::array<gfx::Range, MaxInterpolatedFrames> interpolated;
  gfx::Range previous;
  UniformReplayLayout replayLayout;
};

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
Light prepare_shader_light(Light light) noexcept;
UniformRanges build_uniform(const ShaderInfo& info, uint32_t vtxStart, const BindGroupRanges& ranges,
                            const FrameInterpolationDrawIdentity& drawIdentity, bool perspective,
                            uint16_t usedPnMtxMask = 1) noexcept;
u8 color_channel(GXChannelID id) noexcept;
}; // namespace aurora::gx
