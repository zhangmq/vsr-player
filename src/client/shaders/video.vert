#version 450

layout(location = 0) out vec2 frag_texcoord;

void main() {
    // Fullscreen triangle in clip space (no vertex buffer needed)
    vec2 pos = vec2(
        (gl_VertexIndex == 1 ? 3.0 : -1.0),
        (gl_VertexIndex == 2 ? 3.0 : -1.0)
    );
    gl_Position = vec4(pos, 0.0, 1.0);
    frag_texcoord = (pos + 1.0) * 0.5;
    frag_texcoord.y = 1.0 - frag_texcoord.y;
}
