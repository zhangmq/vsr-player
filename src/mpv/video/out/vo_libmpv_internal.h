// 内部共享接口：f_output_chain 非阻塞查询 vo_libmpv 渲染目标尺寸。
//
// 背景（2026-08-06，fs_stress 实测死锁）：vf_vsr 的 resolve_scale
//（scale=auto）在 filter 处理线程调 get_render_target_size → vo_control
// 同步 dispatch 等 VO 队列——VO dispatch 由 mpv_render_context_render
//（客户端渲染循环）泵，渲染循环等 core 的 update 心跳，core 卡在 filter
// 处理等 VO——三方互相等待，永久死锁。vo_control 无超时/非阻塞变体，
// 故 getter 直接读 vo_libmpv 的渲染目标尺寸缓存（render 时从 render
// params 提取更新），首帧缓存未就绪返回 0，resolve_scale 以
// effective_scale 兜底、后续帧再决策。
#ifndef MPV_VIDEO_OUT_VO_LIBMPV_INTERNAL_H_
#define MPV_VIDEO_OUT_VO_LIBMPV_INTERNAL_H_

struct vo;

// 非阻塞读取渲染目标尺寸（等价 VOCTRL_GET_RENDER_TARGET_SIZE 的缓存版）。
// 未渲染过（缓存未就绪）时 res 置 0。
void mpv_render_context_get_target_size(struct vo *vo, int *res);

#endif
