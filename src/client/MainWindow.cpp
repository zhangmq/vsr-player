#include "MainWindow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QSlider>
#include <QStringListModel>
#include <QVBoxLayout>

#include "PlayerProxy.h"
#include "VulkanWidget.h"

namespace vsr {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("VSR Player");
    resize(1280, 720);

    player_proxy_ = new PlayerProxy(this);
    setup_ui();
    setup_connections();
    connect_player_events();
}

MainWindow::~MainWindow() = default;

void MainWindow::setup_ui() {
    auto* central = new QWidget(this);
    setCentralWidget(central);

    auto* main_layout = new QHBoxLayout(central);

    // Left: playlist panel
    auto* left_panel = new QVBoxLayout;
    playlist_model_ = new QStringListModel(this);
    playlist_view_ = new QListView;
    playlist_view_->setModel(playlist_model_);
    playlist_view_->setFixedWidth(200);
    left_panel->addWidget(new QLabel("Playlist"));
    left_panel->addWidget(playlist_view_);
    main_layout->addLayout(left_panel);

    // Center: Vulkan render area
    vulkan_widget_ = new VulkanWidget(this);
    vulkan_widget_->setMinimumSize(640, 360);
    main_layout->addWidget(vulkan_widget_, 1);

    // Bottom bar
    auto* bottom_bar = new QHBoxLayout;
    play_btn_ = new QPushButton("▶");
    stop_btn_ = new QPushButton("■");
    seek_slider_ = new QSlider(Qt::Horizontal);
    status_label_ = new QLabel("Stopped");

    bottom_bar->addWidget(play_btn_);
    bottom_bar->addWidget(stop_btn_);
    bottom_bar->addWidget(seek_slider_, 1);
    bottom_bar->addWidget(status_label_);

    // Wrap in vertical layout
    auto* root = new QVBoxLayout;
    root->addLayout(main_layout, 1);
    root->addLayout(bottom_bar);
    central->setLayout(root);
}

void MainWindow::setup_connections() {
    connect(play_btn_, &QPushButton::clicked, this, &MainWindow::on_play_pause);
    connect(stop_btn_, &QPushButton::clicked, this, &MainWindow::on_stop);
    connect(seek_slider_, &QSlider::sliderMoved, this, &MainWindow::on_seek);
    connect(playlist_view_, &QListView::activated,
            this, &MainWindow::on_playlist_item_activated);
}

void MainWindow::connect_player_events() {
    // TODO: connect PlayerProxy signals to UI update slots
}

void MainWindow::open_file(const QString& path) {
    QStringList& list = playlist_model_->stringList();
    if (!list.contains(path)) {
        list.append(path);
        playlist_model_->setStringList(list);
    }
    // TODO: send LOAD command to player
}

void MainWindow::on_play_pause() {
    playing_ = !playing_;
    play_btn_->setText(playing_ ? "⏸" : "▶");
    // TODO: send PLAY/PAUSE command
}

void MainWindow::on_stop() {
    playing_ = false;
    play_btn_->setText("▶");
    // TODO: send STOP command
}

void MainWindow::on_seek(int64_t ms) {
    // TODO: send SEEK command
}

void MainWindow::on_quality_changed(int index) {
    // TODO: send SET_QUALITY command
}

void MainWindow::on_playlist_item_activated(const QModelIndex& index) {
    QString file = playlist_model_->data(index, Qt::DisplayRole).toString();
    // TODO: LOAD + PLAY
}

}  // namespace vsr
