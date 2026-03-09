#version 450

layout(set = 0, binding = 0) uniform sampler2D y_tex;
layout(set = 0, binding = 1) uniform sampler2D uv_tex;

layout(location = 0) in vec2 frag_uv;
layout(location = 0) out vec4 out_color;

void main() {
    float y = texture(y_tex, frag_uv).r;
    vec2 uv = texture(uv_tex, frag_uv).rg;
    float y_scaled = (y - 16.0/255.0) * (255.0/219.0);
    float cb = uv.r - 0.5;
    float cr = uv.g - 0.5;
    vec3 rgb;
    rgb.r = y_scaled + 1.5748 * cr;
    rgb.g = y_scaled - 0.1873 * cb - 0.4681 * cr;
    rgb.b = y_scaled + 1.8556 * cb;
    out_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
