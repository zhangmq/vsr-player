#pragma once

#include <QCursor>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QString>
#include <QVariantList>

/// 3D 输出控制器（3D 视频映射 + 独立 presentation 窗口）。
///
/// 设计约束（2026-10-25 用户确认）：
///  - 主 UI 始终在桌面；3D 时主窗口（Video/VO）作为 presentation 移到眼镜屏，
///    满屏、置顶、无其他可见内容；UI 由一个独立的 control 窗口继续显示在桌面；
///  - 屏拆分只在 VO/合成阶段做，vf_*（VSR/VFG/TrueHDR）链路不动；
///  - 不做切回 2D 自动结束；不假设 1080×2/特定眼镜；
///  - 3D 三态运行期状态、不持久化；屏幕记忆只做选择器默认选中。
class StereoController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(int mode READ mode NOTIFY stateChanged)
    Q_PROPERTY(int layout READ layout NOTIFY stateChanged)
    Q_PROPERTY(bool swap READ swap NOTIFY stateChanged)
    Q_PROPERTY(bool half READ half NOTIFY stateChanged)
    Q_PROPERTY(bool helpVisible READ helpVisible WRITE setHelpVisible NOTIFY helpVisibleChanged)
    Q_PROPERTY(QVariantList screens READ screens NOTIFY screensChanged)
    Q_PROPERTY(QString screenName READ screenName NOTIFY stateChanged)

public:
    explicit StereoController(QObject *parent = nullptr);
    ~StereoController() override;

    bool active() const { return active_; }
    int mode() const { return mode_; }
    int layout() const { return layout_; }
    bool swap() const { return swap_; }
    bool half() const { return half_; }
    bool helpVisible() const { return helpVisible_; }
    QVariantList screens() const { return screens_; }
    QString screenName() const { return screenName_; }

    /// presentation = 主窗口（Video/VO），control = 桌面 UI 窗口。
    void setPlayerWindow(QQuickWindow *w) { playerWin_ = w; }
    void setControlWindow(QQuickWindow *w) { controlWin_ = w; }

    Q_INVOKABLE void refreshScreens();
    /// 上次选择的屏幕在 screens() 里的 index；只用于选择器默认选中。
    Q_INVOKABLE int preferredScreenIndex() const;
    /// mode 1 = 3D片源；mode 2（2D转3D）本期不启动，返回不做任何事。
    Q_INVOKABLE void start(int mode, int screenIndex);
    Q_INVOKABLE void stop();

    Q_INVOKABLE void cycleLayout();   // SBS / 上下
    Q_INVOKABLE void swapEyes();
    Q_INVOKABLE void toggleHalf();    // SBS：全宽/半宽；TAB：全高/半高
    Q_INVOKABLE void toggleHelp();
    Q_INVOKABLE void setHelpVisible(bool v);

signals:
    void stateChanged();
    void helpVisibleChanged();
    void screensChanged();

private:
    QScreen *screenAt(int index) const;

    QList<QScreen *> screenPtrs_;
    QVariantList screens_;
    QPointer<QQuickWindow> playerWin_;
    QPointer<QQuickWindow> controlWin_;

    bool active_ = false;
    int mode_ = 0;
    int layout_ = 0;   // 0=SBS, 1=上下
    bool swap_ = false;
    bool half_ = false;  // false=全，true=半（按 split 轴）
    bool helpVisible_ = true;
    QString screenName_;

    QPointer<QScreen> origScreen_;
    QRect origGeom_;
    bool origFullScreen_ = false;
    Qt::WindowFlags origFlags_;
    QCursor origCursor_;

    QSettings settings_{QStringLiteral("vsr-player"), QStringLiteral("vsr-player")};
    QString preferredScreenName_;
};
