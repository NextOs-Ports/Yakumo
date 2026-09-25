#version 100
precision highp float;
precision highp int;
attribute vec4 in_position;
attribute vec2 in_texcoord;
attribute vec4 in_color;
attribute vec3 in_normal;
uniform vec2 ndc_sign;
varying vec2 frag_texcoord;
varying vec4 frag_color;
varying vec3 frag_specular;
varying float frag_fog;
// Through-mode tiles: the texture coordinates the tile may sample, as min.xy,
// max.xy. The vertex carries them in texels, in in_normal.xy and
// (in_normal.z, in_position.w), which through-mode vertices do not use
// otherwise; see clamp_through_quads() in vulkan_renderer.cpp.
varying vec4 frag_uv_rect;

uniform mat4 push_transform;
uniform vec4 push_viewport;
uniform vec4 push_texture_params;
uniform vec4 push_uv_transform;
uniform vec4 push_view_z;

// The lighting environment, shared by every draw until the game changes it;
// the layout matches EnvironmentBlock on the host. Colours are 0..1; small
// integers are stored as floats.
uniform vec4 lighting_ambient;
uniform vec4 lighting_fog;
uniform vec4 lighting_fog_color;
uniform vec4 lighting_light_position[4];
uniform vec4 lighting_light_direction[4];
uniform vec4 lighting_light_attenuation[4];
uniform vec4 lighting_light_spot[4];
uniform vec4 lighting_light_ambient[4];
uniform vec4 lighting_light_diffuse[4];
uniform vec4 lighting_light_specular[4];

// A lit draw's world matrix and material; the layout matches ObjectBlock.
uniform mat4 object_world;
uniform vec4 object_flags;
uniform vec4 object_emissive;
uniform vec4 object_material_ambient;
uniform vec4 object_material_diffuse;
uniform vec4 object_material_specular;

// The GE's per-vertex lighting, evaluated in world space: emissive, plus the
// global ambient light times the material ambient, plus for each enabled light
// its ambient, diffuse and specular terms, scaled by distance attenuation and
// the spot cone. The material update mask makes the vertex colour stand in for
// the ambient (bit 0), diffuse (bit 1) and specular (bit 2) material colours;
// a vertex without a colour keeps the material ones.
void light_vertex(out vec4 color, out vec3 separate_specular) {
    int mask = int(object_flags.w + 0.5);
    bool has_color = object_flags.y > 0.5;
    vec4 ambient_material = (has_color && mod(floor(float(mask) / 1.0), 2.0) > 0.5) ? in_color : object_material_ambient;
    vec3 diffuse_material = (has_color && mod(floor(float(mask) / 2.0), 2.0) > 0.5) ? in_color.rgb : object_material_diffuse.rgb;
    vec3 specular_material = (has_color && mod(floor(float(mask) / 4.0), 2.0) > 0.5) ? in_color.rgb : object_material_specular.rgb;
    float power = object_emissive.w;

    vec3 world_position = (object_world * vec4(in_position.xyz, 1.0)).xyz;
    // Skinned normals come out of the bone matrices far from unit length, so
    // the GE's normalisation after the transform matters.
    vec3 normal = mat3(object_world) * in_normal;
    float length_squared = dot(normal, normal);
    normal = length_squared > 0.0 ? normal * inversesqrt(length_squared) : vec3(0.0, 0.0, 1.0);
    if (object_material_specular.w > 0.5) normal = -normal;

    vec3 sum = object_emissive.rgb + lighting_ambient.rgb * ambient_material.rgb;
    vec3 specular = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        if (lighting_light_position[i].w < 0.5) continue;
        int type = int(lighting_light_direction[i].w + 0.5);
        int kind = int(lighting_light_attenuation[i].w + 0.5);
        vec3 to_light = lighting_light_position[i].xyz;
        float scale = 1.0;
        if (type != 0) {
            to_light -= world_position;
            float distance = length(to_light);
            vec3 k = lighting_light_attenuation[i].xyz;
            scale = clamp(1.0 / max(k.x + k.y * distance + k.z * distance * distance, 1e-20), 0.0, 1.0);
        }
        to_light = dot(to_light, to_light) > 0.0 ? normalize(to_light) : vec3(0.0, 0.0, 1.0);
        if (type == 2) {
            vec3 axis = lighting_light_direction[i].xyz;
            axis = dot(axis, axis) > 0.0 ? normalize(axis) : vec3(0.0, 0.0, 1.0);
            float angle = dot(axis, -to_light);
            scale *= angle >= lighting_light_spot[i].y ? pow(max(angle, 0.0), lighting_light_spot[i].x) : 0.0;
        }
        float n_dot_l = dot(normal, to_light);
        float diffuse = max(n_dot_l, 0.0);
        if (kind == 2) diffuse = pow(diffuse, power);
        sum += (lighting_light_ambient[i].rgb * ambient_material.rgb +
                lighting_light_diffuse[i].rgb * diffuse_material * diffuse) * scale;
        if (kind == 1 && n_dot_l >= 0.0) {
            // The viewer is taken to look down z, as the GE does.
            vec3 half_vector = normalize(to_light + vec3(0.0, 0.0, 1.0));
            specular += lighting_light_specular[i].rgb * specular_material *
                        pow(max(dot(normal, half_vector), 0.0), power) * scale;
        }
    }
    float alpha = lighting_ambient.a * ambient_material.a;
    if (object_material_diffuse.w > 0.5) {
        separate_specular = clamp(specular, 0.0, 1.0);
    } else {
        sum += specular;
        separate_specular = vec3(0.0);
    }
    color = clamp(vec4(sum, alpha), 0.0, 1.0);
}


void main() {

    frag_texcoord = in_texcoord * push_uv_transform.xy + push_uv_transform.zw;
    frag_color = in_color;
    frag_specular = vec3(0.0);
    frag_fog = 1.0;
    frag_uv_rect = vec4(-1e30, -1e30, 1e30, 1e30);
    if (push_viewport.z > 0.5) {
        vec4 rect = vec4(in_normal.xy, in_normal.z, in_position.w);
        frag_uv_rect = vec4(rect.xy * push_uv_transform.xy + push_uv_transform.zw,
                            rect.zw * push_uv_transform.xy + push_uv_transform.zw);
        // Screen-space vertices: pixels to clip space.
        vec2 ndc = vec2(in_position.x / push_viewport.x, in_position.y / push_viewport.y) * 2.0 - 1.0;
        gl_Position = vec4(ndc, clamp(in_position.z / 65535.0, 0.0, 1.0) * 2.0 - 1.0, 1.0);
    } else {
        int enables = int(push_viewport.w + 0.5);
        if (mod(floor(float(enables) / 2.0), 2.0) > 0.5) light_vertex(frag_color, frag_specular);
        // Fog runs linearly from 1 (clear) to 0 (fogged) with view-space z,
        // which is negative in front of the camera: (z + end) * scale.
        if (mod(floor(float(enables) / 1.0), 2.0) > 0.5)
            frag_fog = (dot(push_view_z, vec4(in_position.xyz, 1.0)) + lighting_fog.x) * lighting_fog.y;
        vec4 clip = push_transform * vec4(in_position.xyz, 1.0);
        // PSP clip space follows OpenGL with z in [-w, w]; Vulkan clips against
        // [0, w], so without this remap the near half of every frustum is lost.
        // The PSP viewport's z scale and offset are folded into the Vulkan
        // viewport's min/max depth, which expects this [0, 1] device z.
        clip.xy *= ndc_sign;
        gl_Position = clip;
    }
}
