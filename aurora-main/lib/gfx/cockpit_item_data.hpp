// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Small, bounded reader for MKW's item BRRES models: bind-pose geometry plus
// each material's texture layers, texgens and TEV stages. All input is copied
// from the user's mapped Common.szs. No game data is shipped.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace aurora::gfx::cockpit_item::data {

struct Reader {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  bool has(size_t at, size_t count) const { return at <= size && count <= size - at; }
  uint8_t u8(size_t at) const { return has(at,1) ? bytes[at] : 0; }
  uint16_t u16(size_t at) const { return has(at,2) ? (uint16_t(bytes[at])<<8)|bytes[at+1] : 0; }
  uint32_t u32(size_t at) const {
    return has(at,4) ? (uint32_t(bytes[at])<<24)|(uint32_t(bytes[at+1])<<16)|
                         (uint32_t(bytes[at+2])<<8)|bytes[at+3] : 0;
  }
  float f32(size_t at) const { uint32_t bits=u32(at); float out; std::memcpy(&out,&bits,4); return out; }
  // A section-relative offset: returns 0 (never valid here) when it leaves the file.
  size_t rel(size_t base,size_t at) const {
    const int64_t target=int64_t(base)+int32_t(u32(at));
    return target>0 && size_t(target)<size ? size_t(target) : 0;
  }
  std::string str(size_t at) const {
    if(at>=size) return {};
    size_t end=at;
    while(end<size && end-at<128 && bytes[end]) ++end;
    return end<size && end-at<128 ? std::string(reinterpret_cast<const char*>(bytes+at),end-at) : std::string{};
  }
};

struct Entry { std::string name; size_t at; };
inline std::vector<Entry> dict(Reader r,size_t at) {
  if(!r.has(at,8)) return {};
  const uint32_t count=r.u32(at+4);
  if(count>4096 || !r.has(at+8,size_t(count+1)*16)) return {};
  std::vector<Entry> out;
  out.reserve(count);
  for(uint32_t i=1;i<=count;++i) {
    const size_t e=at+8+size_t(i)*16;
    const size_t target=at+r.u32(e+12);
    const auto name=r.str(at+r.u32(e+8));
    if(name.empty() || target>=r.size) return {};
    out.push_back({name,target});
  }
  return out;
}
inline size_t find(const std::vector<Entry>& entries,const std::string& name) {
  for(const auto& e:entries) if(e.name==name) return e.at;
  return 0;
}

inline std::vector<uint8_t> yaz0(Reader input) {
  if(input.size>32u*1024u*1024u || !input.has(0,16)) return {};
  if(input.u32(0)!=0x59617a30u) return std::vector<uint8_t>(input.bytes,input.bytes+input.size);
  const size_t length=input.u32(4);
  if(length==0 || length>32u*1024u*1024u) return {};
  std::vector<uint8_t> out;
  out.reserve(length);
  size_t at=16;
  while(out.size()<length) {
    if(!input.has(at,1)) return {};
    const uint8_t control=input.u8(at++);
    for(int bit=7;bit>=0 && out.size()<length;--bit) {
      if(control & (1u<<bit)) {
        if(!input.has(at,1)) return {};
        out.push_back(input.u8(at++));
      } else {
        if(!input.has(at,2)) return {};
        const uint8_t a=input.u8(at++), b=input.u8(at++);
        size_t count=a>>4;
        if(count) count+=2;
        else { if(!input.has(at,1)) return {}; count=size_t(input.u8(at++))+18; }
        const size_t distance=((size_t(a&15)<<8)|b)+1;
        if(distance>out.size() || count>length-out.size()) return {};
        for(size_t j=0;j<count;++j) out.push_back(out[out.size()-distance]);
      }
    }
  }
  return out;
}

struct V3 { float x=0,y=0,z=0; };
struct V2 { float x=0,y=0; };
using Color = std::array<float,4>;
struct Matrix {
  std::array<float,12> v{1,0,0,0,0,1,0,0,0,0,1,0};
};
inline V3 point(const Matrix& m,V3 p) {
  const auto& a=m.v;
  return {a[0]*p.x+a[1]*p.y+a[2]*p.z+a[3],a[4]*p.x+a[5]*p.y+a[6]*p.z+a[7],
          a[8]*p.x+a[9]*p.y+a[10]*p.z+a[11]};
}
inline V3 direction(const Matrix& m,V3 p) {
  const auto& a=m.v;
  return {a[0]*p.x+a[1]*p.y+a[2]*p.z,a[4]*p.x+a[5]*p.y+a[6]*p.z,
          a[8]*p.x+a[9]*p.y+a[10]*p.z};
}

inline float component(Reader r,size_t at,uint32_t type,uint8_t shift) {
  if(type==4) return r.f32(at);
  const float scale=std::ldexp(1.0f,-int(shift));
  if(type==0) return r.u8(at)*scale;
  if(type==1) return int8_t(r.u8(at))*scale;
  if(type==2) return r.u16(at)*scale;
  if(type==3) return int16_t(r.u16(at))*scale;
  return 0;
}
struct Array {
  uint32_t id=0;
  std::vector<V3> values;
};
enum class ArrayKind { Position, Normal, UV };
inline std::vector<Array> arrays(Reader r,size_t model,uint32_t dictionary_offset,ArrayKind kind) {
  std::vector<Array> result;
  if(!dictionary_offset) return result;
  for(const auto& e:dict(r,model+dictionary_offset)) {
    const size_t h=e.at;
    if(!r.has(h,0x20)) return {};
    const uint32_t type=r.u32(h+0x18), comps=r.u32(h+0x14);
    const uint8_t shift=r.u8(h+0x1c), stride=r.u8(h+0x1d);
    const uint16_t count=r.u16(h+0x1e);
    const size_t data=h+r.u32(h+8);
    const size_t elem=type==4?4:(type==2||type==3?2:1);
    const size_t n=kind==ArrayKind::UV?(comps?2:1):
                   kind==ArrayKind::Normal?(comps?9:3):(comps?3:2);
    if(type>4 || stride<n*elem || !r.has(data,size_t(count)*stride)) return {};
    Array a; a.id=r.u32(h+0x10);a.values.reserve(count);
    for(uint32_t i=0;i<count;++i) {
      const size_t p=data+size_t(i)*stride;
      a.values.push_back({component(r,p,type,shift),n>1?component(r,p+elem,type,shift):0,
                          n>2?component(r,p+2*elem,type,shift):0});
    }
    result.push_back(std::move(a));
  }
  return result;
}
template <typename T> inline const T* array_id(const std::vector<T>& entries,uint16_t id) {
  for(const auto& a:entries) if(a.id==id) return &a;
  return nullptr;
}
struct ColorArray {
  uint32_t id=0;
  std::vector<Color> values;
};
// GX colour array formats: RGB565, RGB8, RGBX8, RGBA4, RGBA6, RGBA8.
inline std::vector<ColorArray> color_arrays(Reader r,size_t model,uint32_t dictionary_offset) {
  std::vector<ColorArray> result;
  if(!dictionary_offset) return result;
  constexpr uint8_t sizes[6]{2,3,4,2,3,4};
  for(const auto& e:dict(r,model+dictionary_offset)) {
    const size_t h=e.at;
    if(!r.has(h,0x20)) return {};
    const uint32_t format=r.u32(h+0x18);
    const uint8_t stride=r.u8(h+0x1c);
    const uint16_t count=r.u16(h+0x1e);
    const size_t data=h+r.u32(h+8);
    if(format>5 || stride<sizes[format] || !r.has(data,size_t(count)*stride)) return {};
    ColorArray a; a.id=r.u32(h+0x10);a.values.reserve(count);
    for(uint32_t i=0;i<count;++i) {
      const size_t p=data+size_t(i)*stride;
      const uint32_t v16=r.u16(p),v24=(uint32_t(r.u16(p))<<8)|r.u8(p+2);
      Color c{1,1,1,1};
      switch(format) {
      case 0: c={((v16>>11)&31)/31.f,((v16>>5)&63)/63.f,(v16&31)/31.f,1}; break;
      case 1: case 2: c={r.u8(p)/255.f,r.u8(p+1)/255.f,r.u8(p+2)/255.f,1}; break;
      case 3: c={(v16>>12)/15.f,((v16>>8)&15)/15.f,((v16>>4)&15)/15.f,(v16&15)/15.f}; break;
      case 4: c={(v24>>18)/63.f,((v24>>12)&63)/63.f,((v24>>6)&63)/63.f,(v24&63)/63.f}; break;
      default: c={r.u8(p)/255.f,r.u8(p+1)/255.f,r.u8(p+2)/255.f,r.u8(p+3)/255.f}; break;
      }
      a.values.push_back(c);
    }
    result.push_back(std::move(a));
  }
  return result;
}

// G3D texture SRT in Maya mode (every item material uses it), as a 2x3 matrix
// applied to (s,t,1). Other modes fall back to a plain scale-rotate-translate.
inline std::array<float,6> texture_srt(float sx,float sy,float degrees,float tx,float ty,uint32_t mode) {
  const float r=degrees*0.017453292519943295f,c=std::cos(r),s=std::sin(r);
  if(mode!=0) return {sx*c,-sy*s,tx,sx*s,sy*c,ty};
  return {sx*c,sy*-s,sx*(-0.5f*c-(0.5f*s-0.5f)-tx),
          sx*s,sy*c,sy*(-0.5f*c+(0.5f*s-0.5f)+ty)+1.0f};
}

struct Vertex { V3 position;V3 normal;Color color{1,1,1,1};std::array<V2,2> uv{}; };
struct Texture {
  std::string name;
  uint16_t width=0,height=0;
  uint32_t format=0,mips=1;
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> rgba;   // every mip level, level 0 first
};
struct TexGen {
  bool normal=false;           // env map from the view-space normal; else a UV set
  uint8_t uvSet=0;
  std::array<float,6> matrix{1,0,0,0,1,0};
};
struct Stage {
  uint8_t texMap=0,texCoord=0;
  bool textured=false,rasterized=true;
  uint32_t color=0x8fff0,alpha=0;   // BP 0xC0/0xC1 combiner words
  Color konst{1,1,1,1};             // the stage's resolved KSEL constant
};
struct Map { int texture=-1;uint8_t wrapS=1,wrapT=1; };
struct Material {
  uint8_t cull=2;                   // GX: 0 none, 1 front, 2 back, 3 all
  bool blend=false,subtract=false,depthWrite=true;
  uint8_t blendSrc=4,blendDst=5;
  uint32_t alphaCompare=0x3f0000;   // BP 0xF3
  uint32_t colorControl=0x700,alphaControl=0x700;
  Color materialColor{1,1,1,1};
  uint8_t stageCount=0;
  std::array<Stage,4> stages{};
  std::array<Color,4> registers{};  // PREV, C0, C1, C2
  std::array<TexGen,8> texGens{};
  std::array<Map,8> maps{};
};
struct Part {
  std::vector<Vertex> vertices;
  uint16_t material=0;
  bool translucent=false;
  // Billboards keep bone-local positions around origin and face the eye.
  bool billboard=false;
  V3 origin;
};
struct Model {
  std::vector<Part> parts;
  std::vector<Material> materials;
  std::vector<Texture> textures;
  V3 minimum{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
  V3 maximum{-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()};
  bool valid() const { return !parts.empty() && maximum.x>=minimum.x; }
};
inline void bounds(Model& model,V3 p) {
  model.minimum={std::min(model.minimum.x,p.x),std::min(model.minimum.y,p.y),std::min(model.minimum.z,p.z)};
  model.maximum={std::max(model.maximum.x,p.x),std::max(model.maximum.y,p.y),std::max(model.maximum.z,p.z)};
}

inline float signed11(uint32_t v) { int32_t x=int32_t(v&0x7ffu);if(x&0x400) x-=0x800;return float(x)/255.f; }
inline Color konst_value(const std::array<Color,4>& konst,uint32_t sel,bool alpha) {
  if(sel<8) { const float v=float(8-sel)/8.f;return {v,v,v,v}; }
  if(!alpha && sel>=0x0c && sel<=0x0f) { const auto& k=konst[sel-0x0c];return {k[0],k[1],k[2],k[3]}; }
  if(sel>=0x10 && sel<=0x1f) { const float v=konst[sel&3][(sel-0x10)>>2];return {v,v,v,v}; }
  return {0,0,0,0};
}
// Walks a G3D display list of BP (0x61), XF (0x10) and CP (0x08) loads.
template <typename Bp,typename Xf>
inline void walk_dl(Reader r,size_t at,size_t end,Bp&& bp,Xf&& xf) {
  end=std::min(end,r.size);
  while(at<end) {
    const uint8_t op=r.u8(at++);
    if(op==0) continue;
    if(op==0x61 && at+4<=end) { bp(r.u8(at),r.u32(at)&0xffffffu);at+=4; }
    else if(op==0x10 && at+4<=end) {
      const size_t count=size_t(r.u16(at))+1;const uint16_t address=r.u16(at+2);at+=4;
      for(size_t i=0;i<count && at+4<=end;++i,at+=4) xf(uint32_t(address+i),r.u32(at));
    } else if(op==0x08 && at+5<=end) at+=5;
    else return;
  }
}
inline int texture_index(Reader r,const std::vector<Entry>& fileTextures,const std::string& name,Model& model) {
  for(size_t i=0;i<model.textures.size();++i) if(model.textures[i].name==name) return int(i);
  const size_t tex=find(fileTextures,name);
  if(!tex || !r.has(tex,0x40) || r.u32(tex)!=0x54455830u) return -1;
  const size_t end=tex+r.u32(tex+4),start=tex+r.u32(tex+0x10);
  const uint16_t width=r.u16(tex+0x1c),height=r.u16(tex+0x1e);
  const uint32_t format=r.u32(tex+0x20),mips=std::clamp(r.u32(tex+0x24),1u,11u);
  if(end>r.size || start>=end || !width || !height || width>1024 || height>1024 ||
     format>14 || format==7 || (format>=8 && format<=13)) return -1;
  model.textures.push_back({name,width,height,format,mips,std::vector<uint8_t>(r.bytes+start,r.bytes+end),{}});
  return int(model.textures.size()-1);
}
inline bool parse_material(Reader r,size_t mat,const std::vector<Entry>& fileTextures,Model& model,Material& out) {
  if(!r.has(mat,0x418)) return false;
  const uint8_t genCount=std::min<uint8_t>(r.u8(mat+0x14),8);
  out.cull=uint8_t(r.u32(mat+0x18)&3u);
  const uint32_t layers=r.u32(mat+0x2c);
  const size_t layerAt=r.rel(mat,mat+0x30);
  if(layers>8 || (layers && (!layerAt || !r.has(layerAt,size_t(layers)*0x34)))) return false;
  for(uint32_t i=0;i<layers;++i) {
    const size_t layer=layerAt+size_t(i)*0x34;
    const uint32_t map=r.u32(layer+0x10);
    if(map>=8) return false;
    out.maps[map]={texture_index(r,fileTextures,r.str(r.rel(layer,layer)),model),
                   uint8_t(std::min(r.u32(layer+0x18),2u)),uint8_t(std::min(r.u32(layer+0x1c),2u))};
  }
  const uint32_t srtMode=r.u32(mat+0x1ac);
  for(uint8_t i=0;i<genCount;++i) {
    const size_t srt=mat+0x1b0+size_t(i)*20;
    out.texGens[i].matrix=texture_srt(r.f32(srt),r.f32(srt+4),r.f32(srt+8),r.f32(srt+12),r.f32(srt+16),srtMode);
    const uint8_t mapMode=r.u8(mat+0x250+size_t(i)*0x34+2);
    out.texGens[i].normal=mapMode!=0;
  }
  const size_t channel=mat+0x3f0;
  out.materialColor={r.u8(channel+4)/255.f,r.u8(channel+5)/255.f,r.u8(channel+6)/255.f,r.u8(channel+7)/255.f};
  out.colorControl=r.u32(channel+0xc);out.alphaControl=r.u32(channel+0x10);
  std::array<Color,4> konst{};
  std::array<uint8_t,4> kc{},ka{};
  kc.fill(0x0c);ka.fill(0x1c);
  const auto bp=[&](uint8_t reg,uint32_t v) {
    if(reg==0xf3) out.alphaCompare=v;
    else if(reg==0x40) out.depthWrite=(v>>4)&1u;
    else if(reg==0x41) {
      out.blend=v&1u;out.blendDst=(v>>5)&7u;out.blendSrc=(v>>8)&7u;out.subtract=(v>>11)&1u;
    } else if(reg>=0xe0 && reg<=0xe7) {
      const bool hi=reg&1u;
      auto& c=(v>>23)?konst[(reg-0xe0)>>1]:out.registers[(reg-0xe0)>>1];
      const float low=(v>>23)?float(v&0xffu)/255.f:signed11(v),high=(v>>23)?float((v>>12)&0xffu)/255.f:signed11(v>>12);
      if(hi) { c[2]=low;c[1]=high; } else { c[0]=low;c[3]=high; }
    } else if(reg>=0x28 && reg<=0x29) {
      for(uint32_t half=0;half<2;++half) {
        auto& s=out.stages[(reg-0x28)*2+half];
        const uint32_t x=v>>(12*half);
        s.texMap=x&7u;s.texCoord=(x>>3)&7u;s.textured=(x>>6)&1u;s.rasterized=((x>>7)&7u)==0;
      }
    } else if(reg>=0xc0 && reg<=0xc7) {
      auto& s=out.stages[(reg-0xc0)>>1];
      (reg&1u?s.alpha:s.color)=v;
    } else if(reg>=0xf6 && reg<=0xf7) {
      for(uint32_t half=0;half<2;++half) {
        const size_t stage=(reg-0xf6)*2+half;
        kc[stage]=(v>>(4+10*half))&31u;ka[stage]=(v>>(9+10*half))&31u;
      }
    }
  };
  const auto xf=[&](uint32_t address,uint32_t v) {
    if(address<0x1040 || address>=0x1040u+genCount) return;
    // TEXMTXINFO source row: 1 is the normal, 5..12 are UV sets.
    const uint32_t row=(v>>7)&31u;
    auto& gen=out.texGens[address-0x1040];
    if(row>=5 && row<=12 && !gen.normal) gen.uvSet=uint8_t(std::min(row-5,1u));
    else gen.normal=true;
  };
  const size_t tev=r.rel(mat,mat+0x28),dl=r.rel(mat,mat+0x3c);
  if(!tev || !dl || !r.has(tev,0x20)) return false;
  walk_dl(r,dl,dl+0x180,bp,xf);
  walk_dl(r,tev+0x20,tev+std::min<size_t>(r.u32(tev),0x400),bp,xf);
  out.stageCount=std::min<uint8_t>(r.u8(tev+0xc),4);
  for(uint8_t i=0;i<out.stageCount;++i) {
    auto& s=out.stages[i];
    const Color c=konst_value(konst,kc[i],false),a=konst_value(konst,ka[i],true);
    s.konst={c[0],c[1],c[2],a[3]};
    s.textured=s.textured && out.maps[s.texMap].texture>=0;
    if(s.texCoord>=genCount) s.texCoord=0;
  }
  return out.stageCount>0;
}

struct Bone { Matrix matrix;uint32_t billboard=0;bool valid=false; };
inline bool decode_shape(Reader r,size_t shape,const std::vector<Array>& positions,
                         const std::vector<Array>& normals,const std::vector<ColorArray>& colors,
                         const std::vector<Array>& uvs,const std::vector<Bone>& bones,
                         Part& part,Model& model) {
  if(!r.has(shape,0x68)) return false;
  const uint32_t lo=r.u32(shape+0xc),hi=r.u32(shape+0x10);
  const int desc[12]{int((lo>>9)&3),int((lo>>11)&3),int((lo>>13)&3),int((lo>>15)&3),
    int(hi&3),int((hi>>2)&3),int((hi>>4)&3),int((hi>>6)&3),int((hi>>8)&3),
    int((hi>>10)&3),int((hi>>12)&3),int((hi>>14)&3)};
  if(desc[0]<2) return false;
  const auto* pos=array_id(positions,r.u16(shape+0x48));
  const auto* nrm=desc[1]?array_id(normals,r.u16(shape+0x4a)):nullptr;
  const auto* clr=desc[2]?array_id(colors,r.u16(shape+0x4c)):nullptr;
  const Array* uv[2]{desc[4]?array_id(uvs,r.u16(shape+0x50)):nullptr,
                     desc[5]?array_id(uvs,r.u16(shape+0x52)):nullptr};
  if(!pos || (desc[1] && !nrm) || (desc[2] && !clr) || (desc[4] && !uv[0]) || (desc[5] && !uv[1])) return false;
  size_t matrix_bytes=0;
  for(uint32_t mask=lo&511;mask;mask>>=1) matrix_bytes+=mask&1u;
  size_t stride=matrix_bytes;
  for(int d:desc) {
    if(d==1) return false;
    stride+=d==2?1:d==3?2:0;
  }
  const size_t begin=shape+0x24+r.u32(shape+0x2c),length=r.u32(shape+0x28);
  if(stride<2 || length>65536 || !r.has(begin,length)) return false;
  const size_t end=begin+length;
  // Matrix IDs, not bone indices: a single-bound shape names its own matrix;
  // one with PNMTXIDX loads a palette of them. Envelope IDs have no bone and are
  // identity in the bind pose (their vertices are already in model space).
  const int32_t single=int32_t(r.u32(shape+8));
  const auto resolve=[&](uint32_t id) -> const Bone* { return id<bones.size() && bones[id].valid?&bones[id]:nullptr; };
  const Bone identity{};
  const Bone* singleBone=single>=0?resolve(uint32_t(single)):nullptr;
  if(!(lo&1u) && singleBone && singleBone->billboard) {
    part.billboard=true;
    const auto& m=singleBone->matrix.v;
    part.origin={m[3],m[7],m[11]};
  }
  std::array<uint16_t,10> palette{};
  palette.fill(uint16_t(std::max(single,0)));
  size_t at=begin;
  while(at<end) {
    const uint8_t op=r.u8(at++);
    if(op==0) continue;
    if(op==0x20 || op==0x28 || op==0x30 || op==0x38) {
      if(at+4>end) return false;
      const uint32_t slot=(r.u16(at+2)&0xfffu)/12u;
      if(op==0x20 && slot<palette.size()) palette[slot]=r.u16(at);
      at+=4;continue;
    }
    const uint8_t primitive=op&0xf8;
    if((primitive!=0x80 && primitive!=0x90 && primitive!=0x98 && primitive!=0xa0) || at+2>end) return false;
    const uint16_t count=r.u16(at);at+=2;
    if(count>8192 || size_t(count)*stride>end-at) return false;
    std::vector<Vertex> source;
    source.reserve(count);
    for(uint16_t i=0;i<count;++i) {
      const Bone* bone=singleBone;
      if(lo&1u) {
        const uint32_t slot=r.u8(at)/3u;
        bone=slot<palette.size()?resolve(palette[slot]):nullptr;
      }
      at+=matrix_bytes;
      uint16_t indices[12]{};
      for(int a=0;a<12;++a) {
        if(desc[a]==2) indices[a]=r.u8(at++);
        else if(desc[a]==3) { indices[a]=r.u16(at);at+=2; }
      }
      if(indices[0]>=pos->values.size() || (nrm && indices[1]>=nrm->values.size()) ||
         (clr && indices[2]>=clr->values.size()) || (uv[0] && indices[4]>=uv[0]->values.size()) ||
         (uv[1] && indices[5]>=uv[1]->values.size())) return false;
      const Matrix& transform=(bone?bone:&identity)->matrix;
      Vertex v;
      v.normal=nrm?nrm->values[indices[1]]:V3{0,1,0};
      if(part.billboard) {
        // Keep the bone's scale; the renderer replaces its rotation.
        const auto& m=transform.v;
        const V3 p=pos->values[indices[0]];
        v.position={p.x*std::hypot(m[0],m[4],m[8]),p.y*std::hypot(m[1],m[5],m[9]),p.z*std::hypot(m[2],m[6],m[10])};
      } else {
        v.position=point(transform,pos->values[indices[0]]);
        v.normal=direction(transform,v.normal);
      }
      if(clr) v.color=clr->values[indices[2]];
      for(int set=0;set<2;++set) if(uv[set]) {
        const V3 t=uv[set]->values[indices[4+set]];
        v.uv[set]={t.x,t.y};
      }
      source.push_back(v);
    }
    const auto tri=[&](uint16_t a,uint16_t b,uint16_t c) {
      if(a==b || b==c || a==c) return;
      for(uint16_t i:{a,b,c}) part.vertices.push_back(source[i]);
    };
    if(primitive==0x90) { for(uint16_t i=0;i+2<count;i+=3) tri(i,i+1,i+2); }
    else if(primitive==0x80) { for(uint16_t i=0;i+3<count;i+=4) {tri(i,i+1,i+2);tri(i,i+2,i+3);} }
    else if(primitive==0x98) { for(uint16_t i=2;i<count;++i) {
      if(i&1) tri(i-1,i-2,i);else tri(i-2,i-1,i);
    } }
    else { for(uint16_t i=2;i<count;++i) tri(0,i-1,i); }
  }
  for(const auto& v:part.vertices) {
    if(!part.billboard) { bounds(model,v.position);continue; }
    const float radius=std::sqrt(v.position.x*v.position.x+v.position.y*v.position.y+v.position.z*v.position.z);
    bounds(model,{part.origin.x-radius,part.origin.y-radius,part.origin.z-radius});
    bounds(model,{part.origin.x+radius,part.origin.y+radius,part.origin.z+radius});
  }
  return true;
}

inline Model parse_model(Reader r,const std::string& model_name) {
  Model result;
  if(!r.has(0,16) || r.u32(0)!=0x62726573u) return result;
  const auto groups=dict(r,r.u16(12)+8);
  const size_t model_dict=find(groups,"3DModels(NW4R)");
  const size_t texture_dict=find(groups,"Textures(NW4R)");
  if(!model_dict || !texture_dict) return result;
  const size_t m=find(dict(r,model_dict),model_name);
  if(!m || !r.has(m,0x40) || r.u32(m)!=0x4d444c30u || r.u32(m+8)!=11) return result;
  const size_t model_end=m+r.u32(m+4);
  if(model_end>r.size || model_end<=m) return result;
  const auto joint_entries=dict(r,m+r.u32(m+0x14));
  const auto material_entries=dict(r,m+r.u32(m+0x30));
  const auto shape_entries=dict(r,m+r.u32(m+0x38));
  const auto file_textures=dict(r,texture_dict);
  const auto draw_entries=dict(r,m+r.u32(m+0x10));
  if(joint_entries.empty() || material_entries.empty() || shape_entries.empty() ||
     joint_entries.size()>256 || shape_entries.size()>256 || material_entries.size()>64) return result;
  // Each bone stores its bind-pose model matrix; index it by matrix ID, the
  // number shapes and palettes use (NodeTree parents are matrix IDs too).
  std::vector<Bone> bones;
  for(const auto& e:joint_entries) {
    if(!r.has(e.at,0xa0)) return {};
    const uint32_t id=r.u32(e.at+0x10);
    if(id>=1024) return {};
    if(id>=bones.size()) bones.resize(id+1);
    auto& bone=bones[id];
    for(int i=0;i<12;++i) bone.matrix.v[i]=r.f32(e.at+0x70+size_t(i)*4);
    for(float f:bone.matrix.v) if(!std::isfinite(f)) return {};
    bone.billboard=r.u32(e.at+0x18);bone.valid=true;
  }
  const auto positions=arrays(r,m,r.u32(m+0x18),ArrayKind::Position);
  const auto normals=arrays(r,m,r.u32(m+0x1c),ArrayKind::Normal);
  const auto colors=color_arrays(r,m,r.u32(m+0x20));
  const auto uvs=arrays(r,m,r.u32(m+0x24),ArrayKind::UV);
  if(positions.empty()) return {};
  struct Draw { uint16_t mat,shape;bool xlu; };
  std::vector<Draw> draws;
  for(const char* list:{"DrawOpa","DrawXlu"}) {
    const size_t start=find(draw_entries,list);
    if(!start) continue;
    size_t at=start;
    for(size_t guard=0;guard<4096 && at<model_end;++guard) {
      const uint8_t op=r.u8(at);
      if(op==1) break;
      if(op!=4 || !r.has(at,8)) return {};
      draws.push_back({r.u16(at+1),r.u16(at+3),std::strcmp(list,"DrawXlu")==0});
      at+=8;
    }
  }
  if(draws.empty() || draws.size()>512) return {};
  std::vector<int> materialSlot(material_entries.size(),-1);
  for(const auto& draw:draws) {
    if(draw.mat>=material_entries.size() || draw.shape>=shape_entries.size()) return {};
    if(materialSlot[draw.mat]<0) {
      Material material;
      if(!parse_material(r,material_entries[draw.mat].at,file_textures,result,material)) return {};
      materialSlot[draw.mat]=int(result.materials.size());
      result.materials.push_back(material);
    }
    Part part;part.translucent=draw.xlu;part.material=uint16_t(materialSlot[draw.mat]);
    if(!decode_shape(r,shape_entries[draw.shape].at,positions,normals,colors,uvs,bones,part,result)) return {};
    if(!part.vertices.empty()) result.parts.push_back(std::move(part));
  }
  return result.valid()?result:Model{};
}

inline constexpr std::array<const char*,15> names{"koura_green","koura_red","banana","itemBoxNiseRtpa",
  "kinoko","bomb","togezo_koura","thunder","star","kinoko_p","big_kinoko","gesso",
  "pow_bloc","kumo","item_killer"};
inline int model_index(uint8_t id) {
  constexpr int map[19]{0,1,2,3,4,4,5,6,7,8,9,10,11,12,13,14,0,1,2};
  return id<19?map[id]:-1;
}
struct Archive { std::array<Model,15> models;uint32_t loaded=0; };
inline Archive parse_archive(const void* bytes,size_t size) {
  Archive archive;
  if(!bytes || !size || size>32u*1024u*1024u) return archive;
  const auto unpacked=yaz0({static_cast<const uint8_t*>(bytes),size});
  Reader r{unpacked.data(),unpacked.size()};
  if(!r.has(0,0x20) || r.u32(0)!=0x55aa382du) return archive;
  const size_t root=r.u32(4);
  if(!r.has(root,12)) return archive;
  const uint32_t count=r.u32(root+8);
  if(count>8192 || count<2 || !r.has(root,size_t(count)*12)) return archive;
  const size_t names_base=root+size_t(count)*12;
  for(uint32_t i=1;i<count;++i) {
    const size_t e=root+size_t(i)*12,tag=r.u32(e);
    if(tag>>24) continue;
    const std::string filename=r.str(names_base+(tag&0xffffff));
    for(size_t item=0;item<names.size();++item) {
      if(filename!=std::string(names[item])+".brres") continue;
      const size_t at=r.u32(e+4),length=r.u32(e+8);
      if(!r.has(at,length) || length>4u*1024u*1024u) break;
      const Reader file{r.bytes+at,length};
      const std::string model_name=item==3?"itemBoxNise":names[item];
      archive.models[item]=parse_model(file,model_name);
      if(archive.models[item].valid()) ++archive.loaded;
      break;
    }
  }
  return archive;
}

} // namespace aurora::gfx::cockpit_item::data
