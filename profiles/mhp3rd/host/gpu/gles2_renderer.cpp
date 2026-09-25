// GLES2 implementation of the existing renderer interface. The class name is
// retained so the game's kernel, menus and camera keep their existing contract.
#include "vulkan_renderer.hpp"
#include "texture_decode.hpp"
#include "triangle_indices.hpp"
#include "input/bindings.hpp"
#include "install/user_data.hpp"
#include "install/game_identity.hpp"
#include "perf/frame_stats.hpp"
#include "settings/settings.hpp"
#include <SDL3/SDL.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mhp3rd::gpu {
namespace {
#include "gles_shaders.inc"
std::array<float, 16> multiply(const std::array<float, 16> &a, const std::array<float, 16> &b) {
    std::array<float, 16> result{};
    for (std::uint32_t column = 0; column < 4u; ++column) {
        for (std::uint32_t row = 0; row < 4u; ++row) {
            float sum = 0.0f;
            for (std::uint32_t k = 0; k < 4u; ++k) sum += a[k * 4u + row] * b[column * 4u + k];
            result[column * 4u + row] = sum;
        }
    }
    return result;
}

bool write_bmp(const std::string &path, const std::uint8_t *pixels, std::uint32_t width, std::uint32_t height,
               bool bgra) {
    const std::uint32_t row_bytes = (width * 3u + 3u) & ~3u;
    const std::uint32_t image_bytes = row_bytes * height;
    std::vector<std::uint8_t> file(54u + image_bytes, 0u);
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        file[at] = static_cast<std::uint8_t>(value);
        file[at + 1u] = static_cast<std::uint8_t>(value >> 8u);
        file[at + 2u] = static_cast<std::uint8_t>(value >> 16u);
        file[at + 3u] = static_cast<std::uint8_t>(value >> 24u);
    };
    file[0] = 'B';
    file[1] = 'M';
    put32(2u, 54u + image_bytes);
    put32(10u, 54u);
    put32(14u, 40u);
    put32(18u, width);
    put32(22u, height);
    file[26] = 1u;
    file[28] = 24u;
    put32(34u, image_bytes);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t *source = pixels + static_cast<std::size_t>(y) * width * 4u;
        std::uint8_t *destination = file.data() + 54u + static_cast<std::size_t>(height - 1u - y) * row_bytes;
        for (std::uint32_t x = 0; x < width; ++x) {
            destination[x * 3u + 0u] = source[x * 4u + (bgra ? 0u : 2u)];
            destination[x * 3u + 1u] = source[x * 4u + 1u];
            destination[x * 3u + 2u] = source[x * 4u + (bgra ? 2u : 0u)];
        }
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(out);
}

struct PadTuning {
    float dead_zone{0.15f};
    float trigger{0.25f};
    settings::TriggerProfile triggers{settings::TriggerProfile::Standard};
    float right_stick{0.5f};
    settings::RightStick right_stick_mode{settings::RightStick::Camera};
    bool invert_x{};
    bool invert_y{};
    bool confirm_south{};
    bool trace{false};
};

// Read on every poll, so the in-game menu's changes apply at once.
PadTuning pad_tuning() {
    static const bool trace = std::getenv("MHP3RD_TRACE_PAD") != nullptr;
    const settings::Settings &player = settings::current();
    PadTuning value{};
    value.dead_zone = player.dead_zone;
    value.trigger = player.trigger;
    value.triggers = player.trigger_profile;
    value.right_stick = player.right_stick_zone;
    // The right stick is a real nub on this release, so driving the D-pad
    // from it as well would turn the camera twice.
    value.right_stick_mode = player.right_stick;
    value.invert_x = player.invert_camera_x;
    value.invert_y = player.invert_camera_y;
    // A PlayStation pad already carries the PSP's own face buttons, so the
    // positional mapping puts confirm on circle where the prompts want it.
    value.confirm_south = player.confirm_south;
    value.trace = trace;
    return value;
}

// Adds one gamepad's state to the pad bits and to the analog offsets the
// keyboard path also writes, so the two sources simply OR together.
void read_gamepad(SDL_Gamepad *device, PadState &pad, int &analog_x, int &analog_y) {
    const PadTuning tuning = pad_tuning();
    std::uint32_t &buttons = pad.buttons;
    const auto held = [&](SDL_GamepadButton button, std::uint32_t bit) {
        if (SDL_GetGamepadButton(device, button)) buttons |= bit;
    };
    held(SDL_GAMEPAD_BUTTON_DPAD_UP, 0x0010u);
    held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0020u);
    held(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x0040u);
    held(SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x0080u);
    held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x0100u);
    held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200u);
    held(SDL_GAMEPAD_BUTTON_START, 0x0008u);
    held(SDL_GAMEPAD_BUTTON_BACK, 0x0001u);
    held(SDL_GAMEPAD_BUTTON_NORTH, 0x1000u);
    held(SDL_GAMEPAD_BUTTON_WEST, 0x8000u);
    // The game prompts "circle Enter / cross Back", and on a PlayStation pad
    // those are the same two buttons in the same two places.
    held(SDL_GAMEPAD_BUTTON_SOUTH, tuning.confirm_south ? 0x2000u : 0x4000u);
    held(SDL_GAMEPAD_BUTTON_EAST, tuning.confirm_south ? 0x4000u : 0x2000u);

    // The PSP triggers are digital, but hunters hold L for the camera all the
    // time, so the analog triggers press the same bits as the shoulders.
    const auto axis = [&](SDL_GamepadAxis id) {
        return std::clamp(static_cast<float>(SDL_GetGamepadAxis(device, id)) / 32767.0f, -1.0f, 1.0f);
    };
    // The trigger profiles copy other buttons for shooting: R on L2, where it
    // is held to aim, and the weapon's attack on R2 -- triangle for a bow,
    // circle for a bowgun. The buttons copied keep working.
    std::uint32_t left_trigger = 0x0100u;   // L
    std::uint32_t right_trigger = 0x0200u;  // R
    if (tuning.triggers == settings::TriggerProfile::Bows) {
        left_trigger = 0x0200u;
        right_trigger = 0x1000u;  // triangle
    } else if (tuning.triggers == settings::TriggerProfile::Bowguns) {
        left_trigger = 0x0200u;
        right_trigger = 0x2000u;  // circle
    }
    if (axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > tuning.trigger) buttons |= left_trigger;
    if (axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > tuning.trigger) buttons |= right_trigger;

    const float right_x = axis(SDL_GAMEPAD_AXIS_RIGHTX);
    const float right_y = axis(SDL_GAMEPAD_AXIS_RIGHTY);
    // The HD release has its own right-stick camera, so the stick normally goes
    // there. Pressing the D-pad bits as well would turn the camera twice, hence
    // the either/or: the claw emulation is only for builds where that path is
    // not wanted.
    if (tuning.right_stick_mode == settings::RightStick::DPad) {
        if (right_x < -tuning.right_stick) buttons |= 0x0080u;
        if (right_x > tuning.right_stick) buttons |= 0x0020u;
        if (right_y < -tuning.right_stick) buttons |= 0x0010u;
        if (right_y > tuning.right_stick) buttons |= 0x0040u;
    }

    // Rescale the live range, otherwise leaving the dead zone snaps the stick
    // straight to a sixth of its travel.
    const auto deflect = [&](float x, float y, std::uint8_t &out_x, std::uint8_t &out_y) {
        const float length = std::sqrt(x * x + y * y);
        if (length <= tuning.dead_zone) return;
        const float scale = std::min((length - tuning.dead_zone) / (1.0f - tuning.dead_zone), 1.0f) / length;
        out_x = static_cast<std::uint8_t>(std::clamp(0x80 + static_cast<int>(x * scale * 127.0f), 0, 255));
        out_y = static_cast<std::uint8_t>(std::clamp(0x80 + static_cast<int>(y * scale * 127.0f), 0, 255));
    };
    if (tuning.right_stick_mode == settings::RightStick::Camera)
        deflect(tuning.invert_x ? -right_x : right_x, tuning.invert_y ? -right_y : right_y, pad.right_x, pad.right_y);

    const float left_x = axis(SDL_GAMEPAD_AXIS_LEFTX);
    const float left_y = axis(SDL_GAMEPAD_AXIS_LEFTY);
    std::uint8_t nub_x = 0x80u;
    std::uint8_t nub_y = 0x80u;
    deflect(left_x, left_y, nub_x, nub_y);
    analog_x += static_cast<int>(nub_x) - 0x80;
    analog_y += static_cast<int>(nub_y) - 0x80;
}


constexpr unsigned kWidth = 480, kHeight = 272;
void checked(const char *where) {
    const GLenum code = glGetError();
    if (code != GL_NO_ERROR)
        throw std::runtime_error(std::string("GLES2 ") + where + " error " + std::to_string(code));
}
GLuint shader(GLenum kind, const char *source) {
    GLuint id = glCreateShader(kind);
    glShaderSource(id, 1, &source, nullptr);
    glCompileShader(id);
    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192]{};
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        glDeleteShader(id);
        throw std::runtime_error(std::string("GLES2 shader: ") + log);
    }
    return id;
}
GLuint program(const char *vs, const char *fs) {
    GLuint v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    const char *names[]{"in_position", "in_texcoord", "in_color", "in_normal"};
    for (unsigned i = 0; i < 4; ++i) glBindAttribLocation(p, i, names[i]);
    glLinkProgram(p); glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[8192]{}; glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        glDeleteProgram(p); throw std::runtime_error(std::string("GLES2 link: ") + log);
    }
    return p;
}
std::array<float, 4> color(unsigned c, float a = 1.0f) {
    return {float(c & 255)/255, float((c >> 8)&255)/255, float((c >> 16)&255)/255, a};
}
unsigned address(unsigned a) { return a & 0x3ffffffu; }
GLenum compare(unsigned f) {
    const GLenum values[]{GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};
    return values[f & 7];
}
GLenum factor(unsigned f, unsigned fixed, bool source) {
    if (f == 10) {
        if ((fixed & 0xffffffu) == 0) return GL_ZERO;
        if ((fixed & 0xffffffu) == 0xffffffu) return GL_ONE;
        return GL_CONSTANT_COLOR;
    }
    const GLenum values[]{GLenum(source ? GL_DST_COLOR : GL_SRC_COLOR),
        GLenum(source ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR),
        GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
        GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
    return f < 10 ? values[f] : (source ? GL_ONE : GL_ZERO);
}
GLuint texture(unsigned w, unsigned h, const void *pixels) {
    GLuint t = 0; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
        GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    return t;
}
const char *kBlitVertex = R"(
attribute vec4 in_position;
attribute vec2 in_texcoord;
varying vec2 uv;
void main() { gl_Position = in_position; uv = in_texcoord; }
)";
const char *kBlitFragment = R"(
precision mediump float;
uniform sampler2D image;
varying vec2 uv;
void main() { gl_FragColor = vec4(texture2D(image, uv).rgb, 1.0); }
)";
struct GpuVertex {
    float x, y, z, w;
    float u, v;
    unsigned rgba;
    float nx, ny, nz;
};
} // namespace

struct VulkanRenderer::Impl {
    SDL_Window *window{};
    SDL_GLContext context{};
    SDL_Gamepad *gamepad{};
    bool ready{}, quit{}, ui{}, game_input{true}, pointer_free{}, scripted_input{};
    bool hold{}, fast{}, sharp_screen{}, sharp_textures{}, held_pack{}, perf_overlay{};
    bool suppress_held{}, mouse_captured{};
    unsigned suppressed_buttons{};
    std::array<bool, input::kKeyPositions> scripted_keys{};
    unsigned mouse_buttons{};
    PadState pad{};
    MouseMotion mouse_motion{};
    input::touch::Controls touch{};
    std::function<bool(const SDL_Event &)> event_hook;
    ImDrawData *ui_data{};
    std::string capture_pending, gpu;
    GLuint draw_program{}, blit_program{}, vbo{}, ibo{}, white{}, feedback{};
    unsigned width{kWidth}, height{kHeight}, display{}, current{};
    settings::Aspect aspect{settings::Aspect::Original};
    std::uint64_t frames{}, draws{}, tick{};
    std::chrono::steady_clock::time_point last_present{};
    struct Target { GLuint fbo{}, tex{}, depth{}; RenderTarget state{}; bool drawn{}; };
    struct CachedTexture { GLuint tex{}; std::uint64_t used{}; };
    std::map<unsigned, Target> targets;
    std::map<std::uint64_t, CachedTexture> textures;
    std::map<std::string, GLint> locations;
    std::vector<GpuVertex> vertices;
    std::vector<unsigned char> read_pixels;
    CameraReading camera{};

    GLint loc(const std::string &name) {
        auto it = locations.find(name);
        if (it != locations.end()) return it->second;
        return locations.emplace(name, glGetUniformLocation(draw_program, name.c_str())).first->second;
    }
    void vec4(const std::string &name, const std::array<float,4> &v) { glUniform4fv(loc(name), 1, v.data()); }
    void matrix(const char *name, const std::array<float,16> &v) { glUniformMatrix4fv(loc(name), 1, GL_FALSE, v.data()); }
    Target &target(RenderTarget state) {
        const unsigned key = address(state.color_address);
        auto [it, inserted] = targets.try_emplace(key);
        Target &t = it->second; t.state = state;
        if (inserted) {
            if (targets.size() > 32) throw std::runtime_error("GLES2 framebuffer limit exceeded");
            t.tex = texture(width, height, nullptr);
            glGenFramebuffers(1, &t.fbo); glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
            glGenRenderbuffers(1, &t.depth); glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("GLES2 incomplete framebuffer");
            glDisable(GL_SCISSOR_TEST); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glDepthMask(GL_TRUE);
            glClearColor(0,0,0,0); glClearDepthf(1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        return t;
    }
    void drop_targets() {
        for (auto &[key,t] : targets) {
            glDeleteFramebuffers(1,&t.fbo); glDeleteTextures(1,&t.tex); glDeleteRenderbuffers(1,&t.depth);
        }
        targets.clear();
        if (feedback) glDeleteTextures(1,&feedback);
        feedback = 0;
    }
    void bind(Target &t) { glBindFramebuffer(GL_FRAMEBUFFER,t.fbo); current=address(t.state.color_address); }
    void attributes() {
        glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2); glEnableVertexAttribArray(3);
        glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,sizeof(GpuVertex),nullptr);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,sizeof(GpuVertex),reinterpret_cast<void*>(16));
        glVertexAttribPointer(2,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(GpuVertex),reinterpret_cast<void*>(24));
        glVertexAttribPointer(3,3,GL_FLOAT,GL_FALSE,sizeof(GpuVertex),reinterpret_cast<void*>(28));
    }
    void readback(Target &t, GuestMemory &memory) {
        bind(t);
        read_pixels.resize(static_cast<std::size_t>(width)*height*4);
        glReadPixels(0,0,static_cast<GLsizei>(width),static_cast<GLsizei>(height),GL_RGBA,GL_UNSIGNED_BYTE,read_pixels.data());
        checked("framebuffer readback");
        const unsigned stride=std::max(t.state.color_stride,kWidth), fmt=t.state.color_format, bytes=fmt==3?4:2;
        for (unsigned y=0;y<kHeight;++y) {
            auto *dst=memory.raw_pointer(t.state.color_address+y*stride*bytes,kWidth*bytes);
            if (!dst) continue;
            for(unsigned x=0;x<kWidth;++x) {
                const auto *p=&read_pixels[(static_cast<std::size_t>(y*height/kHeight)*width+x*width/kWidth)*4];
                unsigned v=0;
                if(fmt==3) v=unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16)|(unsigned(p[3])<<24);
                else if(fmt==0) v=(p[0]>>3)|((p[1]>>2)<<5)|((p[2]>>3)<<11);
                else if(fmt==1) v=(p[0]>>3)|((p[1]>>3)<<5)|((p[2]>>3)<<10)|((p[3]>>7)<<15);
                else v=(p[0]>>4)|((p[1]>>4)<<4)|((p[2]>>4)<<8)|((p[3]>>4)<<12);
                for(unsigned b=0;b<bytes;++b) dst[x*bytes+b]=static_cast<unsigned char>(v>>(8*b));
            }
        }
    }
    void blit(bool show_game) {
        glBindFramebuffer(GL_FRAMEBUFFER,0);
        int w=0,h=0; SDL_GetWindowSizeInPixels(window,&w,&h);
        glViewport(0,0,w,h); glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        auto it=targets.find(display);
        if (show_game && it!=targets.end()) {
            if(aspect==settings::Aspect::Original) {
                const float ratio=float(kWidth)/kHeight;
                if(float(w)/h>ratio) {int vw=static_cast<int>(float(h)*ratio); glViewport((w-vw)/2,0,vw,h);}
                else {int vh=static_cast<int>(float(w)/ratio); glViewport(0,(h-vh)/2,w,vh);}
            }
            glUseProgram(blit_program); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,it->second.tex);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,sharp_screen?GL_NEAREST:GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,sharp_screen?GL_NEAREST:GL_LINEAR);
            // Target row zero is PSP top; display row zero is the screen bottom.
            const float quad[]{-1,-1,0,1, 1,-1,1,1, -1,1,0,0, 1,1,1,0};
            glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STREAM_DRAW);
            glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
            glDisableVertexAttribArray(2); glDisableVertexAttribArray(3);
            glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,nullptr);
            glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,16,reinterpret_cast<void*>(8));
            glUniform1i(glGetUniformLocation(blit_program,"image"),0);
            glDrawArrays(GL_TRIANGLE_STRIP,0,4);
        }
        glViewport(0,0,w,h);
        if(ui && ui_data) {ImGui_ImplOpenGL3_RenderDrawData(ui_data);ui_data=nullptr;}
        // The Amlogic framebuffer compositor requires opaque scanout alpha.
        glDisable(GL_SCISSOR_TEST);glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
        glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        checked("present");
        if(!capture_pending.empty()) {
            std::vector<unsigned char> pixels(static_cast<std::size_t>(w)*h*4),top(pixels.size());
            glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
            for(int y=0;y<h;++y) std::memcpy(top.data()+static_cast<std::size_t>(y)*w*4,pixels.data()+static_cast<std::size_t>(h-1-y)*w*4,static_cast<std::size_t>(w)*4);
            if(!write_bmp(capture_pending,top.data(),static_cast<unsigned>(w),static_cast<unsigned>(h),false))
                throw std::runtime_error("GLES2 capture write failed");
            capture_pending.clear();
        }
        if(!SDL_GL_SwapWindow(window))throw std::runtime_error(SDL_GetError());
        perf::count_present();last_present=std::chrono::steady_clock::now();
    }
};

VulkanRenderer::VulkanRenderer():impl_(std::make_unique<Impl>()) {}
VulkanRenderer::~VulkanRenderer(){shutdown();}
bool VulkanRenderer::initialize(const RendererConfig &config,std::string &error) {
    auto &i=*impl_;
    try {
        if(!SDL_InitSubSystem(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD))throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,0);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE,8);SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE,8);SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,8);SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,16);SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
        auto flags=SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE;
        const auto &s=settings::current();
        if(s.fullscreen)flags|=SDL_WINDOW_FULLSCREEN;
        i.window=SDL_CreateWindow(config.title.c_str(),static_cast<int>(kWidth*s.window_scale),static_cast<int>(kHeight*s.window_scale),flags);
        if(!i.window)throw std::runtime_error(SDL_GetError());
        i.context=SDL_GL_CreateContext(i.window);if(!i.context)throw std::runtime_error(SDL_GetError());
        i.gpu=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        i.draw_program=program(kGlesVertex,kGlesFragment);i.blit_program=program(kBlitVertex,kBlitFragment);
        glGenBuffers(1,&i.vbo);glGenBuffers(1,&i.ibo);
        unsigned white=0xffffffffu;i.white=texture(1,1,&white);
        int count=0;auto *ids=SDL_GetGamepads(&count);
        if(count>0)i.gamepad=SDL_OpenGamepad(ids[0]);SDL_free(ids);
        i.ready=true;i.aspect=s.aspect;i.sharp_screen=s.sharp_screen;i.sharp_textures=s.sharp_textures;
        set_internal_scale(s.internal_scale);set_present_mode(s.present_mode);
        checked("initialize");
        std::cout<<"[render] GLES2 "<<glGetString(GL_VERSION)<<" renderer="<<i.gpu<<" target="<<i.width<<"x"<<i.height<<"\n"<<std::flush;
        return true;
    } catch(const std::exception &e) {error=e.what();shutdown();return false;}
}
void VulkanRenderer::shutdown(){
    if(!impl_)return;auto &i=*impl_;
    if(i.context){shutdown_ui();i.drop_targets();for(auto &[k,t]:i.textures)glDeleteTextures(1,&t.tex);i.textures.clear();
        glDeleteTextures(1,&i.white);glDeleteBuffers(1,&i.vbo);glDeleteBuffers(1,&i.ibo);glDeleteProgram(i.draw_program);glDeleteProgram(i.blit_program);
        SDL_GL_DestroyContext(i.context);i.context=nullptr;}
    if(i.gamepad){SDL_CloseGamepad(i.gamepad);i.gamepad=nullptr;}
    if(i.window){SDL_DestroyWindow(i.window);i.window=nullptr;}i.ready=false;
}
bool VulkanRenderer::available()const noexcept{return impl_&&impl_->ready;}
bool VulkanRenderer::quit_requested()const noexcept{return !impl_||impl_->quit;}
bool VulkanRenderer::pump_events(){
    auto &i=*impl_;SDL_Event e;
    while(SDL_PollEvent(&e)){
        // Handle the exit chord per physical pad and before a menu can consume it.
        if(e.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && i.gamepad && e.gbutton.which==SDL_GetGamepadID(i.gamepad) &&
           SDL_GetGamepadButton(i.gamepad,SDL_GAMEPAD_BUTTON_BACK)&&SDL_GetGamepadButton(i.gamepad,SDL_GAMEPAD_BUTTON_START))i.quit=true;
        if(e.type==SDL_EVENT_QUIT)i.quit=true;
        if(e.type==SDL_EVENT_GAMEPAD_ADDED && !i.gamepad)i.gamepad=SDL_OpenGamepad(e.gdevice.which);
        if(e.type==SDL_EVENT_GAMEPAD_REMOVED && i.gamepad && SDL_GetGamepadID(i.gamepad)==e.gdevice.which){SDL_CloseGamepad(i.gamepad);i.gamepad=nullptr;}
        if(i.event_hook&&i.event_hook(e))continue;
        if(e.type==SDL_EVENT_MOUSE_MOTION && (!i.scripted_input||e.motion.which==kScriptedMouse)){
            i.mouse_motion.x+=e.motion.xrel;i.mouse_motion.y+=e.motion.yrel;
        }
        if(e.type==SDL_EVENT_MOUSE_BUTTON_DOWN)i.mouse_buttons|=1u<<e.button.button;
        if(e.type==SDL_EVENT_MOUSE_BUTTON_UP)i.mouse_buttons&=~(1u<<e.button.button);
    }
    sample_pad();return !i.quit;
}
void VulkanRenderer::sample_pad(){
    auto &i=*impl_;SDL_PumpEvents();
    if(i.gamepad && SDL_GetGamepadButton(i.gamepad,SDL_GAMEPAD_BUTTON_BACK)&&SDL_GetGamepadButton(i.gamepad,SDL_GAMEPAD_BUTTON_START))i.quit=true;
    if(!i.game_input){i.pad={};return;}
    const auto *keys=SDL_GetKeyboardState(nullptr);
    auto typed=input::read(settings::current().bindings,[&](input::Binding b){
        if(int button=input::mouse_button_of(b))return (i.mouse_buttons&(1u<<button))!=0;
        int p=input::key_position(b);return p>=0&&(i.scripted_keys[static_cast<std::size_t>(p)]||(!i.scripted_input&&keys[p]));
    });
    PadState p{};p.buttons=typed.buttons;int x=typed.stick_x,y=typed.stick_y;
    p.right_x=static_cast<unsigned char>(128+typed.camera_x);p.right_y=static_cast<unsigned char>(128+typed.camera_y);
    if(i.gamepad)read_gamepad(i.gamepad,p,x,y);
    p.analog_x=static_cast<unsigned char>(std::clamp(128+x,0,255));p.analog_y=static_cast<unsigned char>(std::clamp(128+y,0,255));
    if(i.suppress_held){i.suppressed_buttons=p.buttons;i.suppress_held=false;}
    i.suppressed_buttons&=p.buttons;p.buttons&=~i.suppressed_buttons;i.pad=p;
}
PadState VulkanRenderer::pad()const noexcept{return impl_->pad;}
void VulkanRenderer::begin_frame(){}
void VulkanRenderer::begin_display_list(){}
bool VulkanRenderer::gpu_decode()const{return false;}
bool VulkanRenderer::check_gpu_decode()const{return false;}

void VulkanRenderer::submit(const DrawCall &c,const GuestMemory &memory){
    auto &i=*impl_;if(!i.ready||c.vertices.empty())return;
    const perf::SplitScope timer(perf::Split::Draw);
    auto &dst=i.target(c.target);i.bind(dst);
    glUseProgram(i.draw_program);glActiveTexture(GL_TEXTURE0);
    std::array<float,4> uv=c.through&&c.texture.width&&c.texture.height?
        std::array<float,4>{1.0f/c.texture.width,1.0f/c.texture.height,0,0}:
        std::array<float,4>{c.texture.scale_u,c.texture.scale_v,c.texture.offset_u,c.texture.offset_v};
    GLuint tex=i.white;
    if(c.texture.enabled&&!c.clear_mode){
        bool framebuffer=false;
        for(auto &[a,t]:i.targets){
            const unsigned bpp=t.state.color_format==3?4:2;
            const unsigned stride=std::max(t.state.color_stride,kWidth);
            const unsigned ta=address(c.texture.address);
            if(!t.drawn || ta<a || ta>=a+stride*kHeight*bpp)continue;
            const unsigned pixel=(ta-a)/bpp, x=pixel%stride,y=pixel/stride;
            if(x>=kWidth||y>=kHeight)continue;
            tex=t.tex;framebuffer=true;
            if(&t==&dst){
                // GLES2 cannot sample an attachment while drawing into it.
                if(!i.feedback)i.feedback=texture(i.width,i.height,nullptr);
                glBindTexture(GL_TEXTURE_2D,i.feedback);
                glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,static_cast<GLsizei>(i.width),static_cast<GLsizei>(i.height));
                tex=i.feedback;
            }
            uv={uv[0]*c.texture.width/kWidth,uv[1]*c.texture.height/kHeight,
                (uv[2]*c.texture.width+x)/kWidth,(uv[3]*c.texture.height+y)/kHeight};
            break;
        }
        if(!framebuffer){
            const auto key=texture_key(memory,c.texture);
            auto it=i.textures.find(key);
            if(it==i.textures.end()){
                std::vector<unsigned> pixels;
                if(!decode_texture(memory,c.texture,pixels)||pixels.empty())
                    throw std::runtime_error("GLES2 texture decode failed at "+std::to_string(c.texture.address));
                if(i.textures.size()>=512){
                    auto oldest=std::min_element(i.textures.begin(),i.textures.end(),[](auto &a,auto &b){return a.second.used<b.second.used;});
                    glDeleteTextures(1,&oldest->second.tex);i.textures.erase(oldest);
                }
                it=i.textures.emplace(key,Impl::CachedTexture{texture(c.texture.width,c.texture.height,pixels.data()),0}).first;
            }
            tex=it->second.tex;it->second.used=++i.tick;
        }
        glBindTexture(GL_TEXTURE_2D,tex);
        const bool linear=!i.sharp_textures && ((c.texture.min_filter&1u)||(c.texture.mag_filter&1u));
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear?GL_LINEAR:GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,framebuffer||c.texture.wrap_s?GL_CLAMP_TO_EDGE:GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,framebuffer||c.texture.wrap_t?GL_CLAMP_TO_EDGE:GL_REPEAT);
    }else glBindTexture(GL_TEXTURE_2D,tex);
    i.bind(dst);
    glUniform1i(i.loc("guest_texture"),0);
    const bool lit=c.lighting_enabled&&!c.through&&!c.clear_mode;
    const bool fog=c.fog.enabled&&!c.through&&!c.clear_mode;
    const auto vw=multiply(c.view,c.world);
    i.matrix("push_transform",multiply(c.projection,vw));i.matrix("object_world",c.world);
    i.vec4("push_viewport",{float(kWidth),float(kHeight),c.through?1.0f:0.0f,(fog?1.0f:0.0f)+(lit?2.0f:0.0f)});
    i.vec4("push_texture_params",{c.texture.enabled&&!c.clear_mode?1.0f:0.0f,float(c.texture.function),float(c.alpha_test.reference),float(c.alpha_test.function)});
    i.vec4("push_uv_transform",uv);i.vec4("push_view_z",{vw[2],vw[6],vw[10],vw[14]});
    glUniform1i(i.loc("kAlphaTest"),c.alpha_test.enabled&&!c.clear_mode);
    i.vec4("lighting_fog",{c.fog.end,c.fog.scale,0,0});i.vec4("lighting_fog_color",color(c.fog.color));
    if(lit){
        const auto &l=c.lighting;
        i.vec4("lighting_ambient",color(l.ambient_color,float(l.ambient_alpha)/255));
        i.vec4("object_flags",{1,c.has_vertex_color?1.0f:0.0f,0,float(l.material_update)});
        i.vec4("object_emissive",color(l.material_emissive,l.specular_power));
        i.vec4("object_material_ambient",color(c.material_color,float(c.material_color>>24)/255));
        i.vec4("object_material_diffuse",color(l.material_diffuse,float(l.mode)));
        i.vec4("object_material_specular",color(l.material_specular,l.reverse_normals?1.0f:0.0f));
        for(unsigned n=0;n<4;++n){
            const auto &v=l.lights[n];const auto suffix="["+std::to_string(n)+"]";
            i.vec4("lighting_light_position"+suffix,{v.position[0],v.position[1],v.position[2],v.enabled?1.0f:0.0f});
            i.vec4("lighting_light_direction"+suffix,{v.direction[0],v.direction[1],v.direction[2],float(v.type)});
            i.vec4("lighting_light_attenuation"+suffix,{v.attenuation[0],v.attenuation[1],v.attenuation[2],float(v.kind)});
            i.vec4("lighting_light_spot"+suffix,{v.spot_exponent,v.spot_cutoff,0,0});
            i.vec4("lighting_light_ambient"+suffix,color(v.ambient));i.vec4("lighting_light_diffuse"+suffix,color(v.diffuse));
            i.vec4("lighting_light_specular"+suffix,color(v.specular));
        }
    }
    float sign_x=1,sign_y=1;
    glViewport(0,0,static_cast<GLsizei>(i.width),static_cast<GLsizei>(i.height));glDepthRangef(0,1);
    if(!c.through&&c.viewport.x_scale!=0&&c.viewport.y_scale!=0){
        const auto &v=c.viewport;const float sx=float(i.width)/kWidth,sy=float(i.height)/kHeight;
        sign_x=v.x_scale<0?-1.0f:1.0f;sign_y=v.y_scale<0?-1.0f:1.0f;
        glViewport(static_cast<GLint>((v.x_offset-v.offset_x-std::abs(v.x_scale))*sx),
            static_cast<GLint>((v.y_offset-v.offset_y-std::abs(v.y_scale))*sy),
            static_cast<GLsizei>(2*std::abs(v.x_scale)*sx),static_cast<GLsizei>(2*std::abs(v.y_scale)*sy));
        if(v.z_scale!=0)glDepthRangef(std::clamp((v.z_offset-v.z_scale)/65535,0.0f,1.0f),std::clamp((v.z_offset+v.z_scale)/65535,0.0f,1.0f));
    }
    glUniform2f(i.loc("ndc_sign"),sign_x,sign_y);
    unsigned x1=std::min(c.viewport.scissor_x1,kWidth-1),y1=std::min(c.viewport.scissor_y1,kHeight-1);
    unsigned x2=std::min(std::max(c.viewport.scissor_x2,x1),kWidth-1),y2=std::min(std::max(c.viewport.scissor_y2,y1),kHeight-1);
    glEnable(GL_SCISSOR_TEST);glScissor(static_cast<GLint>(x1*i.width/kWidth),static_cast<GLint>(y1*i.height/kHeight),
        static_cast<GLsizei>((x2+1)*i.width/kWidth-x1*i.width/kWidth),static_cast<GLsizei>((y2+1)*i.height/kHeight-y1*i.height/kHeight));
    if(c.clear_mode){
        glDisable(GL_BLEND);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_ALWAYS);glDepthMask((c.clear_flags&4)?GL_TRUE:GL_FALSE);
        const GLboolean rgb=(c.clear_flags&1)?GL_TRUE:GL_FALSE,alpha=(c.clear_flags&2)?GL_TRUE:GL_FALSE;
        glColorMask(rgb,rgb,rgb,alpha);glDisable(GL_CULL_FACE);
    }else{
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        if(c.depth.test_enabled)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
        glDepthFunc(compare(c.depth.function));glDepthMask(c.depth.write_enabled?GL_TRUE:GL_FALSE);
        if(c.culling_enabled&&!c.through){glEnable(GL_CULL_FACE);glCullFace(GL_BACK);glFrontFace(c.cull_clockwise?GL_CW:GL_CCW);}else glDisable(GL_CULL_FACE);
        if(c.blend.enabled){
            glEnable(GL_BLEND);auto src=factor(c.blend.source_factor,c.blend.fixed_source,true),dstf=factor(c.blend.destination_factor,c.blend.fixed_destination,false);
            auto constant=color(src==GL_CONSTANT_COLOR?c.blend.fixed_source:c.blend.fixed_destination);
            if(src==GL_CONSTANT_COLOR&&dstf==GL_CONSTANT_COLOR&&((c.blend.fixed_source^c.blend.fixed_destination)&0xffffffu)==0xffffffu)dstf=GL_ONE_MINUS_CONSTANT_COLOR;
            glBlendColor(constant[0],constant[1],constant[2],constant[3]);glBlendFuncSeparate(src,dstf,GL_ONE,GL_ZERO);
            const GLenum equations[]{GL_FUNC_ADD,GL_FUNC_SUBTRACT,GL_FUNC_REVERSE_SUBTRACT,GL_MIN_EXT,GL_MAX_EXT};
            glBlendEquation(c.blend.equation<5?equations[c.blend.equation]:GL_FUNC_ADD);
        }else glDisable(GL_BLEND);
    }
    i.vertices.clear();
    const auto convert=[&](const Vertex &v){
        unsigned col=(!c.has_vertex_color&&!c.lighting_enabled)?c.material_color:v.color;
        return GpuVertex{v.position[0],v.position[1],v.position[2],1,v.texcoord[0],v.texcoord[1],col,v.normal[0],v.normal[1],v.normal[2]};
    };
    GLenum primitive=GL_TRIANGLES;
    const std::size_t count=c.indices.empty()?c.vertices.size():c.indices.size();
    const auto at=[&](std::size_t n)->const Vertex&{return c.vertices[primitive_vertex(c.indices,c.vertices.size(),n)];};
    if(c.primitive==PrimitiveType::Sprites){
        for(std::size_t n=0;n+1<count;n+=2){
            Vertex a=at(n),b=at(n+1);a.position[2]=b.position[2];a.color=b.color;a.normal=b.normal;
            Vertex tr=b,bl=a;tr.position[1]=a.position[1];tr.texcoord[1]=a.texcoord[1];bl.position[1]=b.position[1];bl.texcoord[1]=b.texcoord[1];
            for(const auto &v:{a,tr,b,a,b,bl})i.vertices.push_back(convert(v));
        }
    }else{
        const GLenum primitives[]{GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_TRIANGLES,GL_TRIANGLE_STRIP,GL_TRIANGLE_FAN};
        primitive=primitives[static_cast<unsigned>(c.primitive)];
        for(std::size_t n=0;n<count;++n)i.vertices.push_back(convert(at(n)));
    }
    if(c.through){
        // The vertex shader carries a shared texel clamp through unused fields.
        // At the native PSP resolution the original texel endpoints suffice.
        for(auto &v:i.vertices){v.nx=-1e10f;v.ny=-1e10f;v.nz=1e10f;v.w=1e10f;}
    }
    glBindBuffer(GL_ARRAY_BUFFER,i.vbo);glBufferData(GL_ARRAY_BUFFER,static_cast<GLsizeiptr>(i.vertices.size()*sizeof(GpuVertex)),i.vertices.data(),GL_STREAM_DRAW);
    i.attributes();glDrawArrays(primitive,0,static_cast<GLsizei>(i.vertices.size()));
    checked("draw");dst.drawn=true;++i.draws;perf::count_draw();
}
void VulkanRenderer::write_back_frame(GuestMemory &memory){
    auto &i=*impl_;auto it=i.targets.find(i.display);if(it!=i.targets.end()&&it->second.drawn)i.readback(it->second,memory);
}
void VulkanRenderer::read_back_framebuffer(std::uint32_t source,GuestMemory &memory){
    auto &i=*impl_;source=address(source);
    for(auto &[a,t]:i.targets){unsigned bytes=t.state.color_format==3?4:2;
        if(t.drawn&&source>=a&&source<a+std::max(t.state.color_stride,kWidth)*kHeight*bytes){i.readback(t,memory);return;}}
}
bool VulkanRenderer::present(std::uint32_t display_address,std::optional<std::chrono::steady_clock::time_point>){
    auto &i=*impl_;++i.frames;if(!i.hold)i.display=address(display_address);
    if(i.fast && std::chrono::steady_clock::now()-i.last_present<kFastForwardPresentInterval)return false;
    i.blit(true);return true;
}
void VulkanRenderer::upload_frame(std::uint32_t a,const std::uint8_t *pixels,std::uint32_t w,std::uint32_t h,std::uint32_t stride){
    auto &i=*impl_;RenderTarget state{};state.color_address=a;state.color_format=3;auto &t=i.target(state);
    std::vector<unsigned char> scaled(static_cast<std::size_t>(i.width)*i.height*4);
    for(unsigned y=0;y<i.height;++y)for(unsigned x=0;x<i.width;++x)
        std::memcpy(&scaled[(static_cast<std::size_t>(y)*i.width+x)*4],pixels+(static_cast<std::size_t>(y*h/i.height)*stride+x*w/i.width)*4,4);
    glBindTexture(GL_TEXTURE_2D,t.tex);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,static_cast<GLsizei>(i.width),static_cast<GLsizei>(i.height),GL_RGBA,GL_UNSIGNED_BYTE,scaled.data());
    t.drawn=true;checked("movie upload");
}
bool VulkanRenderer::capture_frame(const std::string &path){
    auto &i=*impl_;auto it=i.targets.find(i.display);if(it==i.targets.end())return false;i.bind(it->second);
    std::vector<unsigned char> pixels(static_cast<std::size_t>(i.width)*i.height*4);
    glReadPixels(0,0,static_cast<GLsizei>(i.width),static_cast<GLsizei>(i.height),GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());checked("capture");
    return write_bmp(path,pixels.data(),i.width,i.height,false);
}
void VulkanRenderer::capture_window(const std::string &p){impl_->capture_pending=p;}
void VulkanRenderer::present_due(){}
void VulkanRenderer::present_until(std::chrono::steady_clock::time_point wake){std::this_thread::sleep_until(wake);}
void VulkanRenderer::pause_interpolation(){}
void VulkanRenderer::set_fast_forward(bool on){impl_->fast=on;}
void VulkanRenderer::set_frame_rate(settings::FrameRate rate){if(rate!=settings::FrameRate::Fps30)std::cout<<"[render] GLES2 presents the original 30 FPS; interpolation is unavailable\n";}
void VulkanRenderer::set_frame_rate_auto(bool){}
float VulkanRenderer::display_refresh()const noexcept{return 60;}
double VulkanRenderer::frame_rate_now()const noexcept{return 30;}
void VulkanRenderer::set_internal_scale(std::uint32_t scale){
    auto &i=*impl_;scale=std::clamp(scale,1u,4u);unsigned w=kWidth*scale,h=kHeight*scale;
    if(w!=i.width||h!=i.height){i.drop_targets();i.width=w;i.height=h;}
}
void VulkanRenderer::set_window_scale(std::uint32_t scale){SDL_SetWindowSize(impl_->window,static_cast<int>(kWidth*scale),static_cast<int>(kHeight*scale));}
void VulkanRenderer::set_fullscreen(bool full){SDL_SetWindowFullscreen(impl_->window,full);}
void VulkanRenderer::set_present_mode(settings::PresentMode mode){SDL_GL_SetSwapInterval(mode==settings::PresentMode::Immediate?0:1);}
bool VulkanRenderer::supports_present_mode(settings::PresentMode mode)const{return mode!=settings::PresentMode::Mailbox;}
void VulkanRenderer::set_aspect(settings::Aspect a){impl_->aspect=a;}
void VulkanRenderer::set_sharp_screen(bool s){impl_->sharp_screen=s;}
void VulkanRenderer::set_sharp_textures(bool s){impl_->sharp_textures=s;}
void VulkanRenderer::set_texture_pack(bool on){if(on)std::cout<<"[render] GLES2 uses the original game textures; HD replacements are not implemented\n";}
std::string VulkanRenderer::texture_pack_status()const{return "Unavailable in GLES2 prototype";}
void VulkanRenderer::reload_texture_pack(){}
void VulkanRenderer::hold_texture_pack(bool hold){impl_->held_pack=hold;}
bool VulkanRenderer::texture_pack_held()const{return impl_->held_pack;}
std::string VulkanRenderer::texture_pack_folder()const{return textures_root().string();}
std::filesystem::path VulkanRenderer::textures_root(){return install::user_data_directory()/"textures";}
void VulkanRenderer::set_perf_overlay(bool visible){impl_->perf_overlay=visible;}
float VulkanRenderer::game_aspect()const noexcept{return float(kWidth)/kHeight;}
std::array<std::uint32_t,2> VulkanRenderer::target_size()const noexcept{return {impl_->width,impl_->height};}
SDL_Window *VulkanRenderer::window()const noexcept{return impl_->window;}
std::string VulkanRenderer::device_name()const{return impl_->gpu;}
SDL_Gamepad *VulkanRenderer::gamepad()const noexcept{return impl_->gamepad;}
void VulkanRenderer::set_event_hook(std::function<bool(const SDL_Event&)> f){impl_->event_hook=std::move(f);}
void VulkanRenderer::set_game_input(bool on){if(on&&!impl_->game_input)impl_->suppress_held=true;impl_->game_input=on;}
void VulkanRenderer::set_pointer_free(bool free){impl_->pointer_free=free;}
void VulkanRenderer::set_scripted_key(int p,bool down){if(p>0&&p<int(input::kKeyPositions))impl_->scripted_keys[static_cast<std::size_t>(p)]=down;}
void VulkanRenderer::set_scripted_input(bool scripted){impl_->scripted_input=scripted;}
void VulkanRenderer::request_quit()noexcept{impl_->quit=true;}
void VulkanRenderer::hold_frame(bool hold){impl_->hold=hold;}
bool VulkanRenderer::initialize_ui(std::string &error){impl_->ui=ImGui_ImplOpenGL3_Init("#version 100");if(!impl_->ui)error="ImGui GLES2 initialization failed";return impl_->ui;}
void VulkanRenderer::shutdown_ui(){if(impl_->ui){ImGui_ImplOpenGL3_Shutdown();impl_->ui=false;}}
void VulkanRenderer::begin_ui_frame(){if(impl_->ui)ImGui_ImplOpenGL3_NewFrame();}
void VulkanRenderer::set_ui_draw_data(ImDrawData *data){impl_->ui_data=data;}
void VulkanRenderer::present_ui(bool show){impl_->blit(show);}
std::uint64_t VulkanRenderer::frames_presented()const noexcept{return impl_->frames;}
std::uint64_t VulkanRenderer::draws_submitted()const noexcept{return impl_->draws;}
CameraReading VulkanRenderer::camera()const noexcept{return impl_->camera;}
MouseMotion VulkanRenderer::take_mouse_motion()noexcept{return std::exchange(impl_->mouse_motion,MouseMotion{});}
bool VulkanRenderer::mouse_captured()const noexcept{return false;}
bool VulkanRenderer::touch_controls_visible()const noexcept{return false;}
const input::touch::Controls &VulkanRenderer::touch_controls()const{return impl_->touch;}
MouseMotion VulkanRenderer::take_touch_motion()noexcept{return {};}
bool VulkanRenderer::take_touch_menu()noexcept{return false;}
} // namespace mhp3rd::gpu
