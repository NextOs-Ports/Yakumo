// Exercise actual GPU pixels; successful shader compilation alone is insufficient.
#include "gpu/vulkan_renderer.hpp"
#include "settings/settings.hpp"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>
using namespace mhp3rd;
using namespace mhp3rd::gpu;
namespace {
unsigned failures=0;
void expect(bool ok,const char *name){std::cout<<(ok?"PASS ":"FAIL ")<<name<<std::endl;if(!ok)++failures;}
DrawCall rectangle(unsigned target,unsigned rgba,float x,float y,float w,float h){
    DrawCall c{};c.primitive=PrimitiveType::Sprites;c.through=true;c.has_vertex_color=true;
    c.target.color_address=target;c.target.color_format=3;c.target.color_stride=512;
    Vertex a{},b{};a.position={x,y,32767,1};b.position={x+w,y+h,32767,1};a.color=b.color=rgba;
    a.texcoord={0,0};b.texcoord={2,2};c.vertices={a,b};return c;
}
unsigned pixel(psprecomp::GuestMemory &m,unsigned target,unsigned x,unsigned y){return m.load32(target+(y*512+x)*4);}
}
int main(int argc,char **argv){
    try{
        auto &s=settings::current();s.internal_scale=1;s.window_scale=1;s.mouse=false;s.texture_pack=false;
        VulkanRenderer r;std::string error;
        if(!r.initialize({},error))throw std::runtime_error(error);
        psprecomp::GuestMemory memory;
        constexpr unsigned a=0x04000000,b=0x04100000;
        auto clear=rectangle(a,0xff000000,0,0,480,272);clear.clear_mode=true;clear.clear_flags=7;r.submit(clear,memory);
        auto red=rectangle(a,0xff0000ff,10,10,100,100);r.submit(red,memory);
        r.read_back_framebuffer(a,memory);
        expect(pixel(memory,a,30,30)==0xff0000ff,"sprite color and location");
        expect(pixel(memory,a,200,200)==0xff000000,"clear and untouched area");
        unsigned *data=reinterpret_cast<unsigned*>(memory.raw_pointer(0x08800000,16));
        for(int n=0;n<4;++n)data[n]=0xff00ff00;
        auto textured=rectangle(a,0xffffffff,130,10,100,100);textured.texture.enabled=true;
        textured.texture.address=0x08800000;textured.texture.width=2;textured.texture.height=2;textured.texture.buffer_width=2;
        textured.texture.format=TextureFormat::Rgba8888;r.submit(textured,memory);
        r.read_back_framebuffer(a,memory);expect(pixel(memory,a,160,30)==0xff00ff00,"decoded RGBA texture");
        auto alpha=rectangle(a,0x000000ff,130,10,100,100);alpha.alpha_test.enabled=true;alpha.alpha_test.function=6;alpha.alpha_test.reference=128;r.submit(alpha,memory);
        r.read_back_framebuffer(a,memory);expect(pixel(memory,a,160,30)==0xff00ff00,"alpha rejection retains destination");
        auto blue=rectangle(b,0xffff0000,0,0,480,272);r.submit(blue,memory);
        auto copy=rectangle(a,0xffffffff,250,10,100,100);copy.texture.enabled=true;copy.texture.address=b;
        copy.texture.width=512;copy.texture.height=512;copy.texture.buffer_width=512;copy.texture.format=TextureFormat::Rgba8888;
        copy.vertices[1].texcoord={100,100};r.submit(copy,memory);
        r.read_back_framebuffer(a,memory);expect(pixel(memory,a,280,30)==0xffff0000,"render target sampled as texture");
        auto feedback=copy;feedback.texture.address=a;feedback.vertices[0].position={10,150,32767,1};feedback.vertices[1].position={110,250,32767,1};
        feedback.vertices[0].texcoord={250,10};feedback.vertices[1].texcoord={350,110};r.submit(feedback,memory);
        r.read_back_framebuffer(a,memory);expect(pixel(memory,a,30,180)==0xffff0000,"same target feedback copied before draw");
        r.present(a);if(argc>1)expect(r.capture_frame(argv[1]),"capture exact rendered target");
        IMGUI_CHECKVERSION();ImGui::CreateContext();
        expect(r.initialize_ui(error),"original ImGui GLES2 backend");
        auto &io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(480,272);io.DeltaTime=1.0f/30;
        r.begin_ui_frame();ImGui::NewFrame();ImGui::Begin("GLES2 test");ImGui::TextUnformatted("Yakumo graphics test");ImGui::End();ImGui::Render();
        r.set_ui_draw_data(ImGui::GetDrawData());r.present_ui(true);r.shutdown_ui();ImGui::DestroyContext();
        std::cout<<"RESULT failures="<<failures<<" GPU="<<r.device_name()<<std::endl;
        return failures?1:0;
    }catch(const std::exception &e){std::cerr<<e.what()<<std::endl;return 2;}
}
