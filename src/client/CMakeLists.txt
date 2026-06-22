add_executable(vsr-player
    main.cpp
    MainWindow.h MainWindow.cpp
    PlaylistPanel.h PlaylistPanel.cpp
    ControlBar.h ControlBar.cpp
    StatusBar.h StatusBar.cpp
    SettingsDialog.h SettingsDialog.cpp
    VulkanWidget.h VulkanWidget.cpp
    PlayerProxy.h PlayerProxy.cpp
)

target_link_libraries(vsr-player PRIVATE
    vsrplayer
    Qt6::Widgets
)

target_include_directories(vsr-player PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/api"
)
