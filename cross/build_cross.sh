#!/usr/bin/env bash
# 交叉编译 Detect:  bash cross/build_cross.sh
set -e
export RK_SYSROOT="${RK_SYSROOT:-$HOME/rk3588_sysroot}"
export PKG_CONFIG_PATH="$RK_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig"
BUILD="build_cross"
mkdir -p "$BUILD"
cd "$BUILD"
# rpath-link: 让链接器在 sysroot 内解析 OpenCV/aravis 的传递依赖(gstreamer/ffmpeg/libpng 等)
# 注意: 不能静态链接 libstdc++/libgcc — sysroot 里的 .so 需要带版本号的 GLIBCXX 符号
# 头文件: -nostdinc + 全 sysroot 路径(见 toolchain), 保证 stdio.h 等用 2.31 版, 否则 __isoc23 符号链接不上
# -L: 让 -lc/-lpthread 命中 sysroot 的动态库(2.31), 否则解析到编译器自带 libc 2.39, libpthread 报 GLIBC_PRIVATE
# 勿加 -B: 会把 sysroot lib 目录插到搜索最前, 优先选中静态 libpthread.a, 报 _dl_stack_flags undefined
# -nostartfiles + sysroot crt: 编译器自带 Scrt1.o 是 glibc 2.39 版, _start 传 init/fini=NULL,
# 板子 libc 2.31 收到 NULL 跳过 init_array → 静态构造不执行 → g_event_log 未构造首次 push_back 崩溃
INCLUDE_FLAGS="-nostdinc -I$RK_SYSROOT/usr/include/c++/9 -I$RK_SYSROOT/usr/include/aarch64-linux-gnu/c++/9 \
-I$RK_SYSROOT/usr/include/aarch64-linux-gnu/c++/9/aarch64-linux-gnu \
-I$RK_SYSROOT/usr/include/aarch64-linux-gnu/c++/9/backward \
-I$RK_SYSROOT/usr/include/aarch64-linux-gnu -I$RK_SYSROOT/usr/include \
-I/usr/lib/gcc-cross/aarch64-linux-gnu/9/include"
CRT_FILES="$RK_SYSROOT/usr/lib/aarch64-linux-gnu/Scrt1.o $RK_SYSROOT/usr/lib/aarch64-linux-gnu/crti.o \
/usr/lib/gcc-cross/aarch64-linux-gnu/9/crtbegin.o"
CRT_END="/usr/lib/gcc-cross/aarch64-linux-gnu/9/crtend.o $RK_SYSROOT/usr/lib/aarch64-linux-gnu/crtn.o"
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cross/toolchain-rk3588.cmake \
         -DTARGET_SOC=rk3588 \
         -DCMAKE_C_FLAGS="$INCLUDE_FLAGS" -DCMAKE_CXX_FLAGS="$INCLUDE_FLAGS" \
         -DCMAKE_EXE_LINKER_FLAGS="-nostartfiles $CRT_FILES \
         -L$RK_SYSROOT/lib/aarch64-linux-gnu -L$RK_SYSROOT/usr/lib/aarch64-linux-gnu \
         -Wl,-rpath-link,$RK_SYSROOT/lib/aarch64-linux-gnu -Wl,-rpath-link,$RK_SYSROOT/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$RK_SYSROOT/usr/local/lib/aarch64-linux-gnu -Wl,-rpath-link,$RK_SYSROOT/usr/lib $CRT_END" \
         "$@"
make -j4
echo "==> 产物: $BUILD/Detect (aarch64)"
