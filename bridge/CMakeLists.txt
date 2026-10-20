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
    src/qt_signal_utils.cpp
    src/accessible_list.cpp
    src/accessible_widgets.cpp
    src/accessible_quick.cpp
    src/a11y_event_filter.cpp
    src/a11y_diagnostics.cpp
    src/a11y_announce.cpp
)
target_include_directories(line_a11y PRIVATE src)
target_link_libraries(line_a11y PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Quick Qt6::GuiPrivate Qt6::CorePrivate uiautomationcore oleaut32)
target_compile_definitions(line_a11y PRIVATE QT_NO_KEYWORDS UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    $<$<CONFIG:Release,RelWithDebInfo>:QT_NO_DEBUG>)
target_compile_options(line_a11y PRIVATE /W4 /utf-8 /permissive- /wd4100)
