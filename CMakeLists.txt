cmake_minimum_required(VERSION 3.4.1)

project(Detect)

# 指定编译器进行交叉编译
#set(CMAKE_C_COMPILER /home/ljh/ljh_nfs/my_toolchain/host/bin/aarch64-buildroot-linux-gnu-gcc)
#set(CMAKE_CXX_COMPILER /home/ljh/ljh_nfs/my_toolchain/host/bin/aarch64-buildroot-linux-gnu-g++)


# set(CMAKE_BUILD_TYPE "Debug") #启用调试信息
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# 设置链接器标志以允许共享库中的未定义符
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

# install target and libraries
set(CMAKE_INSTALL_PREFIX ${CMAKE_SOURCE_DIR}/install/Detect_${CMAKE_SYSTEM_NAME})

set(CMAKE_SKIP_INSTALL_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")

# set(LIB_ARCH "x86_64")
set(LIB_ARCH aarch64)
if (CMAKE_C_COMPILER MATCHES "aarch64")
  set(LIB_ARCH aarch64)
else()
  set(LIB_ARCH armhf)
endif()
include_directories(${CMAKE_SOURCE_DIR})

# rknn api
#指明目标芯片
set(TARGET_SOC "rk3588" CACHE STRING "Target SoC type") 
if(TARGET_SOC STREQUAL "rk356x")
  set(RKNN_API_PATH ${CMAKE_SOURCE_DIR}/libs/librknn_api)
elseif(TARGET_SOC STREQUAL "rk3588")
  set(RKNN_API_PATH ${CMAKE_SOURCE_DIR}/libs/librknn_api)
else()
  message(FATAL_ERROR "TARGET_SOC is not set, ref value: rk356x or rk3588 or rv110x")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(RKNN_RT_LIB ${RKNN_API_PATH}/${CMAKE_ANDROID_ARCH_ABI}/librknnrt.so)
else()
  set(RKNN_RT_LIB ${RKNN_API_PATH}/lib/librknnrt.so)
endif()
include_directories(${RKNN_API_PATH}/include)
include_directories(${CMAKE_SOURCE_DIR}/3rdparty)

# opencv
if (CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(OpenCV_DIR ${CMAKE_SOURCE_DIR}/../3rdparty/opencv/OpenCV-android-sdk/sdk/native/jni/abi-${CMAKE_ANDROID_ARCH_ABI})
elseif(CMAKE_CROSSCOMPILING)
  # 交叉编译: 从 sysroot 里找 arm64 OpenCV 配置
  set(OpenCV_DIR ${CMAKE_FIND_ROOT_PATH}/usr/lib/aarch64-linux-gnu/cmake/opencv4)
else()
  set(OpenCV_DIR /usr/lib/aarch64-linux-gnu/cmake/opencv4)
endif()
find_package(OpenCV REQUIRED) #在OpenCV_DIR路径中根据OpenCV包名寻找OpenCV库的配置文件

# aravis (GigE Vision)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ARAVIS REQUIRED aravis-0.8)
include_directories(${ARAVIS_INCLUDE_DIRS})

# RGA (Rockchip 2D accelerator - hardware resize/color convert)
include_directories(/usr/include/rga)
find_library(RGA_LIB rga /usr/lib/aarch64-linux-gnu)
if(NOT RGA_LIB)
  message(FATAL_ERROR "librga not found")
endif()


set(CMAKE_INSTALL_RPATH "lib")

include_directories( ${CMAKE_SOURCE_DIR}/include)

include_directories( ${CMAKE_SOURCE_DIR}/src)


# 可执行文件
add_executable(Detect 
               src/main_MultiDetect.cpp
               src/Detect.cc
               src/resize_function.cc
               src/rknn_utils.cc
               src/postprocess.cc
               src/DetectImpl.cc
               src/BYTETracker.cpp
               src/kalmanFilter.cpp
               src/lapjv.cpp
               src/STrack.cpp
               src/utils.cpp
               )

#将库链接到可执行文件               
target_link_libraries(Detect
  ${RKNN_RT_LIB}
  ${OpenCV_LIBS}
  ${ARAVIS_LIBRARIES}
  ${RGA_LIB}
)

# 运行时优先用项目自带的 librknnrt.so (v1.5.2)
set_target_properties(Detect PROPERTIES BUILD_RPATH "${RKNN_API_PATH}/lib")

#链接phtead库
find_package(Threads REQUIRED)
target_link_libraries(Detect ${CMAKE_THREAD_LIBS_INIT})

# ============================================================
# DetectUI: Python detect_ui.py 的 C++ Qt 移植版 (界面 1:1)
# 板上需要 Qt5 开发包: sudo apt install -y qtbase5-dev
# 单独编译(不影响 Detect): cd build && make DetectUI
# ============================================================
find_package(Qt5 COMPONENTS Widgets QUIET)
if(Qt5Widgets_FOUND)
  set(CMAKE_AUTOMOC ON)
  add_executable(DetectUI src/detect_ui.cpp src/detect_ui.h)
  target_link_libraries(DetectUI Qt5::Widgets ${CMAKE_THREAD_LIBS_INIT})
  message(STATUS "DetectUI enabled (Qt5 found)")
else()
  message(STATUS "Qt5 not found - DetectUI skipped (sudo apt install -y qtbase5-dev)")
endif()
