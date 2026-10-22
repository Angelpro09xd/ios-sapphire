#version 450
// CoreAnimation-style layer quad: a rect in clip space, uv 0..1
layout(push_constant) uniform PC { vec4 rect; vec4 tint; } pc;
layout(location = 0) out vec2 uv;
void main() {
    vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    uv = c;
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, c), 0.0, 1.0);
}
