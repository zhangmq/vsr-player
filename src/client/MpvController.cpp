#include "MpvController.h"
#include "Log.h"
#include <chrono>
static double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool MpvController::init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev,
                         uint32_t qfi, bool benchmark, bool hwaccel,
                         const char *vf_opt,
                         const std::vector<std::pair<std::string, std::string>> &passthrough,
                         const VkPhysicalDeviceFeatures2 *features,
                         const char *const *dev_exts, int num_dev_exts) {
    mpv_ = mpv_create();
    if (!mpv_) { MLOG_ERR("mpv_create failed"); return false; }
    MLOG_INFO("mpv_create ok");

    mpv_vulkan_init_params vkp = {};
    vkp.instance            = inst;
    vkp.physical_device     = pd;
    vkp.device              = dev;
    vkp.get_proc_addr       = (PFN_vkGetInstanceProcAddr)vkGetInstanceProcAddr;
    vkp.graphics_queue_family = (int)qfi;
    vkp.graphics_queue_index  = 0;
    vkp.graphics_queue_count  = 1;
    vkp.features            = const_cast<VkPhysicalDeviceFeatures2*>(features);
    vkp.extensions          = dev_exts;
    vkp.num_extensions      = num_dev_exts;

    mpv_set_option_string(mpv_, "vo", "libmpv");
    if (hwaccel)
        mpv_set_option_string(mpv_, "hwdec", "auto-unsafe");
    if (vf_opt && *vf_opt)
        mpv_set_option_string(mpv_, "vf", vf_opt);
    for (const auto &kv : passthrough)
        mpv_set_option_string(mpv_, kv.first.c_str(), kv.second.c_str());

    if (benchmark) {
        mpv_set_option_string(mpv_, "terminal", "no");
        mpv_set_option_string(mpv_, "audio", "no");
        mpv_set_option_string(mpv_, "untimed", "yes");
        mpv_set_option_string(mpv_, "video-sync", "display-desync");
        mpv_set_option_string(mpv_, "framedrop", "no");
        mpv_set_option_string(mpv_, "msg-level", "all=no");
    } else {
        // 默认只输出 info 及以上（规划：info=状态变更节点，warn/err/
        // fatal 默认可见）。调试需要更细日志时 --msg-level all=v|dbg。
        mpv_set_option_string(mpv_, "msg-level", "all=info");
        // 关闭 mpv 自身 OSD：seek 进度/音量条等由 Qt UI 呈现，
        // mpv 的 seek OSD（osd-level=1 默认）会叠加显示进度。
        mpv_set_option_string(mpv_, "osd-level", "0");
    }

    int advanced = 1;
    mpv_render_param mrp[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_VULKAN},
        {MPV_RENDER_PARAM_VULKAN_INIT_PARAMS, &vkp},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    double t1 = now();
    int e = mpv_render_context_create(&mctx_, mpv_, mrp);
    double t2 = now();
    if (e < 0) { MLOG_ERR("mpv_render_context_create: %s", mpv_error_string(e)); return false; }
    MLOG_INFO("render_context_create: %.0fms", (t2 - t1) * 1000.0);

    t1 = now();
    e = mpv_initialize(mpv_);
    t2 = now();
    if (e < 0) { MLOG_ERR("mpv_initialize: %s", mpv_error_string(e)); return false; }
    MLOG_INFO("mpv_initialize: %.0fms", (t2 - t1) * 1000.0);

    // mpv 内部日志 → MPV_EVENT_LOG_MESSAGE 事件（事件线程转发 stderr）：
    // 请求 info 及以上（与 --msg-level all=info 一致）；benchmark 请求
    // "no" 全静默（与 all=no 对齐）。不用 log-file——其过滤级别下限
    // 是 MSGL_DEBUG，会绕过 msg-level 把 verbose/debug 全灌进 stderr。
    mpv_request_log_messages(mpv_, benchmark ? "no" : "info");

    return true;
}

void MpvController::destroy() {
    // 摘除 update 回调——防御：render_context_free 期间 VO 线程仍可能
    // 触发 update（free 内部按 null 处理），不能打到外部对象。
    clearUpdateCallback();
    // Stop event thread first — it holds the only mpv_wait_event caller.
    eventsRunning_.store(false);
    if (eventThread_.joinable()) {
        mpv_wakeup(mpv_);  // interrupt mpv_wait_event
        eventThread_.join();
    }
    if (mctx_) { mpv_render_context_free(mctx_); mctx_ = nullptr; }
    if (mpv_)  { mpv_terminate_destroy(mpv_); mpv_ = nullptr; }
}

void MpvController::startEvents(std::function<void(mpv_event *)> handler) {
    eventHandler_ = std::move(handler);
    eventsRunning_.store(true);
    eventThread_ = std::thread(&MpvController::eventLoop, this);
}

void MpvController::stopEvents() {
    eventsRunning_.store(false);
    if (eventThread_.joinable()) {
        mpv_wakeup(mpv_);  // interrupt mpv_wait_event
        eventThread_.join();
    }
}

void MpvController::eventLoop() {
    // 周期任务（OSD 拉取推送等）按时间间隔节流调用——不能依赖
    // MPV_EVENT_NONE：播放中属性观察事件流密集（estimated-frame-count
    // 每解码一帧变化一次），wait_event 几乎总是立即返回事件不超时，
    // NONE 只在毫秒级间隙出现（实测 osdPoll 5s 仅触发 1 次）。
    auto lastPoll = std::chrono::steady_clock::now();
    while (eventsRunning_.load()) {
        mpv_event *ev = mpv_wait_event(mpv_, 0.1);
        auto now = std::chrono::steady_clock::now();
        if (idlePollCb_ && now - lastPoll >= std::chrono::milliseconds(100)) {
            idlePollCb_();   // 事件线程执行，调 mpv API 安全
            lastPoll = now;
        }
        if (ev->event_id == MPV_EVENT_NONE)
            continue;
        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            // mpv 内部日志转发 stderr（级别由 request_log_messages 过滤，
            // 默认 info 及以上；benchmark 请求 "no" 不产生该事件）
            auto *lm = (mpv_event_log_message *)ev->data;
            fprintf(stderr, "[mpv %s] %s", lm->level, lm->text);
            continue;
        }
        if (ev->event_id == MPV_EVENT_SHUTDOWN)
            break;
        if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto *pc = (mpv_event_property *)ev->data;
            // 同名属性可注册多个观察者（合法 mpv 用法），逐个分发
            for (const auto &o : propObs_) {
                if (o.name == pc->name) o.cb();
            }
            continue;
        }
        if (eventHandler_)
            eventHandler_(ev);
    }
}

// ── Generic property access ────────────────────────────────────────────

int64_t MpvController::propertyInt64(const char *name) {
    int64_t v = 0;
    mpv_get_property(mpv_, name, MPV_FORMAT_INT64, &v);
    return v;
}

double MpvController::propertyDouble(const char *name) {
    double v = 0;
    mpv_get_property(mpv_, name, MPV_FORMAT_DOUBLE, &v);
    return v;
}

bool MpvController::propertyFlag(const char *name) {
    int v = 0;
    mpv_get_property(mpv_, name, MPV_FORMAT_FLAG, &v);
    return v != 0;
}

std::string MpvController::propertyString(const char *name) {
    char *s = nullptr;
    if (mpv_get_property(mpv_, name, MPV_FORMAT_STRING, &s) < 0 || !s)
        return {};
    std::string out(s);
    mpv_free(s);
    return out;
}

bool MpvController::setPropertyString(const char *name, const std::string &value) {
    return mpv_set_property_string(mpv_, name, value.c_str()) >= 0;
}

bool MpvController::setPropertyDouble(const char *name, double value) {
    return mpv_set_property(mpv_, name, MPV_FORMAT_DOUBLE, &value) >= 0;
}

bool MpvController::setPropertyFlag(const char *name, bool value) {
    int v = value ? 1 : 0;
    return mpv_set_property(mpv_, name, MPV_FORMAT_FLAG, &v) >= 0;
}

// ── Async property set / command ───────────────────────────────────────
// 主线程调用不阻塞：请求投递到 mpv 核心队列异步执行。参数在调用时
// 同步复制（mpv_set_property_async → m_option_copy；mpv_command_async
// → mp_input_parse_cmd_strv），无保活需求。失败（投递错误）记日志。

bool MpvController::setPropertyDoubleAsync(const char *name, double value) {
    int e = mpv_set_property_async(mpv_, 0, name, MPV_FORMAT_DOUBLE, &value);
    if (e < 0)
        MLOG_ERR("setPropertyAsync(\"%s\") failed: %s", name, mpv_error_string(e));
    return e >= 0;
}

bool MpvController::setPropertyStringAsync(const char *name, const std::string &value) {
    int e = mpv_set_property_async(mpv_, 0, name, MPV_FORMAT_STRING, (void *)value.c_str());
    if (e < 0)
        MLOG_ERR("setPropertyAsync(\"%s\") failed: %s", name, mpv_error_string(e));
    return e >= 0;
}

void MpvController::commandAsync(std::initializer_list<const char *> args) {
    std::vector<const char *> v(args);
    v.push_back(nullptr);
    int e = mpv_command_async(mpv_, 0, v.data());
    if (e < 0) {
        MLOG_ERR("commandAsync failed: %s", mpv_error_string(e));
        for (const char *a : v) { if (!a) break; MLOG_ERR("  arg: %s", a); }
    }
}

// ── Generic commands ───────────────────────────────────────────────────

void MpvController::commandV(std::initializer_list<const char *> args) {
    std::vector<const char *> v(args);
    v.push_back(nullptr);
    mpv_command(mpv_, v.data());
}

void MpvController::commandStr(const char *cmd) {
    mpv_command_string(mpv_, cmd);
}

// ── Property observation ───────────────────────────────────────────────

void MpvController::observeProperty(const char *name, mpv_format format,
                                    PropertyObserver cb) {
    propObs_.push_back({name, format, std::move(cb)});
    mpv_observe_property(mpv_, 0, name, format);
}

uint64_t MpvController::update() {
    return mpv_render_context_update(mctx_);
}

void MpvController::render(VkImage img, VkFormat fmt, int w, int h) {
    mpv_vulkan_image vi = {img, fmt, w, h};
    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_VULKAN_IMAGE, &vi},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    mpv_render_context_render(mctx_, rp);
}

void MpvController::skipRender(VkImage img, VkFormat fmt, int w, int h) {
    mpv_vulkan_image vi = {img, fmt, w, h};
    int skip = 1;
    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_VULKAN_IMAGE, &vi},
        {MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    mpv_render_context_render(mctx_, rp);
}

void MpvController::reportSwap() {
    mpv_render_context_report_swap(mctx_);
}

void MpvController::loadFile(const char *path) {
    const char *cmd[] = {"loadfile", path, nullptr};
    mpv_command(mpv_, cmd);
}

void MpvController::setUpdateCallback(void (*cb)(void*), void *data) {
    mpv_render_context_set_update_callback(mctx_, cb, data);
}

void MpvController::clearUpdateCallback() {
    if (mctx_)
        mpv_render_context_set_update_callback(mctx_, nullptr, nullptr);
}
