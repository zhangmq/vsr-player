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

    // 输出窗口左右半 = 左/右眼。分屏边界必须落在**整数像素**上：Wayland
    // 分数缩放会让 framebuffer 宽成为奇数（本机 2844×1.35 = 3839），若按
    // uv.x < 0.5 分屏，两眼采样网格会差半个像素 → 整幅画面恒定水平视差
    // （左眼与右眼看到的同一物体相差 0.5px，等于给所有内容加了一个会聚偏移）。
    // 这里左眼固定 [0, splitX)、右眼 [splitX, outW)，两眼都用 splitX 作为
    // 眼宽，偶数宽时 splitX = outW/2 与原来完全一致；奇数宽时最右一列是
    // 多出来的，夹取边缘采样（复制右眼最后一列）而不是留一条黑缝。
    int splitX = max(pc.outW / 2, 1);
    float eyeW = float(splitX);
    float eyeH = float(pc.outH);
    float px = uv.x * float(pc.outW);              // 窗口像素坐标（中心 = i+0.5）
    int eye = (px < eyeW) ? 0 : 1;
    float ex = (px - float(eye) * eyeW) / eyeW;    // 眼内归一化横坐标
    vec2 e = vec2(clamp(ex, 0.0, 1.0), uv.y);
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
    // 眼视口 = splitX × outH（上面已算，两眼同宽）。全/半先决定源眼图像
    // 的原生宽高比，最后再按眼窗口大小做 contain 等比缩放（可放大/缩小、
    // 不拉伸），居中。全/半与几何都是整数比值，两眼取值完全相同。
    float imgW = float(pc.srcW);
    float imgH = float(pc.srcH);
    if (pc.halfMode == 0) {
        if (pc.layoutMode == 1)
            imgH *= 0.5;
        else
            imgW *= 0.5;
    }
    // 源尺寸未知（早期帧/未装载）→ 不映射，按 2D 直采，避免 0 除。
    if (imgW < 1.0 || imgH < 1.0) {
        outColor = texture(srcTex, uv);
        return;
    }
    float scale = min(eyeW / imgW, eyeH / imgH);
    vec2 imgSize = vec2(imgW * scale / eyeW, imgH * scale / eyeH);
    vec2 imgMin = (vec2(1.0) - imgSize) * 0.5;
    vec2 local = (e - imgMin) / imgSize;
    if (local.x < 0.0 || local.x > 1.0 || local.y < 0.0 || local.y > 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 sampleUV = contentMin + (srcRect.xy + local * srcRect.zw) * contentSize;
    outColor = texture(srcTex, sampleUV);
}
