# vsr-player Qt Quick client — video playback via mpv + QSGRenderNode compositing
qt6 = import('qt6')

qt6_dep = [
    dependency('Qt6Quick'),
    dependency('Qt6Gui'),
    dependency('Qt6Core'),
]

vulkan_dep     = dependency('vulkan')
libplacebo_dep = dependency('libplacebo')

# MPV_BUILD_DIR（env）：dist 构建指向独立 mpv 构建树（build_release.sh 用
# build/mpv-dist/_build）；默认 dev 构建树 build/mpv/_build
# mpv-build-dir option：dist 构建指向独立 mpv 构建树（build_release.sh 用
# -Dmpv-build-dir=build/mpv-dist/_build）；默认 dev 构建树 build/mpv/_build
mpv_build_dir = get_option('mpv-build-dir')
if mpv_build_dir == ''
    mpv_build_dir = meson.project_source_root() / 'build/mpv/_build'
endif
# dist-rpath option：发布打包时 libmpv/依赖库走 $ORIGIN 相对捆绑目录
# （build_release.sh 传 -Ddist-rpath='$ORIGIN/../lib/vsr-player'）；空 = 开发默认
# （build 树 libmpv）
mpv_rpath = get_option('dist-rpath')
if mpv_rpath == ''
    mpv_rpath = mpv_build_dir
endif
mpv_dep = declare_dependency(
    link_args: ['-L' + mpv_build_dir, '-lmpv', '-Wl,-rpath,' + mpv_rpath],
)

# ── Shader compilation ─────────────────────────────────────────────────
glslc  = find_program('glslc', required: true)
sh_cmd = [find_program('sh'), '-c',
          'xxd -i "$1" | sed "s|src_client_||g" > "$2"', '--', '@INPUT@', '@OUTPUT@']

comp_vert_spv = custom_target('comp_vert_spv',
    input: files('shaders/composite.vert'), output: 'composite.vert.spv',
    command: [glslc, '-c', '@INPUT@', '-o', '@OUTPUT@'])
comp_frag_spv = custom_target('comp_frag_spv',
    input: files('shaders/composite.frag'), output: 'composite.frag.spv',
    command: [glslc, '-c', '@INPUT@', '-o', '@OUTPUT@'])

c_vert_h = custom_target('composite_vert_spv_h',
    input: comp_vert_spv, output: 'composite_vert_spv.h', command: sh_cmd)
c_frag_h = custom_target('composite_frag_spv_h',
    input: comp_frag_spv, output: 'composite_frag_spv.h', command: sh_cmd)

# ── Translations (.ts → .qm, loaded from VSR_TRANSLATIONS_DIR) ────────
# meson qt6 module 无 add_resources，.qm 由 lrelease 生成到构建目录，
# main.cpp 经 VSR_TRANSLATIONS_DIR 宏加载（dev 构建路径；打包时可改）。
lrelease = find_program('/usr/lib/qt6/bin/lrelease', required: true)
translations_qm = []
foreach ts : ['vsr-player_en', 'vsr-player_zh_CN']
    translations_qm += custom_target(ts + '.qm',
        input: files('translations/' + ts + '.ts'),
        output: ts + '.qm',
        command: [lrelease, '@INPUT@', '-qm', '@OUTPUT@'])
endforeach
translations_dir = meson.project_build_root() / 'src/client'
add_project_arguments('-DVSR_TRANSLATIONS_DIR="' + translations_dir + '"',
                      language: 'cpp')

# 图标字体随项目分发（third_party），不依赖系统安装
# Material Design Icons (Pictogrammers) 7.4.47 — 补充平面 PUA-B 码点（U+F0000+）
add_project_arguments('-DVSR_ICON_FONT="' +
                      meson.project_source_root() / 'third_party/material-icons/materialdesignicons-webfont.ttf' + '"',
                      language: 'cpp')

# ── MOC ────────────────────────────────────────────────────────────────
processed = qt6.preprocess(
    moc_headers: ['Video.h', 'PlayerViewModel.h', 'PlaylistModel.h'],
    qresources: files('resources.qrc'),
    dependencies: qt6_dep,
)

# ── Executable ─────────────────────────────────────────────────────────
executable('vsr-player',
    'main.cpp', 'Options.cpp', 'VulkanContext.cpp',
    'CompositePipeline.cpp', 'MpvController.cpp', 'Video.cpp',
    'PlayerViewModel.cpp', 'PlaylistModel.cpp', 'RpcServer.cpp', 'Log.cpp',
    c_vert_h, c_frag_h, translations_qm, processed,
    dependencies: [qt6_dep, vulkan_dep, libplacebo_dep, mpv_dep],
    include_directories: include_directories(
        '../../src/mpv/include',
        '../../third_party/mpv/include',
    ),
    cpp_args: ['-std=c++20', '-g', '-O0', '-fPIC'],
    install: false,
)
