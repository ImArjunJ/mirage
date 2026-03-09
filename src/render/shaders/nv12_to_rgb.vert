#version 450

layout(push_constant) uniform PushConstants {
    vec4 dst_rect;
}
pc;

layout(location = 0) out vec2 frag_uv;

void main() {
    vec2 pos;
    if (gl_VertexIndex == 0) {
        pos = vec2(0.0, 0.0);
    } else if (gl_VertexIndex == 1) {
        pos = vec2(1.0, 0.0);
    } else if (gl_VertexIndex == 2) {
        pos = vec2(0.0, 1.0);
    } else if (gl_VertexIndex == 3) {
        pos = vec2(1.0, 0.0);
    } else if (gl_VertexIndex == 4) {
        pos = vec2(1.0, 1.0);
    } else {
        pos = vec2(0.0, 1.0);
    }

    frag_uv = pos;

    vec2 screen = pc.dst_rect.xy + pos * pc.dst_rect.zw;
    vec2 ndc = screen * 2.0 - 1.0;

    gl_Position = vec4(ndc, 0.0, 1.0);
}
