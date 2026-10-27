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
  uint8_t positionMatrixCount = 0;
  uint8_t normalMatrixCount = 0;
  bool perspective = false;
  bool indexedMatrices = false;
  // A 2D draw compositing the framebuffer back over itself: bloom, blur and the
  // rest of the native post-processing chain. It belongs to the rendered image,
  // not to the game's 2D layer, so it must stay where the game aimed it.
  bool nativeEfbEffect = false;
  // For such an effect sampling a fresh, frame-sized depth copy: that copy. Mario Kart Wii's Time Trial ghost is
  // drawn alone into the cleared EFB, copied out (colour and depth), and blended back over the race by one
  // orthographic quad whose depth comes from this copy. An eye re-issues the ghost's own draws in that quad's place
  // (gfx common.cpp, link_composite_source) instead of stamping the flat copy over its view.
  const gfx::TextureRef* compositeDepthCopy = nullptr;
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
