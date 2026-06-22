#pragma once

#include <QDialog>

#include "api/Player.h"

class QComboBox;

namespace vsr {

/// Settings dialog: VSR quality, output scale, etc.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    Quality quality() const;

private:
    QComboBox* quality_combo_ = nullptr;
};

}  // namespace vsr
