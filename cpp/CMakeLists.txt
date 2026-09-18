cmake_minimum_required(VERSION 3.21)

project(side_shelf VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译 Qt 时若提示缺少 Qt6，请用 -DCMAKE_PREFIX_PATH=C:/Qt/6.9.3/msvc2022_64
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL)

qt_standard_project_setup()

# 性能基线对照（开发用）：-DSIDESHELF_BASELINE=ON 时按修复前的行为编译
# （图片缓存禁用预算/按矩形尺寸解码、GL FBO 精确匹配逐帧重建），
# 用于 --test-panel-heavy + SIDESHELF_PROF=1 的修复前后逐帧耗时对比。
# 默认 OFF：正常构建包含全部性能修复。
set(SIDESHELF_BASELINE OFF CACHE STRING "Build the pre-fix performance baseline")
if(SIDESHELF_BASELINE)
    add_compile_definitions(SIDESHELF_PERF_BASELINE)
endif()

qt_add_executable(side_shelf WIN32
    src/main.cpp
    src/autostart.h
    src/autostart.cpp
    src/shelfwidget.h
    src/shelfwidget.cpp
    src/itempreviewpanel.h
    src/itempreviewpanel.cpp
    src/glcanvas.h
    src/glcanvas.cpp
    src/uifont.h
    src/uifont.cpp
)

target_link_libraries(side_shelf PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL)

# MSVC 警告级别
if(MSVC)
    target_compile_options(side_shelf PRIVATE /W4 /utf-8)
endif()

set_target_properties(side_shelf PROPERTIES
    OUTPUT_NAME "side_shelf"
)
