// Samples the render-thread VkImage via a combined image sampler.
#version 450
layout(binding = 0) uniform sampler2D srcTex;
layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = texture(srcTex, fragUV);
}
