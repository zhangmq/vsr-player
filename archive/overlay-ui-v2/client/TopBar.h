#pragma once

#include <QWidget>
#include <QString>

namespace vsr {

class TopBar : public QWidget {
    Q_OBJECT

public:
    explicit TopBar(QWidget* parent = nullptr);

    void setFileName(const QString& name);

    void showBar();
    void hideBar();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QString fileName_;

    static constexpr int kHeight = 36;
    static constexpr int kHMargin = 12;
};

}  // namespace vsr
