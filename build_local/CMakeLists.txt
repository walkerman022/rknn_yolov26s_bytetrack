# 本地 Windows 预览专用: 只编译 JEEFence (不依赖 OpenCV/aravis/rga)
cmake_minimum_required(VERSION 3.10)
project(JEEFencePreview)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()

find_package(Qt5 REQUIRED COMPONENTS Widgets)

add_executable(JEEFence ../src/industrial_ui.cpp ../src/industrial_ui.h ../src/industrial_ui.qrc)
target_link_libraries(JEEFence Qt5::Widgets ${CMAKE_THREAD_LIBS_INIT})
