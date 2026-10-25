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
    int srcW;
    int srcH;
    int outW;
    int outH;
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

    // 全/半是“源眼输出像素尺寸”的区别，不是缩放到眼视口：
    //   全：SBS 源眼 = srcW/2 × srcH；TAB = srcW × srcH/2（不拉伸）
    //   半：源眼沿 split 轴拉成源整帧 = srcW × srcH（SBS 横向 2×、TAB 纵向 2×）
    // 例 1920x1080 SBS：全 960x1080，半 1920x1080。
    // 例  960x720  SBS：全  480x720，半  960x720。
    // 眼视口 = outW/2 × outH；图像按上述像素尺寸居中，不放大/不缩小。
    float eyeW = float(pc.outW) * 0.5;
    float eyeH = float(pc.outH);
    float imgW = float(pc.srcW);
    float imgH = float(pc.srcH);
    if (pc.halfMode == 0) {
        if (pc.layoutMode == 1)
            imgH *= 0.5;
        else
            imgW *= 0.5;
    }
    vec2 imgSize = vec2(imgW / eyeW, imgH / eyeH);
    vec2 imgMin = (vec2(1.0) - imgSize) * 0.5;
    vec2 local = (e - imgMin) / imgSize;
    if (local.x < 0.0 || local.x > 1.0 || local.y < 0.0 || local.y > 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 sampleUV = contentMin + (srcRect.xy + local * srcRect.zw) * contentSize;
    outColor = texture(srcTex, sampleUV);
}
