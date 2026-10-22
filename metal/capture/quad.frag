#version 450
// textured, tinted, premultiplied: the bread and butter of a compositor
layout(push_constant) uniform PC { vec4 rect; vec4 tint; } pc;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() { color = texture(tex, uv) * pc.tint; }
