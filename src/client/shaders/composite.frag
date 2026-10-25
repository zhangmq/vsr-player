// Samples the render-thread VkImage via a combined image sampler.
//
// 2D：原样采样。3D：输出窗口左右半 = 左右眼；源帧在 rtImage 里是 mpv
// 按源比例 letterbox 后的内容。每只眼取源半幅，按 split 轴的全/半选一个
// 居中的 box，源半幅保持自身比例（contain）放进 box 并居中。
#version 450
layout(binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    int enabled;
    int layoutMode; // 0=SBS 1=上下
    int swap;
    int halfMode;   // 0=全 1=半
    float srcAspect;
    float outAspect;
} pc;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = fragUV;
    if (pc.enabled == 0) {
        outColor = texture(srcTex, uv);
        return;
    }

    // mpv 在 rtImage 里按源整帧比例居中 letterbox 后的内容矩形。
    vec2 contentSize = (pc.srcAspect > pc.outAspect)
        ? vec2(1.0, pc.outAspect / pc.srcAspect)
        : vec2(pc.srcAspect / pc.outAspect, 1.0);
    vec2 contentMin = (vec2(1.0) - contentSize) * 0.5;

    // 输出窗口左右半 = 左/右眼。
    int eye = (uv.x < 0.5) ? 0 : 1;
    vec2 e = vec2((uv.x - float(eye) * 0.5) * 2.0, uv.y);
    int srcEye = (pc.swap != 0) ? (1 - eye) : eye;

    vec4 srcRect; // 源整帧归一化坐标里的眼半幅
    if (pc.layoutMode == 1) {
        srcRect = vec4(0.0, float(srcEye) * 0.5, 1.0, 0.5);
    } else {
        srcRect = vec4(float(srcEye) * 0.5, 0.0, 0.5, 1.0);
    }

    // Full：源眼保持自身比例，contain 到整个眼视口（可能留黑边）。
    // 例：1920x1080 SBS 的源眼 960x1080 原样输出为 960x1080。
    // Half：源眼沿 split 轴拉伸到整个眼视口（anamorphic 展开），
    // 例：960x1080 横向拉成 1920x1080；TAB 则是纵向拉满。
    if (pc.halfMode != 0) {
        vec2 local = e;
        vec2 sampleUV = contentMin + (srcRect.xy + local * srcRect.zw) * contentSize;
        outColor = texture(srcTex, sampleUV);
        return;
    }

    vec2 b = e;
    float srcHalfAspect = (pc.srcAspect * srcRect.z) / srcRect.w;
    // 输出 framebuffer 恒为 SBS：每只眼的视口 = 半宽 × 全高。
    float eyeAspect = pc.outAspect * 0.5;
    float boxAspect = eyeAspect;
    vec2 cscale;
    if (srcHalfAspect > boxAspect)
        cscale = vec2(1.0, boxAspect / srcHalfAspect);
    else
        cscale = vec2(srcHalfAspect / boxAspect, 1.0);
    vec2 coff = (vec2(1.0) - cscale) * 0.5;
    if (b.x < coff.x || b.x > coff.x + cscale.x ||
        b.y < coff.y || b.y > coff.y + cscale.y) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 local = (b - coff) / cscale;
    vec2 sampleUV = contentMin + (srcRect.xy + local * srcRect.zw) * contentSize;
    outColor = texture(srcTex, sampleUV);
}
