cmake_minimum_required(VERSION 3.21)
project(line_a11y LANGUAGES CXX)

# Build against the stock Qt 6.6.3 MSVC SDK (headers + import libs); at run time the plugin
# binds to the Qt6*.dll files shipped with LINE (same version, same MSVC ABI).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Qml Quick)

add_library(line_a11y MODULE
    src/plugin_main.cpp
    src/a11y_log.cpp
    src/text_capture.cpp
    src/line_introspect.cpp
    src/line_strings.cpp
    src/qt_signal_utils.cpp
    src/accessible_list.cpp
    src/accessible_widgets.cpp
    src/accessible_quick.cpp
    src/a11y_event_filter.cpp
    src/a11y_diagnostics.cpp
    src/a11y_announce.cpp
    src/call_install.cpp
    src/full_text_view.cpp
    src/video_controls.cpp
    src/file_dialog_fill.cpp
    src/focus_highlight.cpp
)
target_include_directories(line_a11y PRIVATE src)
target_link_libraries(line_a11y PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::GuiPrivate Qt6::CorePrivate uiautomationcore oleaut32 shell32 advapi32)
target_compile_definitions(line_a11y PRIVATE QT_NO_KEYWORDS UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    $<$<CONFIG:Release,RelWithDebInfo>:QT_NO_DEBUG>)
target_compile_options(line_a11y PRIVATE /W4 /utf-8 /permissive- /wd4100)

# Voice message transcription: audio capture and Windows speech recognition, in a process of
# its own (shipped in the add-on, run by appModules/line/transcribe.py).
# It carries a version resource (publisher, description, add-on version): antivirus heuristics
# rate an executable without one as suspicious.
enable_language(RC)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../addon/manifest.ini" _manifest_version REGEX "^version[ \t]*=")
string(REGEX REPLACE "^version[ \t]*=[ \t]*([0-9.]+).*" "\\1" LINEMR_VERSION "${_manifest_version}")
if(NOT LINEMR_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Could not read a version from addon/manifest.ini (got '${LINEMR_VERSION}')")
endif()
string(REPLACE "." "," LINEMR_VERSION_COMMAS "${LINEMR_VERSION},0")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../addon/manifest.ini")
configure_file(src/audio_helper.rc.in "${CMAKE_CURRENT_BINARY_DIR}/audio_helper.rc" @ONLY)
add_executable(lineMR_audio src/audio_helper.cpp "${CMAKE_CURRENT_BINARY_DIR}/audio_helper.rc")
target_link_libraries(lineMR_audio PRIVATE mmdevapi ole32 mfplat mfreadwrite mfuuid)
target_compile_definitions(lineMR_audio PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(lineMR_audio PRIVATE /W4 /utf-8 /permissive- /wd4100)
set_target_properties(lineMR_audio PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded" AUTOMOC OFF)
