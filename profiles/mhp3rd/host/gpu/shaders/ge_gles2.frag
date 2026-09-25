#version 100
precision highp float;
precision highp int;

varying vec2 frag_texcoord;
varying vec4 frag_color;
varying vec3 frag_specular;
varying float frag_fog;
varying vec4 frag_uv_rect;


uniform sampler2D guest_texture;

// False for pipelines of draws without an alpha test (texture_params.w 0):
// the discard below is then compiled out, so tiled GPUs keep their early
// depth test and hidden surface removal for them.
uniform bool kAlphaTest;

uniform mat4 push_transform;
uniform vec4 push_viewport;
uniform vec4 push_texture_params;
uniform vec4 push_uv_transform;
uniform vec4 push_view_z;

// Only the fog colour is read here; the block is described in ge.vert.
uniform vec4 lighting_ambient;
uniform vec4 lighting_fog;
uniform vec4 lighting_fog_color;

void main() {
    vec4 color = frag_color;
    if (push_texture_params.x > 0.5) {
        vec4 texel = texture2D(guest_texture, clamp(frag_texcoord, frag_uv_rect.xy, frag_uv_rect.zw));
        int function = int(push_texture_params.y + 0.5);
        if (function == 0) {          // modulate
            color *= texel;
        } else if (function == 1) {   // decal
            color = vec4(mix(color.rgb, texel.rgb, texel.a), color.a);
        } else if (function == 2) {   // blend
            color = vec4(mix(color.rgb, texel.rgb, texel.rgb), color.a * texel.a);
        } else {                      // replace and everything else
            color = texel;
        }
    }

    // A separate specular term is added after texturing, then fog blends
    // towards its colour; neither touches alpha.
    color.rgb = min(color.rgb + frag_specular, vec3(1.0));
    if (mod(floor(push_viewport.w + 0.5), 2.0) > 0.5) color.rgb = mix(lighting_fog_color.rgb, color.rgb, clamp(frag_fog, 0.0, 1.0));

    // PSP alpha test, evaluated per fragment.
    if (!kAlphaTest) {
        gl_FragColor = color;
        return;
    }
    int alpha_function = int(push_texture_params.w + 0.5);
    float reference = push_texture_params.z / 255.0;
    float alpha = color.a;
    bool passed = true;
    if (alpha_function == 1) passed = false;                     // never
    else if (alpha_function == 2) passed = abs(alpha - reference) < 0.002;
    else if (alpha_function == 3) passed = abs(alpha - reference) >= 0.002;
    else if (alpha_function == 4) passed = alpha < reference;
    else if (alpha_function == 5) passed = alpha <= reference;
    else if (alpha_function == 6) passed = alpha > reference;
    else if (alpha_function == 7) passed = alpha >= reference;
    if (!passed) discard;

    gl_FragColor = color;
}
