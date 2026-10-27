#include "../lib/gfx/cockpit_item_data.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

static void Check(bool value,const char* what) {
  if (!value) { std::cerr << "FAILED: " << what << '\n'; std::abort(); }
}
static bool Near(float a,float b) { return std::fabs(a-b)<1e-4f; }

int main(int argc, char** argv) {
  using namespace aurora::gfx::cockpit_item::data;
  for (uint8_t id=0; id<19; ++id) Check(model_index(id)>=0,"every inventory ID has a model");
  Check(model_index(19)==-1 && model_index(20)==-1,"no model past the triple banana");
  const uint8_t brokenYaz[]{'Y','a','z','0',0,0,0,4,0,0,0,0,0,0,0,0,0};
  const uint8_t brokenU8[]{0x55,0xaa,0x38,0x2d,0,0,0,0};
  Check(parse_archive(nullptr,0).loaded==0,"null archive");
  Check(parse_archive(brokenYaz,sizeof(brokenYaz)).loaded==0,"truncated Yaz0");
  Check(parse_archive(brokenU8,sizeof(brokenU8)).loaded==0,"truncated U8");
  // Maya texture SRT: a 2x scale pivots t around 1, as G3D does.
  const auto srt=texture_srt(2,2,0,0,0,0);
  Check(Near(srt[0],2) && Near(srt[1],0) && Near(srt[2],0) &&
        Near(srt[3],0) && Near(srt[4],2) && Near(srt[5],-1),"Maya scale");
  const auto identity=texture_srt(1,1,0,0,0,0);
  Check(Near(identity[0],1) && Near(identity[2],0) && Near(identity[4],1) && Near(identity[5],0),"identity SRT");
  // KSEL: fixed fractions, whole konst colours, and single konst components.
  const std::array<Color,4> konst{{{0.1f,0.2f,0.3f,0.4f},{0.5f,0.6f,0.7f,0.8f},{},{}}};
  Check(Near(konst_value(konst,0,false)[0],1) && Near(konst_value(konst,4,true)[3],0.5f),"KSEL fractions");
  Check(Near(konst_value(konst,0x0d,false)[1],0.6f),"KSEL konst colour");
  Check(Near(konst_value(konst,0x1c,true)[3],0.4f) && Near(konst_value(konst,0x15,true)[3],0.6f),"KSEL components");
  Check(Near(signed11(0x7ff),-1.f/255.f) && Near(signed11(0xff),1),"TEV register sign");
  if (argc>1) {
    std::ifstream file(argv[1],std::ios::binary);
    Check(bool(file),"archive readable");
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    const Archive archive=parse_archive(bytes.data(),bytes.size());
    Check(archive.loaded==15,"all 15 item models");
    for (size_t i=0;i<archive.models.size();++i) {
      const auto& model=archive.models[i];
      Check(model.valid(),"model valid");
      size_t vertices=0,billboards=0;
      for (const auto& part:model.parts) {
        Check(part.vertices.size()%3==0,"triangle list");
        Check(part.material<model.materials.size(),"part material");
        vertices+=part.vertices.size();
        billboards+=part.billboard;
      }
      for (const auto& material:model.materials) {
        Check(material.stageCount>=1 && material.stageCount<=4,"TEV stage count");
        for (uint32_t s=0;s<material.stageCount;++s) {
          const auto& stage=material.stages[s];
          if (stage.textured) Check(material.maps[stage.texMap].texture>=0,"stage texture resolves");
        }
      }
      Check(vertices>0,"geometry");
      std::cout << names[i] << ": " << vertices << " vertices, " << model.textures.size() << " textures, "
                << model.materials.size() << " materials, " << billboards << " billboard parts\n";
    }
    // The properties whose absence corrupted the held items.
    const auto& shell=archive.models[0].materials[0];
    Check(shell.maps[0].wrapS==2 && Near(shell.texGens[0].matrix[0],2),"shell mirror wrap and 2x SRT");
    Check(shell.texGens[1].normal,"shell specular is an env map");
    Check(archive.models[2].materials[0].cull==0,"banana is double-sided");
    Check(archive.models[3].parts.size()==3,"fake item box parts");
    size_t thunderBillboards=0;
    for (const auto& part:archive.models[7].parts) thunderBillboards+=part.billboard;
    Check(thunderBillboards==1,"lightning glow is a billboard");
    const auto& bolt=archive.models[7].materials[archive.models[7].parts[0].material];
    Check(Near(bolt.registers[1][0],1) && Near(bolt.registers[1][1],1) && Near(bolt.registers[1][2],0),
          "lightning C0 is yellow");
  }
}
