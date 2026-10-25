#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

class QQuickView;
class QScreen;

/// 3D OSD 原型控制器（不含 3D 视频播放）。
///
/// 设计约束（与用户确认）：
///  - 右键菜单三态：0=不使用、1=3D片源、2=2D转3D；**运行期状态，不持久化**，
///    每次启动都是 0；
///  - 选中 1/2 后由 UI 选择眼镜对应的显示器；本类在所选屏幕全屏打开
///    StereoOsd.qml，把 OSD 在左右两半各画一份（SBS 双眼可读）；
///  - J/K/L/Tab 只更新 OSD 文本（布局/眼序/缩放/显隐），不触碰视频链；
///  - 退出 3D 不走 Esc：关闭 3D 窗口/切回 2D 后由 UI 调 stop()。
class StereoController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(int mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(int layout READ layout NOTIFY stateChanged)
    Q_PROPERTY(bool swap READ swap NOTIFY stateChanged)
    Q_PROPERTY(bool fit READ fit NOTIFY stateChanged)
    Q_PROPERTY(bool osdVisible READ osdVisible WRITE setOsdVisible NOTIFY osdVisibleChanged)
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)
    Q_PROPERTY(QString screenName READ screenName NOTIFY stateChanged)

public:
    explicit StereoController(QObject *parent = nullptr);
    ~StereoController() override;

    bool active() const { return active_; }
    int mode() const { return mode_; }
    int layout() const { return layout_; }
    bool swap() const { return swap_; }
    bool fit() const { return fit_; }
    bool osdVisible() const { return osdVisible_; }
    QVariantList screens() const { return screens_; }
    QString screenName() const { return screenName_; }

    /// 重新枚举屏幕（供选择器打开时调用；屏幕增删/分辨率变化也会自动刷新）。
    Q_INVOKABLE void refreshScreens();
    /// 开始 3D OSD 原型：mode 1/2，screenIndex = screens() 里的 index。
    Q_INVOKABLE void start(int mode, int screenIndex);
    Q_INVOKABLE void stop();

    // 3D 独有操作（原型只更新 OSD 文本）
    Q_INVOKABLE void cycleLayout();   // SBS / 上下
    Q_INVOKABLE void swapEyes();
    Q_INVOKABLE void toggleFit();     // 拉伸 / 保持比例
    Q_INVOKABLE void toggleOsd();
    Q_INVOKABLE void setOsdVisible(bool v);
    /// 测试用：把 3D OSD 窗口内容抓成 PNG（锁屏/无眼镜时也能检查渲染）。
    Q_INVOKABLE bool grabToFile(const QString &path);

signals:
    void stateChanged();
    void osdVisibleChanged();
    void screensChanged();

private:
    QScreen *screenAt(int index) const;
    void createOsdView(QScreen *screen);
    void destroyOsdView();

    QList<QScreen *> screenPtrs_;
    QVariantList screens_;
    QQuickView *osdView_ = nullptr;

    bool active_ = false;
    int mode_ = 0;
    int layout_ = 0;   // 0=SBS, 1=上下
    bool swap_ = false;
    bool fit_ = false;
    bool osdVisible_ = true;
    QString screenName_;
};
