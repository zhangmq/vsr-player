#version 450

layout(binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 frag_texcoord;
layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(texture(tex, frag_texcoord).rgb, 1.0);
}
