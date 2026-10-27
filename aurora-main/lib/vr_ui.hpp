// Native stereo controller tutorial. Placement inspired by CircuitLord's BigWalkVR.
// Included in aurora.cpp after the WebGPU helpers are available.
namespace vrui {
using V=std::array<float,3>;
struct Vertex { float clip[4],uv[2],color[3],textured; float origin[3]{}; };
std::mutex mutex;
AuroraVRUiGuide guide{};
std::array<std::vector<AuroraVRControllerVertex>,2> models;
std::vector<AuroraVRControllerVertex> workshopModel;
bool workshopActive=false;
std::array<bool,2> workshopCaptured{};
constexpr uint32_t workshopMaterial=0xfffffff0u;
struct ControllerTexture {
  uint32_t width=0,height=0;
  std::vector<uint8_t> pixels;
  wgpu::Texture texture;
  wgpu::BindGroup binding;
};
std::unordered_map<uint32_t,ControllerTexture> controllerTextures;
std::array<std::array<V,5>,2> anchors{{
  {{{-.015f,.018f,-.034f},{.013f,.018f,-.031f},{.021f,.035f,-.031f},{0,.018f,-.05f},{.025f,-.045f,.018f}}},
  {{{.015f,.018f,-.034f},{-.013f,.018f,-.031f},{-.021f,.035f,-.031f},{0,.018f,-.05f},{-.025f,-.045f,.018f}}}
}};
wgpu::RenderPipeline pipeline;
wgpu::TextureFormat format{};
wgpu::Texture panorama;
wgpu::TextureView panoramaView;
wgpu::BindGroup panoramaBinding;
std::atomic<int> requestedQuality{3};
std::atomic<float> panelAspect{16.f/9.f};
int activeQuality=-1;
V point(const float* m,V p) { return {m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3],m[4]*p[0]+m[5]*p[1]+m[6]*p[2]+m[7],m[8]*p[0]+m[9]*p[1]+m[10]*p[2]+m[11]}; }
// The menus' environment layer (the Frame): one eye's room around the screen drawn into its part of a
// packed two-eye target. The screen itself is a separate quad layer, so its area stays transparent.
// Lane AF (KARTFRAME_MENU_CONTROLLERS=projected): `screen` set, the screen's area shows that image's `screenUv`
// rectangle (u0, v0, u1, v1) instead, and controller triangles behind the screen are left out.
struct Environment { wgpu::TextureView view; uint32_t x=0,width=0,height=0; bool clear=false;
                     wgpu::BindGroup screen; std::array<float,4> screenUv{}; };
// Lane AF (KARTFRAME_MENU_CONTROLLERS=eyes): one eye's controllers and rays drawn into its own copy of the screen's
// image, in the viewport (x, y, width, height) its picture fills there: each point is projected from the eye onto
// the screen's plane, and only what lies between the eye and the screen is drawn (the far plane is the screen).
// Shown on that eye's screen quad, every pixel is where that eye sees the point, so it is stereo-correct.
struct ScreenComposite { wgpu::TextureView view; float x=0,y=0,width=0,height=0; };
void render(wgpu::CommandEncoder& encoder,const webgpu::PresentSource& source,const AuroraStereoFrame& frame,
            const AuroraVRUiGuide& layout,uint32_t eye,const Environment* env=nullptr,const ScreenComposite* comp=nullptr) {
  const auto& output=g_stereoEyeTargets[env?1:(comp?0:eye)].output();
  const wgpu::TextureView targetView=env?env->view:comp?comp->view:output.view;
  const auto setViewport=[&](const wgpu::RenderPassEncoder& pass) {
    if(env) pass.SetViewport(float(env->x),0,float(env->width),float(env->height),0,1);
    if(comp) pass.SetViewport(comp->x,comp->y,comp->width,comp->height,0,1);
  };
  panelAspect.store(float(source.size.width)/std::max(1u,source.size.height),std::memory_order_relaxed);
  // Apply changes between stereo pairs, so both eyes use the same quality.
  const int quality=(eye==0 || activeQuality<0)?requestedQuality.load(std::memory_order_relaxed):activeQuality;
  if(!pipeline || format!=output.format || activeQuality!=quality) {
    activeQuality=quality;
    wgpu::ShaderSourceWGSL code{};code.code=R"(
      @group(0) @binding(0) var samp: sampler;
      @group(0) @binding(1) var tex: texture_2d<f32>;
      struct O { @builtin(position) p: vec4f, @location(0) uv: vec2f, @location(1) c: vec3f, @location(2) t: f32, @location(3) origin:vec3f };
      @vertex fn vs(@location(0) p:vec4f,@location(1) uv:vec2f,@location(2) c:vec3f,@location(3) t:f32,@location(4) origin:vec3f)->O { var o:O;o.p=p;o.uv=uv;o.c=c;o.t=t;o.origin=origin;return o; }
      // Dielectric by @Xor, adapted from the user-supplied FragCoord screenshot.
      // March the same world-space field from each eye, including translation.
      fn dielectric(ray:vec3f,origin:vec3f,time:f32)->vec4f {
        var z=0.3; var light=vec4f(0);
        for(var step=0;step<28;step++) {
          var p=origin+z*ray; var t=p; var d=3.0;
          for(var octave=0;octave<6;octave++) {
            p+=sin(p.zxy*d-vec3f(time))/d; d+=d;
          }
          z+=max(abs(1.0-length(p.xy))/3.0,0.001);
          light+=0.002*(1.0+cos(9.0/z-time))/max(abs(vec4f(t.x,t.y,-t.z,-t.z)-vec4f(1)),vec4f(0.004));
        }
        return vec4f(tanh(light.xyz+vec3f(0.1*length(light)))*0.65,1);
      }
      @fragment fn fs(i:O)->@location(0) vec4f {
        if(i.t > 2.5) { return vec4f(0.0); }
        if(i.t < -1.5) {
          return dielectric(normalize(i.c),i.origin,-i.t-2.0);
        }
        if(i.t < -0.5) {
          return textureSampleLevel(tex,samp,i.uv,0);
        }
        if(i.t > 1.5) {
          let material=textureSampleLevel(tex,samp,i.uv,0);
          if(material.a < 0.5) { discard; }
          return vec4f(material.rgb*i.c,1);
        }
        return mix(vec4f(i.c,1),textureSampleLevel(tex,samp,i.uv,0),i.t);
      }
    )";
    wgpu::ShaderModuleDescriptor md{};md.nextInChain=&code;auto shader=g_device.CreateShaderModule(&md);
    const wgpu::VertexAttribute attrs[]={{.format=wgpu::VertexFormat::Float32x4,.offset=0,.shaderLocation=0},
      {.format=wgpu::VertexFormat::Float32x2,.offset=16,.shaderLocation=1},
      {.format=wgpu::VertexFormat::Float32x3,.offset=24,.shaderLocation=2},
      {.format=wgpu::VertexFormat::Float32,.offset=36,.shaderLocation=3},
      {.format=wgpu::VertexFormat::Float32x3,.offset=40,.shaderLocation=4}};
    const wgpu::VertexBufferLayout vb{.arrayStride=sizeof(Vertex),.attributeCount=5,.attributes=attrs};
    const auto bgl=webgpu::g_CopyPipeline.GetBindGroupLayout(0);
    const wgpu::PipelineLayoutDescriptor ld{.bindGroupLayoutCount=1,.bindGroupLayouts=&bgl};
    const wgpu::ColorTargetState ct{.format=output.format};
    const wgpu::FragmentState fs{.module=shader,.entryPoint="fs",.targetCount=1,.targets=&ct};
    wgpu::RenderPipelineDescriptor pd{};pd.layout=g_device.CreatePipelineLayout(&ld);
    pd.vertex={.module=shader,.entryPoint="vs",.bufferCount=1,.buffers=&vb};pd.fragment=&fs;
    pipeline=g_device.CreateRenderPipeline(&pd);format=output.format;
    // Resolution covers one eye's visible field, not an entire 360-degree map.
    const uint32_t sizes[]{1,640,960,1440};
    const wgpu::TextureDescriptor td{.label="Dielectric stereo field",.usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::TextureBinding,
      .dimension=wgpu::TextureDimension::e2D,.size={sizes[quality],sizes[quality],1},.format=output.format,.mipLevelCount=1,.sampleCount=1};
    panorama=g_device.CreateTexture(&td);panoramaView=panorama.CreateView();
    const wgpu::SamplerDescriptor sd{.addressModeU=wgpu::AddressMode::Repeat,.addressModeV=wgpu::AddressMode::ClampToEdge,
      .magFilter=wgpu::FilterMode::Linear,.minFilter=wgpu::FilterMode::Linear};
    panoramaBinding=webgpu::create_copy_bind_group(panoramaView,g_device.CreateSampler(&sd));
  }
  const auto drawVertices=[&](wgpu::TextureView view,wgpu::BindGroup binding,const Vertex* data,size_t count,wgpu::LoadOp load) {
    const wgpu::BufferDescriptor bd{.usage=wgpu::BufferUsage::Vertex|wgpu::BufferUsage::CopyDst,.size=count*sizeof(Vertex)};
    auto buffer=g_device.CreateBuffer(&bd);g_queue.WriteBuffer(buffer,0,data,count*sizeof(Vertex));
    const wgpu::RenderPassColorAttachment ca{.view=view,.loadOp=load,.storeOp=wgpu::StoreOp::Store};
    const wgpu::RenderPassDescriptor rd{.label="VR menu environment",.colorAttachmentCount=1,.colorAttachments=&ca};
    auto pass=encoder.BeginRenderPass(&rd);pass.SetPipeline(pipeline);pass.SetBindGroup(0,binding);
    if(view.Get()==targetView.Get()) setViewport(pass);
    pass.SetVertexBuffer(0,buffer);pass.Draw(count);pass.End();
  };
  static const auto start=std::chrono::steady_clock::now();
  static float time=0;
  if(eye==0 && !comp) time=std::chrono::duration<float>(std::chrono::steady_clock::now()-start).count()*0.25f;
  Vertex sky[3]{};
  const float corners[3][2]{{-1,-1},{3,-1},{-1,3}};
  for(int i=0;i<3;++i) {
    const auto* p=frame.eyes[eye].projection;const auto* m=frame.ui.eyeFromPanel[eye];
    const V ray{(corners[i][0]+p[2])/p[0],(corners[i][1]+p[6])/p[5],-1};
    sky[i]={{corners[i][0],corners[i][1],0,1},{(corners[i][0]+1)*.5f,(1-corners[i][1])*.5f},
      {m[0]*ray[0]+m[4]*ray[1]+m[8]*ray[2],m[1]*ray[0]+m[5]*ray[1]+m[9]*ray[2],m[2]*ray[0]+m[6]*ray[1]+m[10]*ray[2]},-2-time,
      {-(m[0]*m[3]+m[4]*m[7]+m[8]*m[11])/6,
       -(m[1]*m[3]+m[5]*m[7]+m[9]*m[11])/6,
       -(m[2]*m[3]+m[6]*m[7]+m[10]*m[11])/6}};
  }
  if(comp) {
  } else if(quality && !env) {
    drawVertices(panoramaView,source.bindGroup,sky,3,wgpu::LoadOp::Clear);
    for(auto& v:sky) v.textured=-1;
    drawVertices(targetView,panoramaBinding,sky,3,wgpu::LoadOp::Clear);
  } else {
    for(auto& v:sky) { v.textured=0;v.color[0]=.012f;v.color[1]=.018f;v.color[2]=.027f; }
    drawVertices(targetView,source.bindGroup,sky,3,!env || env->clear?wgpu::LoadOp::Clear:wgpu::LoadOp::Load);
  }
  std::vector<Vertex> vertices;
  bool workshop=false;
  std::array<bool,2> captured{};
  {std::lock_guard lock(mutex);workshop=workshopActive;captured=workshopCaptured;}
  struct MaterialRange { uint32_t first,count; wgpu::BindGroup binding; };
  std::vector<MaterialRange> materialRanges;
  const float* fromPanel=frame.ui.eyeFromPanel[eye];
  // Eyes mode: the eye in panel space, and its distance to the screen's plane (z = -distance) along the normal.
  const V eyeInPanel{-(fromPanel[0]*fromPanel[3]+fromPanel[4]*fromPanel[7]+fromPanel[8]*fromPanel[11]),
                     -(fromPanel[1]*fromPanel[3]+fromPanel[5]*fromPanel[7]+fromPanel[9]*fromPanel[11]),
                     -(fromPanel[2]*fromPanel[3]+fromPanel[6]*fromPanel[7]+fromPanel[10]*fromPanel[11])};
  const float toScreen=eyeInPanel[2]+frame.ui.distance,compNear=.01f;
  if(comp && !(toScreen>2*compNear)) return;
  const auto vertex=[&](V p,float u,float v,V c,float t) {
    if(comp) {
      // Off-axis projection onto the screen: x, y in the screen's half extents, w the depth along the screen's
      // normal, z from the near plane (0) to the screen's plane (1).
      const float dz=eyeInPanel[2]-p[2];
      const float hw=std::max(.25f,frame.ui.width)*.5f,hh=hw*float(source.size.height)/std::max(1u,source.size.width);
      return Vertex{{(eyeInPanel[0]*dz+(p[0]-eyeInPanel[0])*toScreen)/hw,(eyeInPanel[1]*dz+(p[1]-eyeInPanel[1])*toScreen)/hh,
                     (dz-compNear)*toScreen/(toScreen-compNear),dz},{u,v},{c[0],c[1],c[2]},t};
    }
    const auto q=point(fromPanel,p);const auto* proj=frame.eyes[eye].projection;
    return Vertex{{proj[0]*q[0]+proj[2]*q[2],proj[5]*q[1]+proj[6]*q[2],-q[2]-.01f,-q[2]},{u,v},{c[0],c[1],c[2]},t};
  };
  const auto quad=[&](V center,float w,float h,float u0,float v0,float u1,float v1) {
    const Vertex a=vertex({center[0]-w/2,center[1]+h/2,center[2]},u0,v0,{1,1,1},1);
    const Vertex b=vertex({center[0]+w/2,center[1]+h/2,center[2]},u1,v0,{1,1,1},1);
    const Vertex c=vertex({center[0]+w/2,center[1]-h/2,center[2]},u1,v1,{1,1,1},1);
    const Vertex d=vertex({center[0]-w/2,center[1]-h/2,center[2]},u0,v1,{1,1,1},1);
    vertices.insert(vertices.end(),{a,b,c,a,c,d});
  };
  const float width=std::max(.25f,frame.ui.width),height=width*float(source.size.height)/std::max(1u,source.size.width);
  const auto panelSlice=[&](float top,float bottom) { quad({0,(.5f-(top+bottom)/2)*height,-frame.ui.distance},width,(bottom-top)*height,0,top,1,bottom); };
  if(comp) {
  } else if(env && env->screen) {
    // Projected: the screen's picture in the environment (no quad layer).
    const auto& uv=env->screenUv;
    quad({0,0,-frame.ui.distance},width,height,uv[0],uv[1],uv[2],uv[3]);
  } else if(env) {
    // The screen's own quad layer shows the picture: leave its area transparent black.
    quad({0,0,-frame.ui.distance},width,height,0,0,1,1);
    for(size_t i=vertices.size()-6;i<vertices.size();++i) vertices[i].textured=3;
  } else if(!layout.active) panelSlice(0,1);
  else {
    panelSlice(0,layout.headerEnd);panelSlice(layout.footerStart,1);
  }
  for(int hand=0;hand<2;++hand) if(frame.ui.pointerTracked[hand]) {
    const auto* ray=frame.ui.pointerRay[hand];
    if(ray[5]>=-.001f) continue;
    const float distance=(-frame.ui.distance-ray[2])/ray[5];
    if(distance<=0 || distance>10) continue;
    const V end{ray[0]+ray[3]*distance,ray[1]+ray[4]*distance,-frame.ui.distance+.003f};
    if(std::abs(end[0])>width*.5f || std::abs(end[1])>height*.5f) continue;
    const V color=hand?V{.96f,.74f,.31f}:V{.15f,.88f,.82f};
    const auto a=vertex({ray[0]-.001f,ray[1],ray[2]},0,0,color,0);
    const auto b=vertex({ray[0]+.001f,ray[1],ray[2]},0,0,color,0);
    const auto c=vertex({end[0]-.001f,end[1],end[2]},0,0,color,0);
    const auto d=vertex({end[0]+.001f,end[1],end[2]},0,0,color,0);
    vertices.insert(vertices.end(),{a,b,c,b,d,c});
    // A small target marker remains readable against bright game menus (the screen's own mark in eyes mode).
    if(!comp) for(int segment=0;segment<16;++segment) {
      const float angle=segment*6.2831853f/16,next=(segment+1)*6.2831853f/16;
      vertices.push_back(vertex(end,0,0,{1,1,1},0));
      vertices.push_back(vertex({end[0]+.006f*std::cos(angle),end[1]+.006f*std::sin(angle),end[2]},0,0,color,0));
      vertices.push_back(vertex({end[0]+.006f*std::cos(next),end[1]+.006f*std::sin(next),end[2]},0,0,color,0));
    }
  }
  {
    // Sort solid controller triangles back-to-front. Each eye gets its own
    // perspective and ordering; no scene depth exists behind the tutorial.
    struct Triangle { std::array<Vertex,3> v;float depth;uint32_t material; };
    std::vector<Triangle> triangles;
    triangles.reserve(9216);
    {
      std::lock_guard lock(mutex);
      for(auto& [id,material]:controllerTextures) if(!material.pixels.empty()) {
        const wgpu::Extent3D size{material.width,material.height,1};
        const wgpu::TextureDescriptor td{.label="SteamVR controller material",.usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::CopyDst,
          .dimension=wgpu::TextureDimension::e2D,.size=size,.format=wgpu::TextureFormat::RGBA8Unorm,.mipLevelCount=1,.sampleCount=1};
        material.texture=g_device.CreateTexture(&td);
        const wgpu::TexelCopyTextureInfo dst{.texture=material.texture};
        const wgpu::TexelCopyBufferLayout dataLayout{.bytesPerRow=4*material.width,.rowsPerImage=material.height};
        g_queue.WriteTexture(&dst,material.pixels.data(),material.pixels.size(),&dataLayout,&size);
        const wgpu::SamplerDescriptor sd{.addressModeU=wgpu::AddressMode::Repeat,.addressModeV=wgpu::AddressMode::Repeat,
          .magFilter=wgpu::FilterMode::Linear,.minFilter=wgpu::FilterMode::Linear};
        material.binding=webgpu::create_copy_bind_group(material.texture.CreateView(),g_device.CreateSampler(&sd));
        material.pixels.clear();
      }
      if(workshop) for(size_t i=0;i+2<workshopModel.size();i+=3) {
        Triangle tri{};tri.material=workshopMaterial;
        for(int j=0;j<3;++j) {
          const auto& v=workshopModel[i+j];
          tri.v[j]=vertex({v.position[0],v.position[1],v.position[2]},v.uv[0],v.uv[1],{1,1,1},2);
          tri.depth+=tri.v[j].clip[3];
        }
        triangles.push_back(tri);
      }
      for(int hand=0;hand<2;++hand) if(!workshop && frame.ui.tracked[hand]) {
        const auto& model=models[hand];
        for(size_t i=0;i+2<model.size();i+=3) {
          Triangle tri{};
          tri.material=model[i].material;
          float panelZ=0;
          for(int j=0;j<3;++j) {
            const auto& v=model[i+j];const auto p=point(frame.ui.panelFromGrip[hand],{v.position[0],v.position[1],v.position[2]});
            tri.v[j]=vertex(p,v.uv[0],v.uv[1],{v.color[0],v.color[1],v.color[2]},tri.material?2:0);tri.depth+=tri.v[j].clip[3];
            panelZ+=p[2];
          }
          // Projected: no depth buffer, so a controller pushed through the screen must not show in front of it.
          if(env && env->screen && panelZ<-3*frame.ui.distance) continue;
          triangles.push_back(tri);
        }
      }
    }
    // Sort small keys, not the triangles themselves: the controller models are ~9000
    // triangles per eye and moving them dominated the frame worker at 120 Hz. Back to front
    // in 5 mm slices, by material within a slice: a strict depth order switched material
    // (a draw and a bind group each) almost every triangle.
    struct Key { int32_t slice; uint32_t material; uint32_t index; };
    std::vector<Key> order(triangles.size());
    for(uint32_t i=0;i<order.size();++i)
      order[i]={static_cast<int32_t>(std::floor(triangles[i].depth*(1.f/3.f)/.005f)),triangles[i].material,i};
    std::sort(order.begin(),order.end(),[](const Key& a,const Key& b){
      return a.slice!=b.slice?a.slice>b.slice:a.material<b.material;});
    materialRanges.push_back({0,static_cast<uint32_t>(vertices.size()),env && env->screen?env->screen:source.bindGroup});
    vertices.reserve(vertices.size()+triangles.size()*3+512);
    {
      std::lock_guard lock(mutex);
      for(const auto& key:order) {
        const auto& t=triangles[key.index];
        const auto found=controllerTextures.find(t.material);
        auto binding=found!=controllerTextures.end() && found->second.binding?found->second.binding:source.bindGroup;
        if(materialRanges.back().binding.Get()==binding.Get()) materialRanges.back().count+=3;
        else materialRanges.push_back({static_cast<uint32_t>(vertices.size()),3,binding});
        vertices.insert(vertices.end(),t.v.begin(),t.v.end());
      }
    }
    const auto labelStart=static_cast<uint32_t>(vertices.size());
    std::array<std::array<V,5>,2> buttonPositions;
    { std::lock_guard lock(mutex); buttonPositions=anchors; }
    if(workshop) for(int hand=0;hand<2;++hand) if(frame.ui.tracked[hand]) {
      const auto* m=frame.ui.panelFromGrip[hand];
      const V color=captured[hand]?V{.12f,1.f,.3f}:V{.08f,.5f,1.f};
      for(int axis=0;axis<3;++axis) {
        V a{},b{};a[axis]=-.025f;b[axis]=.025f;a=point(m,a);b=point(m,b);
        const int across=axis==0?1:0;
        auto a0=a,a1=a,b0=b,b1=b;a0[across]-=.0015f;a1[across]+=.0015f;b0[across]-=.0015f;b1[across]+=.0015f;
        vertices.insert(vertices.end(),{vertex(a0,0,0,color,0),vertex(a1,0,0,color,0),vertex(b0,0,0,color,0),
          vertex(a1,0,0,color,0),vertex(b1,0,0,color,0),vertex(b0,0,0,color,0)});
      }
    }
    if(layout.active && !workshop) for(int hand=0;hand<2;++hand) if(frame.ui.tracked[hand]) for(int row=0;row<5;++row) {
      const auto* m=frame.ui.panelFromGrip[hand];
      const V c{m[3]+(hand?1.f:-1.f)*.23f,m[7]+.17f-row*.078f,m[11]-.08f};
      const auto* uv=layout.labels[hand*5+row];
      const auto target=point(m,buttonPositions[hand][row]);
      const V end{c[0]+(hand?-.13f:.13f),c[1],c[2]};
      const V color=hand?V{.96f,.74f,.31f}:V{.15f,.88f,.82f};
      const Vertex a=vertex({target[0],target[1]-.0015f,target[2]},0,0,color,0);
      const Vertex b=vertex({target[0],target[1]+.0015f,target[2]},0,0,color,0);
      const Vertex d=vertex({end[0],end[1]-.0015f,end[2]},0,0,color,0);
      const Vertex e=vertex({end[0],end[1]+.0015f,end[2]},0,0,color,0);
      vertices.insert(vertices.end(),{a,b,d,b,e,d});
      quad(c,.26f,.055f,uv[0],uv[1],uv[2],uv[3]);
    }
    materialRanges.push_back({labelStart,static_cast<uint32_t>(vertices.size())-labelStart,source.bindGroup});
  }
  const wgpu::BufferDescriptor bd{.label="Anchored VR UI vertices",.usage=wgpu::BufferUsage::Vertex|wgpu::BufferUsage::CopyDst,.size=std::max(size_t(4),vertices.size()*sizeof(Vertex))};
  auto buffer=g_device.CreateBuffer(&bd);if(!vertices.empty()) g_queue.WriteBuffer(buffer,0,vertices.data(),vertices.size()*sizeof(Vertex));
  const wgpu::RenderPassColorAttachment ca{.view=targetView,.loadOp=wgpu::LoadOp::Load,.storeOp=wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor rd{.label="Anchored VR UI stereo",.colorAttachmentCount=1,.colorAttachments=&ca};
  auto pass=encoder.BeginRenderPass(&rd);pass.SetPipeline(pipeline);setViewport(pass);pass.SetVertexBuffer(0,buffer);
  for(const auto& range:materialRanges) if(range.count) {
    pass.SetBindGroup(0,range.binding);pass.Draw(range.count,1,range.first);
  }
  pass.End();
}
// The pointer's target marks drawn into the menus' screen image itself (eye target 0, the screen quad's
// picture): a mark on the quad's own surface is visible and stereo-correct whatever order the runtime
// composites the layers in (SteamVR on the Frame hid the environment layer's mark under the quad, H4.md).
// Call after render() built the pipeline for this format; `source` is the picture placed in the image. Lane AF:
// into the screen image's columns [x0, x0 + imageWidth) (one eye's half of the packed pair in eyes mode; 0 = all).
void screen_marks(wgpu::CommandEncoder& encoder,const webgpu::PresentSource& source,const AuroraStereoFrame& frame,
                  uint32_t x0=0,uint32_t imageWidth=0) {
  const auto& output=g_stereoEyeTargets[0].output();
  if(!pipeline || format!=output.format || !source.bindGroup || !source.size.width || !source.size.height) return;
  if(!imageWidth || x0+imageWidth>output.size.width) { x0=0;imageWidth=output.size.width; }
  const auto vp=webgpu::calculate_present_viewport(imageWidth,output.size.height,source.size.width,source.size.height);
  const float width=std::max(.25f,frame.ui.width),height=width*float(source.size.height)/float(source.size.width);
  std::vector<Vertex> vertices;
  const auto at=[&](float x,float y,V color) {
    const float px=vp.left+(x/width+.5f)*vp.width,py=vp.top+(.5f-y/height)*vp.height;
    return Vertex{{px/float(imageWidth)*2-1,1-py/float(output.size.height)*2,0,1},{0,0},{color[0],color[1],color[2]},0};
  };
  for(int hand=0;hand<2;++hand) if(frame.ui.pointerTracked[hand]) {
    const auto* ray=frame.ui.pointerRay[hand];
    if(ray[5]>=-.001f) continue;
    const float distance=(-frame.ui.distance-ray[2])/ray[5];
    if(distance<=0 || distance>10) continue;
    const float x=ray[0]+ray[3]*distance,y=ray[1]+ray[4]*distance;
    if(std::abs(x)>width*.5f || std::abs(y)>height*.5f) continue;
    const V color=hand?V{.96f,.74f,.31f}:V{.15f,.88f,.82f};
    // A coloured ring with a dark edge and a white centre: readable on bright and dark menus alike.
    const auto disc=[&](float radius,V inner,V outer) {
      for(int segment=0;segment<20;++segment) {
        const float angle=segment*6.2831853f/20,next=(segment+1)*6.2831853f/20;
        vertices.push_back(at(x,y,inner));
        vertices.push_back(at(x+radius*std::cos(angle),y+radius*std::sin(angle),outer));
        vertices.push_back(at(x+radius*std::cos(next),y+radius*std::sin(next),outer));
      }
    };
    disc(.018f,{.02f,.02f,.02f},{.02f,.02f,.02f});
    disc(.015f,color,color);
    disc(.006f,{1,1,1},{1,1,1});
  }
  if(vertices.empty()) return;
  const wgpu::BufferDescriptor bd{.label="VR menu pointer marks",.usage=wgpu::BufferUsage::Vertex|wgpu::BufferUsage::CopyDst,.size=vertices.size()*sizeof(Vertex)};
  auto buffer=g_device.CreateBuffer(&bd);g_queue.WriteBuffer(buffer,0,vertices.data(),vertices.size()*sizeof(Vertex));
  const wgpu::RenderPassColorAttachment ca{.view=output.view,.loadOp=wgpu::LoadOp::Load,.storeOp=wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor rd{.label="VR menu pointer marks",.colorAttachmentCount=1,.colorAttachments=&ca};
  auto pass=encoder.BeginRenderPass(&rd);pass.SetPipeline(pipeline);pass.SetBindGroup(0,source.bindGroup);
  pass.SetViewport(float(x0),0,float(imageWidth),float(output.size.height),0,1);
  pass.SetVertexBuffer(0,buffer);pass.Draw(static_cast<uint32_t>(vertices.size()));pass.End();
}
}
