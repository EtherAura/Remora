cmake_minimum_required(VERSION 3.20)

# The release version lives in VERSION and nowhere else that a human edits: tools/bump-version.sh
# writes it, the PKGBUILD, the AppStream metainfo and CHANGELOG.md together, and release.yml refuses
# a tag they disagree with. CalVer, YYYY.MM.PATCH.
#
# REMORA_VERSION, not PROJECT_VERSION, is the string to use: project() parses each component as an
# integer and re-prints it, so a September release's "2026.09.0" would come back as "2026.9.0" —
# a different version to dpkg's eyes than the one on the tag.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" REMORA_VERSION)
string(STRIP "${REMORA_VERSION}" REMORA_VERSION)
if(NOT REMORA_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION holds '${REMORA_VERSION}' — expected YYYY.MM.PATCH")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

# C is for the vendored native helpers (vendor/native), which are plain C; everything else is C++.
project(remora VERSION ${REMORA_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)  # for Q_OBJECT (Qt Test; later the UI/engine QObjects)

# Sql (with its SQLite driver, QSQLITE): the Reddit sign-in edits Android's account databases with
# it, so no sqlite3 binary is needed on either platform (bd remora-7oih.3).
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Test Network Sql OpenGLWidgets Multimedia)
# DBus: org.freedesktop.FileManager1, so "show this folder" is asked as itself rather than as an
# inode/directory mime lookup that any media player can win (see showFolder in MainWindow.cpp).
# A freedesktop session bus is a Linux desktop's, so the Windows client (bd remora-7oih) asks
# Explorer instead and needs no DBus. WebEngine is required on Linux, where every package carries
# it; Qt ships it for Windows only in the MSVC flavour, so an llvm-mingw build has no Reddit
# sign-in window and says so when asked for one.
if(WIN32)
    find_package(Qt6 6.4 QUIET COMPONENTS WebEngineWidgets)
    set(REMORA_HAVE_WEBENGINE ${Qt6WebEngineWidgets_FOUND})
else()
    find_package(Qt6 6.4 REQUIRED COMPONENTS DBus WebEngineWidgets)
    set(REMORA_HAVE_WEBENGINE ON)
endif()

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
    src/core/Guest.cpp
    src/core/ImageLineage.cpp
    src/core/ManagedHost.cpp
    src/core/MirrorHwDecode.cpp
    src/core/MirrorInput.cpp
    src/core/MirrorProto.cpp
    src/core/MirrorRegistry.cpp
    src/core/Parsers.cpp
    src/core/Prereqs.cpp
    src/core/RedditSession.cpp
    src/core/SourcePatches.cpp
    src/core/SplashFormats.cpp
    src/core/StartMenu.cpp
)
target_include_directories(remora_core PUBLIC src)
target_link_libraries(remora_core PUBLIC Qt6::Core)

# --- engine (impure: QProcess subprocess/ssh, backends, orchestrator) ---
add_library(remora_engine STATIC
    src/engine/PublicImages.cpp
    src/engine/PlayInstall.cpp
    src/engine/Addons.cpp
    src/engine/SourceBuild.cpp
    src/engine/Spawner.cpp
    src/engine/GuestVm.cpp
    src/engine/MirrorRegistry.cpp
    src/engine/Chain.cpp
    src/engine/Deployers.cpp
    src/engine/Exclusion.cpp
    src/engine/ImageBuilder.cpp
    src/engine/Orchestrator.cpp
    src/engine/Probe.cpp
    src/engine/AppMenu.cpp
    src/engine/AppTransfer.cpp
    src/engine/RedditLogin.cpp
    src/engine/HostShell.cpp
)
target_include_directories(remora_engine PUBLIC src)
# Network: GuestVm talks to its QEMU's control port and probes loopback ports (bd remora-7oih.5),
# and the image side fetches over HTTP with Qt. Sql: the Reddit sign-in edits account databases
# with Qt. Both replace a local curl and sqlite3, which a Windows client does not have
# (bd remora-7oih.3).
target_link_libraries(remora_engine PUBLIC remora_core Qt6::Core Qt6::Network Qt6::Sql)
# Start-menu shortcuts are written through the shell's COM objects (Spawner::writeShortcut).
if(WIN32)
    target_link_libraries(remora_engine PUBLIC ole32 uuid)
endif()
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
# remora_engine for the mirror registry alone (engine/MirrorRegistry.h): a mirror registers
# itself through the same code the engine reads it back with.
target_link_libraries(remora_mirror PUBLIC remora_core remora_engine Qt6::Widgets Qt6::Network
    Qt6::OpenGLWidgets Qt6::Multimedia PkgConfig::FFMPEG)

# --- GUI (QtWidgets config workspace) ---
add_library(remora_gui STATIC
    src/ui/MainWindow.cpp
    src/ui/Workers.cpp
    src/ui/App.cpp
)
target_include_directories(remora_gui PUBLIC src)
target_link_libraries(remora_gui PUBLIC remora_engine remora_store remora_core
    Qt6::Widgets Qt6::Network)
if(NOT WIN32)
    target_link_libraries(remora_gui PUBLIC Qt6::DBus)
endif()
if(REMORA_HAVE_WEBENGINE)
    target_sources(remora_gui PRIVATE src/ui/RedditLoginWindow.cpp)
    target_link_libraries(remora_gui PUBLIC Qt6::WebEngineWidgets)
    target_compile_definitions(remora_gui PUBLIC REMORA_HAVE_WEBENGINE=1)
endif()
target_compile_definitions(remora_gui PRIVATE REMORA_VENDOR_DIR="${REMORA_VENDOR_PATH}")

# --- binary: CLI (QCoreApplication) + `gui` subcommand (QApplication) ---
add_executable(remora src/main.cpp)
target_link_libraries(remora PRIVATE remora_gui remora_mirror remora_engine remora_store remora_core Qt6::Widgets)
target_compile_definitions(remora PRIVATE REMORA_VERSION_STRING="${REMORA_VERSION}")
# The window icon. Linux finds it in the icon theme the install puts it in, or at its installed
# path; Windows has neither, so the binary carries it as :/remora/<id>.ico (core/Branding.h): the
# same .ico the executables carry for Explorer, through packaging/windows/remora.rc.in, which a
# light title bar and a dark taskbar can both show (tools/windows-icon.sh).
if(WIN32)
    string(REPLACE "." "," REMORA_VERSION_COMMAS "${REMORA_VERSION}")
    set(REMORA_ICO "${CMAKE_SOURCE_DIR}/packaging/windows/io.github.EtherAura.Remora.ico")
    foreach(REMORA_EXE remora remora-gui)
        configure_file(packaging/windows/remora.rc.in "${CMAKE_BINARY_DIR}/${REMORA_EXE}.rc" @ONLY)
    endforeach()
    target_sources(remora PRIVATE "${CMAKE_BINARY_DIR}/remora.rc")
    qt_add_resources(remora remora_branding PREFIX "/remora" BASE packaging/windows
        FILES packaging/windows/io.github.EtherAura.Remora.ico)
    # The same program for Explorer and the Start menu (bd remora-7oih.6). remora.exe is a console
    # program, which is what a terminal wants, and started from Explorer it opens a console
    # window behind the workspace. remora-gui.exe is a Windows-subsystem build of the same
    # main.cpp: with no arguments it opens the workspace, and the mirrors it starts are
    # remora-gui.exe too, so none of them ever has a console.
    add_executable(remora-gui WIN32 src/main.cpp)
    target_link_libraries(remora-gui PRIVATE remora_gui remora_mirror remora_engine remora_store
        remora_core Qt6::Widgets)
    target_compile_definitions(remora-gui PRIVATE REMORA_VERSION_STRING="${REMORA_VERSION}"
        REMORA_GUI_BY_DEFAULT=1)
    target_sources(remora-gui PRIVATE "${CMAKE_BINARY_DIR}/remora-gui.rc")
    qt_add_resources(remora-gui remora_gui_branding PREFIX "/remora" BASE packaging/windows
        FILES packaging/windows/io.github.EtherAura.Remora.ico)
endif()

# --- vendored native helpers (vendor/native) ---
# Built IN PLACE, into vendor/native/, because that is where both kinds of build look for them: a
# dev build reads the source vendor dir directly, and install(DIRECTORY vendor/) below carries the
# binaries into the share dir with everything else. Before this, only a hand-run
# `make -C vendor/native` built them, so a fresh build-and-install shipped no fwd — and fwd is
# bind-mounted into every container. The commands are the Makefile's, flag for flag (the Makefile
# stays, for rebuilding one helper by hand); the binaries are gitignored there.
#
# fwd and uinput-kbd run INSIDE the container, which has no host libc to link against, so they are
# static and required: no static libc is a configure error here, not a link error halfway through.
#
# Every one of them is a LINUX binary — two run in the container, two beside docker on the host —
# so a Windows build (bd remora-7oih) cannot make them with its own compiler. It skips them; the
# Windows package has to carry the ones a Linux build made.
if(WIN32)
    message(STATUS "vendor/native: NOT built — these are Linux binaries, and this is a Windows build")
else()
set(REMORA_NATIVE_DIR "${CMAKE_SOURCE_DIR}/vendor/native")
include(CheckCSourceCompiles)
include(CheckIncludeFile)
set(CMAKE_REQUIRED_LINK_OPTIONS -static)
check_c_source_compiles("int main(void) { return 0; }" REMORA_HAVE_STATIC_LIBC)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(NOT REMORA_HAVE_STATIC_LIBC)
    message(FATAL_ERROR "vendor/native/fwd and uinput-kbd are linked statically (they run inside "
        "the container) and the C compiler cannot link a static binary. Install the static C "
        "library: libc6-dev on Debian/Ubuntu, glibc-static on Fedora; Arch and Gentoo ship it "
        "with glibc.")
endif()
set(REMORA_NATIVE_OUTPUTS)
foreach(helper fwd uinput-kbd)
    add_custom_command(OUTPUT "${REMORA_NATIVE_DIR}/${helper}"
        COMMAND "${CMAKE_C_COMPILER}" -static -O2 ${helper}.c -o ${helper}
        WORKING_DIRECTORY "${REMORA_NATIVE_DIR}"
        DEPENDS "${REMORA_NATIVE_DIR}/${helper}.c"
        COMMENT "Building vendor/native/${helper} (static)"
        VERBATIM)
    list(APPEND REMORA_NATIVE_OUTPUTS "${REMORA_NATIVE_DIR}/${helper}")
endforeach()

# The host-side frame encoder (host_encode) and decoder (host_decode) are optional, exactly as in the
# Makefile: they link the host's own FFmpeg, and a missing FFmpeg must not break the ordinary build.
# `remora up` reports either one as a clear prerequisite when a profile asks for it and it is absent.
# Configure says which of them it is building, and why it skipped one.
#
# The encoder's floor is the first libavfilter with AVBufferSrcParameters.color_space, which it sets;
# FFmpeg 6.1 (Ubuntu 24.04) predates it and fails to compile. libavutil's hwcontext_vulkan.h
# includes vulkan/vulkan.h, and FFmpeg's .pc files do not say so — hence the separate header check.
pkg_check_modules(REMORA_ENCODER_DEPS QUIET libavcodec "libavfilter>=9.16.100" libavutil)
check_include_file(vulkan/vulkan.h REMORA_HAVE_VULKAN_H)
if(REMORA_ENCODER_DEPS_FOUND AND REMORA_HAVE_VULKAN_H)
    add_custom_command(OUTPUT "${REMORA_NATIVE_DIR}/remora-frame-encoder"
        COMMAND "${CMAKE_C_COMPILER}" -O2 -Wall -Wextra -o remora-frame-encoder
                remora-frame-encoder.c ${REMORA_ENCODER_DEPS_CFLAGS} ${REMORA_ENCODER_DEPS_LDFLAGS}
        WORKING_DIRECTORY "${REMORA_NATIVE_DIR}"
        DEPENDS "${REMORA_NATIVE_DIR}/remora-frame-encoder.c"
                "${REMORA_NATIVE_DIR}/remora-encoder-names.h"
        COMMENT "Building vendor/native/remora-frame-encoder"
        VERBATIM)
    list(APPEND REMORA_NATIVE_OUTPUTS "${REMORA_NATIVE_DIR}/remora-frame-encoder")
    message(STATUS "remora-frame-encoder: building (host_encode)")
else()
    message(STATUS "remora-frame-encoder: NOT built (host_encode unavailable) — needs libavcodec, "
        "libavfilter >= 9.16.100 (FFmpeg 7 or newer), libavutil and the Vulkan headers")
endif()
pkg_check_modules(REMORA_DECODER_DEPS QUIET libavcodec libavutil libva)
if(REMORA_DECODER_DEPS_FOUND)
    add_custom_command(OUTPUT "${REMORA_NATIVE_DIR}/remora-frame-decoder"
        COMMAND "${CMAKE_C_COMPILER}" -O2 -Wall -Wextra -o remora-frame-decoder
                remora-frame-decoder.c ${REMORA_DECODER_DEPS_CFLAGS} ${REMORA_DECODER_DEPS_LDFLAGS}
        WORKING_DIRECTORY "${REMORA_NATIVE_DIR}"
        DEPENDS "${REMORA_NATIVE_DIR}/remora-frame-decoder.c"
        COMMENT "Building vendor/native/remora-frame-decoder"
        VERBATIM)
    list(APPEND REMORA_NATIVE_OUTPUTS "${REMORA_NATIVE_DIR}/remora-frame-decoder")
    message(STATUS "remora-frame-decoder: building (host_decode)")
else()
    message(STATUS "remora-frame-decoder: NOT built (host_decode unavailable) — needs libavcodec, "
        "libavutil and libva")
endif()
add_custom_target(remora_native ALL DEPENDS ${REMORA_NATIVE_OUTPUTS})
endif()

# --- tests ---
# Every test runs with HOME and the XDG base directories inside the build tree (set below, once
# all tests are registered), so no suite can read or write the user's real configuration. The
# store suite refuses to run unless its default remorarc resolves inside this directory.
set(REMORA_TEST_HOME "${CMAKE_BINARY_DIR}/test-home")
set(REMORA_TEST_ENV
    "HOME=${REMORA_TEST_HOME}"
    "XDG_CONFIG_HOME=${REMORA_TEST_HOME}/.config"
    "XDG_DATA_HOME=${REMORA_TEST_HOME}/.local/share"
    "XDG_CACHE_HOME=${REMORA_TEST_HOME}/.cache"
    "XDG_STATE_HOME=${REMORA_TEST_HOME}/.local/state")
file(MAKE_DIRECTORY "${REMORA_TEST_HOME}/.config" "${REMORA_TEST_HOME}/.local/share"
     "${REMORA_TEST_HOME}/.cache" "${REMORA_TEST_HOME}/.local/state")
# Python runs four suites and two prereqs cases, and nothing that is built or installed, so a
# machine without it still configures and builds Remora; those tests then do not run, and this
# says so. -DCMAKE_REQUIRE_FIND_PACKAGE_Python3=ON turns a missing interpreter into a configure
# error instead: CI and the packages' own test runs pass it, so the suites cannot drop out quietly.
#
# A Windows build registers none of them and gives prereqs no interpreter: what they test is Linux
# tooling (the Play installer, the ARM fetch and the image helper all run on the docker host, the
# audit in the release pipeline), and the interpreter a cross build would find is this machine's.
set(REMORA_TEST_PYTHON "")
if(NOT WIN32)
    find_package(Python3 COMPONENTS Interpreter)
endif()
if(WIN32)
    message(STATUS "tests: a Windows build registers no Python suites — they test Linux tooling")
elseif(Python3_Interpreter_FOUND)
    set(REMORA_TEST_PYTHON "${Python3_EXECUTABLE}")
    add_test(NAME play_install COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_play_install.py)
    add_test(NAME arm_fetch COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_arm_fetch.py)
    add_test(NAME public_images COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_public_images.py)
    add_test(NAME public_audit COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_public_audit.py)
else()
    message(STATUS "tests: Python 3 not found — play_install, arm_fetch, public_images and public_audit are "
        "not registered, and prereqs skips its two image-helper cases")
endif()
# A cross-built suite is a Windows program, so ctest runs it through Wine (tools/windows-wine.sh):
# a smoke test, not a sign-off — tools/windows-stage.sh --tests stages the suites for a real
# Windows machine. The SDK is named outright, because the HOME a test runs with is the isolated
# one below. Set before the suites are created, which is when a target takes it.
if(WIN32 AND CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${CMAKE_COMMAND}" -E env
        "REMORA_WINDOWS_SDK=${REMORA_WINDOWS_SDK}" "${CMAKE_SOURCE_DIR}/tools/windows-wine.sh")
endif()
# Registers suite <name>, built as test_<name>. Every suite finds the checkout's fixtures and
# vendor files through sourcePath() (tests/TestPlatform.h): from this root, or from
# REMORA_TEST_SOURCE_DIR where it runs on another machine. On Windows the log also goes to a file
# in test-results/, because QtTest prints nothing to a Wine console.
function(remora_suite_test name)
    target_compile_definitions(test_${name} PRIVATE REMORA_TEST_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
    if(WIN32)
        add_test(NAME ${name} COMMAND test_${name}
            -o "${CMAKE_BINARY_DIR}/test-results/${name}.txt,txt" -o "-,txt")
    else()
        add_test(NAME ${name} COMMAND test_${name})
    endif()
endfunction()
if(WIN32)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-results")
endif()
add_executable(test_play_engine tests/test_play_engine.cpp)
target_link_libraries(test_play_engine PRIVATE remora_engine Qt6::Test)
remora_suite_test(play_engine)
add_executable(test_addons tests/test_addons.cpp)
target_link_libraries(test_addons PRIVATE remora_engine Qt6::Test)
remora_suite_test(addons)
add_executable(test_builders tests/test_builders.cpp)
target_link_libraries(test_builders PRIVATE remora_core Qt6::Test)
remora_suite_test(builders)

add_executable(test_gating tests/test_gating.cpp)
target_link_libraries(test_gating PRIVATE remora_core Qt6::Test)
remora_suite_test(gating)

add_executable(test_parsers tests/test_parsers.cpp)
target_link_libraries(test_parsers PRIVATE remora_core Qt6::Test)
remora_suite_test(parsers)

add_executable(test_mirrorproto tests/test_mirrorproto.cpp)
target_link_libraries(test_mirrorproto PRIVATE remora_core Qt6::Test)
remora_suite_test(mirrorproto)

add_executable(test_prereqs tests/test_prereqs.cpp)
target_link_libraries(test_prereqs PRIVATE remora_core Qt6::Test)
# The image-fetch cache regression is the helper's own Python; this suite runs it. REMORA_PYTHON is
# empty when no interpreter was found, and those cases skip, as they do on Windows.
target_compile_definitions(test_prereqs PRIVATE REMORA_PYTHON="${REMORA_TEST_PYTHON}")
remora_suite_test(prereqs)

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE remora_engine remora_store remora_core Qt6::Test)
# Same fixtures as test_parsers: the phantom-keyboard identity tests feed the engine a REAL
# /proc/bus/input/devices dump rather than a hand-made one, so a parser change that stops
# recognising the host's own keyboard fails here too.
remora_suite_test(engine)

add_executable(test_reddit_session tests/test_reddit_session.cpp)
target_link_libraries(test_reddit_session PRIVATE remora_core Qt6::Test)
remora_suite_test(reddit_session)

add_executable(test_store tests/test_store.cpp)
target_link_libraries(test_store PRIVATE remora_store remora_core Qt6::Test)
target_compile_definitions(test_store PRIVATE REMORA_TEST_HOME="${REMORA_TEST_HOME}")
remora_suite_test(store)

# --- fuzz targets (off by default; Clang only, in a build directory of their own; bd remora-fdcv) ---
# The parser under test is compiled again, instrumented, into a library of its own: linking the
# harness against remora_core would fuzz an uninstrumented parser. Every sanitizer flag is set on
# these targets alone, so nothing else in the tree changes when the option is on.
option(REMORA_FUZZ "Build the libFuzzer targets (Clang only) and the `fuzz`-labelled smoke tests" OFF)
if(REMORA_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "REMORA_FUZZ needs Clang (libFuzzer), not ${CMAKE_CXX_COMPILER_ID}: "
            "configure a separate build directory with -DCMAKE_C_COMPILER=clang "
            "-DCMAKE_CXX_COMPILER=clang++")
    endif()
    set(REMORA_FUZZ_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer -g)
    add_library(remora_fuzz_mirrorproto STATIC src/core/MirrorProto.cpp)
    target_include_directories(remora_fuzz_mirrorproto PUBLIC src)
    target_link_libraries(remora_fuzz_mirrorproto PUBLIC Qt6::Core)
    target_compile_options(remora_fuzz_mirrorproto PRIVATE ${REMORA_FUZZ_SANITIZERS}
        -fsanitize=fuzzer-no-link)

    add_executable(remora_fuzz_device_message_parser tests/fuzz/fuzz_device_message_parser.cpp)
    target_link_libraries(remora_fuzz_device_message_parser PRIVATE remora_fuzz_mirrorproto)
    target_compile_options(remora_fuzz_device_message_parser PRIVATE ${REMORA_FUZZ_SANITIZERS}
        -fsanitize=fuzzer)
    target_link_options(remora_fuzz_device_message_parser PRIVATE ${REMORA_FUZZ_SANITIZERS}
        -fsanitize=fuzzer)

    # About ten seconds of fuzzing from the checked-in seeds. New inputs go to the first corpus
    # directory, which is in the build tree, as do crash artifacts; the seed directory is only read.
    set(_fuzz_out "${CMAKE_BINARY_DIR}/fuzz/device_message_parser")
    file(MAKE_DIRECTORY "${_fuzz_out}/corpus" "${_fuzz_out}/artifacts")
    add_test(NAME fuzz_device_message_parser
        COMMAND remora_fuzz_device_message_parser -max_total_time=10 -timeout=10
            -rss_limit_mb=2048 -print_final_stats=1 "-artifact_prefix=${_fuzz_out}/artifacts/"
            "${_fuzz_out}/corpus" "${CMAKE_SOURCE_DIR}/tests/fuzz/corpus/device_message_parser")
    set_tests_properties(fuzz_device_message_parser PROPERTIES LABELS fuzz TIMEOUT 60
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")

    add_executable(remora_fuzz_parse_session_reply tests/fuzz/fuzz_parse_session_reply.cpp)
    target_link_libraries(remora_fuzz_parse_session_reply PRIVATE remora_fuzz_mirrorproto)
    target_compile_options(remora_fuzz_parse_session_reply PRIVATE ${REMORA_FUZZ_SANITIZERS}
        -fsanitize=fuzzer)
    target_link_options(remora_fuzz_parse_session_reply PRIVATE ${REMORA_FUZZ_SANITIZERS}
        -fsanitize=fuzzer)

    set(_fuzz_out "${CMAKE_BINARY_DIR}/fuzz/parse_session_reply")
    file(MAKE_DIRECTORY "${_fuzz_out}/corpus" "${_fuzz_out}/artifacts")
    add_test(NAME fuzz_parse_session_reply
        COMMAND remora_fuzz_parse_session_reply -max_total_time=10 -timeout=10
            -rss_limit_mb=2048 -print_final_stats=1 "-artifact_prefix=${_fuzz_out}/artifacts/"
            "${_fuzz_out}/corpus" "${CMAKE_SOURCE_DIR}/tests/fuzz/corpus/parse_session_reply")
    set_tests_properties(fuzz_parse_session_reply PROPERTIES LABELS fuzz TIMEOUT 60
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")

    add_executable(remora_fuzz_text_message tests/fuzz/fuzz_text_message.cpp)
    target_link_libraries(remora_fuzz_text_message PRIVATE remora_fuzz_mirrorproto)
    target_compile_options(remora_fuzz_text_message PRIVATE ${REMORA_FUZZ_SANITIZERS}
        -fsanitize=fuzzer)
    target_link_options(remora_fuzz_text_message PRIVATE ${REMORA_FUZZ_SANITIZERS}
        -fsanitize=fuzzer)

    set(_fuzz_out "${CMAKE_BINARY_DIR}/fuzz/text_message")
    file(MAKE_DIRECTORY "${_fuzz_out}/corpus" "${_fuzz_out}/artifacts")
    add_test(NAME fuzz_text_message
        COMMAND remora_fuzz_text_message -max_total_time=10 -timeout=10
            -rss_limit_mb=2048 -print_final_stats=1 "-artifact_prefix=${_fuzz_out}/artifacts/"
            "${_fuzz_out}/corpus" "${CMAKE_SOURCE_DIR}/tests/fuzz/corpus/text_message")
    set_tests_properties(fuzz_text_message PROPERTIES LABELS fuzz TIMEOUT 60
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endif()

# ENVIRONMENT, not ENVIRONMENT_MODIFICATION: the latter needs CMake 3.22, and an older ctest drops
# an unknown property silently, which would leave the run quietly unisolated. APPEND keeps whatever
# else a test puts in its ENVIRONMENT. Register every test above this line.
get_property(_remora_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _remora_tests)
    set_property(TEST "${_t}" APPEND PROPERTY ENVIRONMENT ${REMORA_TEST_ENV})
endforeach()

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
# Python writes bytecode caches into the source tree whenever something imports a vendor module,
# and the PKGBUILD's check() runs ctest, which does, right before package() installs.
install(DIRECTORY vendor/ DESTINATION ${CMAKE_INSTALL_DATADIR}/remora/vendor
        USE_SOURCE_PERMISSIONS PATTERN "__pycache__" EXCLUDE)
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
# GPL-3.0 travels with the binary, and NOTICE with it: it credits the third-party work Remora builds
# on, which redistribution has to carry along.
install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})

# --- .deb via CPack (`cpack -G DEB` in a build tree) ---
# Configure a packaged build with -DREMORA_INSTALLED_VENDOR=ON -DCMAKE_INSTALL_PREFIX=/usr, or the
# binary will look for its scripts in the source tree it was built from. release.yml builds the
# released .deb this way on ubuntu-24.04. SHLIBDEPS walks the installed ELF files with
# dpkg-shlibdeps for the exact runtime package names (Qt, FFmpeg, libva), so none are hand-kept
# here. docker and adb are tools Remora drives rather than libraries it links, hence Recommends:
# a `remote` target needs no docker on this machine at all.
set(CPACK_PACKAGE_NAME remora)
set(CPACK_PACKAGE_VERSION "${REMORA_VERSION}")
set(CPACK_PACKAGE_VENDOR "Remora")
set(CPACK_PACKAGE_CONTACT "Remora <noreply@localhost>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/EtherAura/Remora")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Build, deploy and mirror Android running in Docker")
# Pre-wrapped: CPackDeb indents each line of the extended description but does not wrap it.
set(CPACK_PACKAGE_DESCRIPTION
    "Remora is a desktop control plane for Android in Docker. Compose an\n\
Android image feature by feature and build it from source, deploy it to\n\
this machine or to any docker host over ssh, and connect with Remora's\n\
own hardware-accelerated mirror: a Qt client talking to an agent baked\n\
into the image. A headless CLI and a Qt workspace share one unit-tested\n\
core.")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR DEB)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Remora <noreply@localhost>")
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# The one runtime dependency no ELF names: Qt loads its SQLite driver as a plugin, and the Reddit
# sign-in edits account databases through it (bd remora-7oih.3). SHLIBDEPS appends to this.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "docker.io | docker-ce, adb")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "openssh-client, python3, unzip, pkexec")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
# Stated rather than asked of dpkg, which a non-Debian host does not have (CPack then guesses
# i386). The static helpers and the images are x86_64-only, so amd64 is the only honest label.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
endif()
include(CPack)
