cmake_minimum_required(VERSION 3.20)
project(remora VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)  # for Q_OBJECT (Qt Test; later the UI/engine QObjects)

# DBus: org.freedesktop.FileManager1, so "show this folder" is asked as itself rather than as an
# inode/directory mime lookup that any media player can win (see showFolder in MainWindow.cpp).
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Test WebEngineWidgets Network DBus OpenGLWidgets Multimedia)

include(GNUInstallDirs)

# Installed builds read the vendored scripts from the share dir; dev builds from the source tree.
option(REMORA_INSTALLED_VENDOR "Point REMORA_VENDOR_DIR at the installed share location" OFF)
if(REMORA_INSTALLED_VENDOR)
    set(REMORA_VENDOR_PATH "${CMAKE_INSTALL_FULL_DATADIR}/remora/vendor")
else()
    set(REMORA_VENDOR_PATH "${CMAKE_SOURCE_DIR}/vendor")
endif()

enable_testing()

# --- pure domain (no Qt Widgets) ---
add_library(remora_core STATIC
    src/core/Backend.cpp
    src/core/Resolver.cpp
    src/core/Builders.cpp
    src/core/Features.cpp
    src/core/Gating.cpp
    src/core/ImageBuild.cpp
    src/core/MirrorInput.cpp
    src/core/MirrorProto.cpp
    src/core/Parsers.cpp
    src/core/Prereqs.cpp
    src/core/RedditSession.cpp
    src/core/SourcePatches.cpp
    src/core/SplashFormats.cpp
)
target_include_directories(remora_core PUBLIC src)
target_link_libraries(remora_core PUBLIC Qt6::Core)

# --- engine (impure: QProcess subprocess/ssh, backends, orchestrator) ---
add_library(remora_engine STATIC
    src/engine/SourceBuild.cpp
    src/engine/Spawner.cpp
    src/engine/Chain.cpp
    src/engine/Deployers.cpp
    src/engine/Exclusion.cpp
    src/engine/ImageBuilder.cpp
    src/engine/Orchestrator.cpp
    src/engine/Probe.cpp
    src/engine/AppMenu.cpp
    src/engine/AppTransfer.cpp
    src/engine/RedditLogin.cpp
)
target_include_directories(remora_engine PUBLIC src)
target_link_libraries(remora_engine PUBLIC remora_core Qt6::Core)
# The drift audit (bd remora-y9y9) needs both trees' locations and which one this binary reads:
# REMORA_VENDOR_DIR is what it stages from, the other two are the counterpart candidates it
# byte-compares against. The installed path is a candidate, not the truth — remora-install
# redirects the prefix at install time (see the prune block below), so the audit probes.
if(REMORA_INSTALLED_VENDOR)
    set(REMORA_VENDOR_IS_INSTALLED 1)
else()
    set(REMORA_VENDOR_IS_INSTALLED 0)
endif()
target_compile_definitions(remora_engine PRIVATE
    REMORA_VENDOR_DIR="${REMORA_VENDOR_PATH}"
    REMORA_SOURCE_VENDOR_DIR="${CMAKE_SOURCE_DIR}/vendor"
    REMORA_INSTALLED_VENDOR_DIR="${CMAKE_INSTALL_FULL_DATADIR}/remora/vendor"
    REMORA_VENDOR_IS_INSTALLED=${REMORA_VENDOR_IS_INSTALLED})

# --- config store (remorarc via QSettings) ---
add_library(remora_store STATIC src/store/Store.cpp)
target_include_directories(remora_store PUBLIC src)
target_link_libraries(remora_store PUBLIC remora_core Qt6::Core)

# --- mirror client (sockets + FFmpeg decode + render window; bd remora-28ix.2) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample)
add_library(remora_mirror STATIC
    src/mirror/AudioPlayer.cpp
    src/mirror/CudaInterop.cpp
    src/mirror/Decoder.cpp
    src/mirror/MirrorSession.cpp
    src/mirror/MirrorWindow.cpp
    src/mirror/MirrorApp.cpp
    src/mirror/Recorder.cpp
    src/mirror/Splash.cpp
    src/mirror/VideoReader.cpp
)
target_include_directories(remora_mirror PUBLIC src)
target_link_libraries(remora_mirror PUBLIC remora_core Qt6::Widgets Qt6::Network
    Qt6::OpenGLWidgets Qt6::Multimedia PkgConfig::FFMPEG)

# --- GUI (QtWidgets config workspace) ---
add_library(remora_gui STATIC
    src/ui/MainWindow.cpp
    src/ui/Workers.cpp
    src/ui/App.cpp
    src/ui/RedditLoginWindow.cpp
)
target_include_directories(remora_gui PUBLIC src)
target_link_libraries(remora_gui PUBLIC remora_engine remora_store remora_core
    Qt6::Widgets Qt6::WebEngineWidgets Qt6::Network Qt6::DBus)
target_compile_definitions(remora_gui PRIVATE REMORA_VENDOR_DIR="${REMORA_VENDOR_PATH}")

# --- binary: CLI (QCoreApplication) + `gui` subcommand (QApplication) ---
add_executable(remora src/main.cpp)
target_link_libraries(remora PRIVATE remora_gui remora_mirror remora_engine remora_store remora_core Qt6::Widgets)

# --- tests ---
add_executable(test_builders tests/test_builders.cpp)
target_link_libraries(test_builders PRIVATE remora_core Qt6::Test)
target_compile_definitions(test_builders PRIVATE REMORA_VENDOR_DIR="${REMORA_VENDOR_PATH}" REMORA_SOURCE_VENDOR_DIR="${CMAKE_SOURCE_DIR}/vendor")
add_test(NAME builders COMMAND test_builders)

add_executable(test_gating tests/test_gating.cpp)
target_link_libraries(test_gating PRIVATE remora_core Qt6::Test)
add_test(NAME gating COMMAND test_gating)

add_executable(test_parsers tests/test_parsers.cpp)
target_link_libraries(test_parsers PRIVATE remora_core Qt6::Test)
target_compile_definitions(test_parsers PRIVATE FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
add_test(NAME parsers COMMAND test_parsers)

add_executable(test_mirrorproto tests/test_mirrorproto.cpp)
target_link_libraries(test_mirrorproto PRIVATE remora_core Qt6::Test)
add_test(NAME mirrorproto COMMAND test_mirrorproto)

add_executable(test_prereqs tests/test_prereqs.cpp)
target_link_libraries(test_prereqs PRIVATE remora_core Qt6::Test)
add_test(NAME prereqs COMMAND test_prereqs)

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE remora_engine remora_store remora_core Qt6::Test)
# Same fixtures as test_parsers: the phantom-keyboard identity tests feed the engine a REAL
# /proc/bus/input/devices dump rather than a hand-made one, so a parser change that stops
# recognising the host's own keyboard fails here too.
target_compile_definitions(test_engine PRIVATE FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")
add_test(NAME engine COMMAND test_engine)

add_executable(test_reddit_session tests/test_reddit_session.cpp)
target_link_libraries(test_reddit_session PRIVATE remora_core Qt6::Test)
add_test(NAME reddit_session COMMAND test_reddit_session)

add_executable(test_store tests/test_store.cpp)
target_link_libraries(test_store PRIVATE remora_store remora_core Qt6::Test)
add_test(NAME store COMMAND test_store)

# --- install (binary + vendored scripts + desktop integration) ---
install(TARGETS remora RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# PRUNE BEFORE COPYING. install(DIRECTORY) copies but never removes, so a file deleted from the
# repo lingers in the installed tree forever — and builds stage from the INSTALLED tree
# (REMORA_INSTALLED_VENDOR), so a deleted patch keeps being applied. That cost a build
# 0009-Add-toggle-to-enable-ADB-root.patch was git rm'd, remora-install was run, and the
# next build still applied it from /usr/share (bd remora-4ei.33). The apply loop globs
# "[0-9]*.patch", so any orphan in a patch directory is picked up automatically — orphans are not
# inert clutter, they are live inputs.
#
# Removing only the patch trees, not all of vendor/: those are what the build consumes by glob, and
# a narrower rm is easier to reason about than one rooted at the whole vendor directory.
#
# THE PREFIX IS RESOLVED AT INSTALL TIME, not configure time, and that is the whole correctness of
# this block. CMAKE_INSTALL_FULL_DATADIR is expanded when cmake CONFIGURES, so it baked the
# configure-time prefix — /usr/local — into the generated script. remora-install installs with
# `sudo cmake --install build --prefix /usr`, which redirects where install(DIRECTORY) COPIES but
# does nothing to a path already frozen in the prune. So the prune tested a directory that does not
# exist, silently pruned nothing, and install(DIRECTORY) — which never removes — kept layering onto
# the real /usr/share tree. Measured: three orphans survived there, including two files
# this session had just git rm'd/git mv'd. The guard bd remora-4ei.33 added had never once fired on
# the path this machine actually installs by, which is why that bug's exact shape stayed possible.
# \${CMAKE_INSTALL_PREFIX} is set by the generated cmake_install.cmake from --prefix, so it follows
# the install; DATADIR stays configure-time because it is the prefix-RELATIVE "share".
# The staged tree projects (bd remora-28ix.4) are pruned for the SAME reason as the patch trees,
# one level over: stage-device-tree.sh copies them wholesale into the source tree, so a file
# deleted from the repo but lingering in the installed copy is staged into the build as if it
# still existed. That already happened once benignly — the LineageOS wallpapers removed from
# device-remora survived here and would have been staged back in — and the next occurrence need
# not be benign. They are live inputs, not clutter.
install(CODE "
  foreach(sub source-patches source-build/container-patches source-build/device-remora
          source-build/vendor-remora)
    set(_stale \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/remora/vendor/\${sub}\")
    if(EXISTS \"\${_stale}\")
      message(STATUS \"Pruning stale vendor tree: \${_stale}\")
      file(REMOVE_RECURSE \"\${_stale}\")
    endif()
  endforeach()
")
install(DIRECTORY vendor/ DESTINATION ${CMAKE_INSTALL_DATADIR}/remora/vendor
        USE_SOURCE_PERMISSIONS)
# No standalone PIP entry: it would sit loose in the application list, outside every profile folder,
# and act on whichever profile happened to be active — ambiguous the moment there are two. The PIP
# is launched from its profile's folder ("Android (PIP)"), where the profile is explicit.
install(FILES packaging/io.github.EtherAura.Remora.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
# Scheme-adaptive app icon. Plasma renders hicolor app icons with plain QtSvg (no Breeze
# stylesheet recoloring), so adaptation is done the robust way: baked color variants installed
# into the per-scheme Breeze theme dirs — white into breeze-dark, dark into breeze (light) —
# with the white default as the hicolor fallback for every other environment.
install(FILES packaging/io.github.EtherAura.Remora.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/io.github.EtherAura.Remora.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/breeze-dark/apps/48)
install(FILES packaging/io.github.EtherAura.Remora.light-scheme.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/breeze/apps/48
        RENAME io.github.EtherAura.Remora.svg)
install(FILES packaging/io.github.EtherAura.Remora.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
