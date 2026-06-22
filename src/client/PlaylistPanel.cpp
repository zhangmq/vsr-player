#include "PlaylistPanel.h"

#include <QListView>
#include <QStringListModel>
#include <QVBoxLayout>

namespace vsr {

PlaylistPanel::PlaylistPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    model_ = new QStringListModel(this);
    view_ = new QListView;
    view_->setModel(model_);
    layout->addWidget(view_);
}

void PlaylistPanel::add_file(const QString& path) {
    auto list = model_->stringList();
    if (!list.contains(path)) {
        list.append(path);
        model_->setStringList(list);
    }
}

void PlaylistPanel::add_files(const QStringList& paths) {
    auto list = model_->stringList();
    for (const auto& p : paths) {
        if (!list.contains(p)) list.append(p);
    }
    model_->setStringList(list);
}

QString PlaylistPanel::current_file() const {
    auto idx = view_->currentIndex();
    return model_->data(idx, Qt::DisplayRole).toString();
}

void PlaylistPanel::clear() {
    model_->setStringList({});
}

}  // namespace vsr
