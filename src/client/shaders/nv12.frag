#version 450

layout(binding = 0) uniform sampler2D texY;
layout(binding = 1) uniform sampler2D texUV;

layout(location = 0) in vec2 frag_texcoord;
layout(location = 0) out vec4 out_color;

void main() {
    float y  = texture(texY, frag_texcoord).r;
    vec2  uv = texture(texUV, frag_texcoord).rg;

    // BT.601 full-range YUV -> RGB
    float u = uv.r - 0.5;
    float v = uv.g - 0.5;

    float r = clamp(y + 1.402 * v, 0.0, 1.0);
    float g = clamp(y - 0.34414 * u - 0.71414 * v, 0.0, 1.0);
    float b = clamp(y + 1.772 * u, 0.0, 1.0);

    out_color = vec4(r, g, b, 1.0);
}
