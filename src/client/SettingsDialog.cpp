#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QVBoxLayout>

namespace vsr {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Settings");
    auto* layout = new QVBoxLayout(this);

    quality_combo_ = new QComboBox;
    quality_combo_->addItem("LOW", static_cast<int>(Quality::LOW));
    quality_combo_->addItem("MEDIUM", static_cast<int>(Quality::MEDIUM));
    quality_combo_->addItem("HIGH", static_cast<int>(Quality::HIGH));
    quality_combo_->addItem("ULTRA", static_cast<int>(Quality::ULTRA));
    quality_combo_->setCurrentIndex(2);  // HIGH default
    layout->addWidget(quality_combo_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

Quality SettingsDialog::quality() const {
    return static_cast<Quality>(quality_combo_->currentData().toInt());
}

}  // namespace vsr
