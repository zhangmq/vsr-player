# ---- Shader compilation to SPIR-V ----
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SPV_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SPV_DIR})

find_program(GLSLC glslc REQUIRED)

function(compile_shader NAME STAGE)
    set(SRC "${SHADER_DIR}/${NAME}")
    set(DST "${SPV_DIR}/${NAME}.spv")
    add_custom_command(
        OUTPUT ${DST}
        COMMAND ${GLSLC} -fshader-stage=${STAGE} ${SRC} -o ${DST}
        DEPENDS ${SRC}
        COMMENT "Compiling shader: ${NAME} (${STAGE})"
    )
    set(${NAME}_SPV ${DST} PARENT_SCOPE)
    # Also generate C header for embedding
    set(HEADER "${SPV_DIR}/${NAME}_spv.h")
    add_custom_command(
        OUTPUT ${HEADER}
        COMMAND xxd -i ${DST} > ${HEADER}
        DEPENDS ${DST}
        COMMENT "Generating SPIR-V header: ${NAME}_spv.h"
    )
    set(${NAME}_SPV_H ${HEADER} PARENT_SCOPE)
endfunction()

compile_shader(video.vert vert)
compile_shader(video.frag frag)

# ---- Executable ----
set(CMAKE_AUTOMOC ON)

add_executable(vsr-player
    main.cpp
    MainWindow.h MainWindow.cpp
    PlaylistPanel.h PlaylistPanel.cpp
    ControlBar.h ControlBar.cpp
    StatusBar.h StatusBar.cpp
    SettingsDialog.h SettingsDialog.cpp
    VulkanWidget.h VulkanWidget.cpp
    PlayerProxy.h PlayerProxy.cpp
    ${video_vert_SPV_H}
    ${video_frag_SPV_H}
)

target_link_libraries(vsr-player PRIVATE
    vsrplayer
    Qt6::Widgets
)

target_include_directories(vsr-player PRIVATE
    "${CMAKE_SOURCE_DIR}/src/core/api"
    "${SPV_DIR}"
)
