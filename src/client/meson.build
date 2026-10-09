# vsr-player Qt Quick client — video playback via mpv + QSGRenderNode compositing
qt6 = import('qt6')

qt6_dep = [
    dependency('Qt6Quick'),
    dependency('Qt6Gui'),
    dependency('Qt6Core'),
]

# QtDBus：播放期防息屏走会话总线 org.freedesktop.ScreenSaver（SessionIdle.cpp）。
# 可选依赖——缺失时不定义 HAVE_QTDBUS，该功能降级为 no-op（只警告一次）。
qt6_dbus = dependency('Qt6DBus', required: false)
if qt6_dbus.found()
    qt6_dep += qt6_dbus
    add_project_arguments('-DHAVE_QTDBUS', language: 'cpp')
endif

vulkan_dep     = dependency('vulkan')
libplacebo_dep = dependency('libplacebo')

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

# ── Wayland 空闲抑制模块（单独 .so，主程序运行时 dlopen）────────────────
# 协议实现需要 QtWaylandClient（在 Qt 的 registry 上安全 bind 全局）与 qtbase
# 私有头（取窗口 wl_surface）。硬链进主程序会让未装 qt6-wayland 的系统
#（GNOME/XFCE 上 Qt 应用常走 XWayland）启动失败，故隔离成模块：加载失败即
# 回退 D-Bus（见 SessionIdle.cpp / WaylandIdleInhibit.h）。
# 四件套齐备才构建：wayland-scanner + wayland-protocols(XML) + wayland-client
# + Qt6WaylandClient(含私有头)。任一缺失 → 不生成模块（功能降级，不影响构建）。
_fs = import('fs')
_cpp = meson.get_compiler('cpp')
_wl_scanner   = find_program('wayland-scanner', required: false)
_wl_protocols = dependency('wayland-protocols', required: false)
_wl_client    = dependency('wayland-client', required: false)
_qt6_wl       = dependency('Qt6WaylandClient', required: false)
_qt6_inc      = dependency('Qt6Core').get_variable(pkgconfig: 'includedir')
# qtbase 私有头目录：**不要**用 dependency('Qt6Core').version() 推——实测本机
# Qt6Core.pc 报 6.11.1 而私有头装在 6.12.0/（Qt 打包版本号不同步），按版本
# 拼路径会静默失配。改为直接探测两种安装形态：
#   <inc>/QtGui/<ver>/QtGui/{qpa,private/qpa}/qplatformwindow_p.h
#   <inc>/QtCore/<ver>/QtCore/private/qglobal_p.h
_qt6_priv_inc = []
foreach _m : ['QtGui', 'QtCore']
    if _m == 'QtGui'
        _pat = _qt6_inc + '/QtGui/*/QtGui'
        _test = '{ [ -f "$d/qpa/qplatformwindow_p.h" ] || [ -f "$d/private/qpa/qplatformwindow_p.h" ]; }'
    else
        _pat = _qt6_inc + '/QtCore/*/QtCore'
        _test = '[ -f "$d/private/qglobal_p.h" ]'
    endif
    _cmd = 'for d in ' + _pat + '; do ' + _test + ' && echo "$(dirname "$d")" && break; done'
    _r = run_command('sh', '-c', _cmd, check: false)
    if _r.returncode() == 0 and _r.stdout().strip() != ''
        _qt6_priv_inc += '-I' + _r.stdout().strip()
    endif
endforeach
_idle_xml = ''
if _wl_protocols.found()
    _idle_xml = _wl_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'
endif

_qt6_gui_dep = dependency('Qt6Gui')
_have_priv_hdr = _cpp.has_header('QtGui/qpa/qplatformwindow_p.h', dependencies: [_qt6_gui_dep], args: _qt6_priv_inc) or _cpp.has_header('QtGui/private/qpa/qplatformwindow_p.h', dependencies: [_qt6_gui_dep], args: _qt6_priv_inc)

if _wl_scanner.found() and _wl_client.found() and _qt6_wl.found() and _fs.exists(_idle_xml) and _have_priv_hdr
    _wl_hdr = custom_target('idle-inhibit-client-header',
        output: 'idle-inhibit-unstable-v1-client-protocol.h',
        command: [_wl_scanner, 'client-header', _idle_xml, '@OUTPUT@'])
    _wl_code = custom_target('idle-inhibit-protocol-code',
        output: 'idle-inhibit-unstable-v1-protocol.c',
        command: [_wl_scanner, 'private-code', _idle_xml, '@OUTPUT@'])
    shared_module('vsr-idle-wayland',
        'WaylandIdleInhibit.cpp', _wl_hdr, _wl_code,
        dependencies: [dependency('Qt6Core'), dependency('Qt6Gui'), _qt6_wl, _wl_client],
        include_directories: include_directories('.'),
        # 发布布局下 Qt 可能就在 $ORIGIN/..（full 变体 <prefix>/lib），
        # 该模块被 dlopen，rpath 必须自带（调用方的 RUNPATH 不传递给 dlopen 的依赖）。
        build_rpath: '$ORIGIN:$ORIGIN/..',
        cpp_args: ['-std=c++20', '-g', '-O0', '-fPIC'] + _qt6_priv_inc,
        install: false)
    message('wayland idle-inhibit module: enabled')
else
    message('wayland idle-inhibit module: disabled (missing wayland-scanner / wayland-protocols / wayland-client / Qt6WaylandClient)')
endif

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
    'SessionIdle.cpp',
    c_vert_h, c_frag_h, translations_qm, processed,
    dependencies: [qt6_dep, vulkan_dep, libplacebo_dep, mpv_dep],
    include_directories: include_directories(
        '../../src/mpv/include',
        '../../third_party/mpv/include',
    ),
    cpp_args: ['-std=c++20', '-g', '-O0', '-fPIC'],
    install: false,
)
