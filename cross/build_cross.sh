#!/usr/bin/env bash
# 交叉编译 Detect:  bash cross/build_cross.sh
set -e
export RK_SYSROOT="${RK_SYSROOT:-$HOME/rk3588_sysroot}"
export PKG_CONFIG_PATH="$RK_SYSROOT/usr/local/lib/aarch64-linux-gnu/pkgconfig:$RK_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig"
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
         -DQT_RCC_EXECUTABLE=/usr/lib/qt5/bin/rcc \
         -DCMAKE_EXE_LINKER_FLAGS="-nostartfiles $CRT_FILES \
         -L$RK_SYSROOT/lib/aarch64-linux-gnu -L$RK_SYSROOT/usr/lib/aarch64-linux-gnu \
         -Wl,-rpath-link,$RK_SYSROOT/lib/aarch64-linux-gnu -Wl,-rpath-link,$RK_SYSROOT/usr/lib/aarch64-linux-gnu -Wl,-rpath-link,$RK_SYSROOT/usr/local/lib/aarch64-linux-gnu -Wl,-rpath-link,$RK_SYSROOT/usr/lib $CRT_END" \
         "$@"

# ============================================================
# JEEFence qrc 资源生成(关键坑修复, 2026-08-17):
# AUTORCC 会用宿主 rcc(WSL /usr/lib/qt5/bin/rcc = Qt 5.15.13)生成
# qrc_industrial_ui.cpp, 其资源结构/格式与板子 Qt 5.12.8 不兼容
# → 板上运行时所有 ":/" 资源(JEE.png logo + 图标 PNG)加载失败 → 图标/logo 不显示。
# 修复: 用板子自带的 rcc 5.12.8(arm64)ssh 过去生成 qrc, scp 取回覆盖
# AUTORCC 产物, 再 touch 时间戳让它比源 qrc 新(避免 make 时 AUTORCC 重跑覆盖)。
# 依赖: 板子 192.168.137.137 在线, 密码 orangepi(sshpass)。
# ============================================================
BOARD="${BOARD:-orangepi@192.168.137.137}"
BOARD_PW="${BOARD_PW:-orangepi}"
QRC_AUTOGEN="JEEFence_autogen/UVLADIE3JM/qrc_industrial_ui.cpp"
QRC_SRC="../src/industrial_ui.qrc"
gen_qrc_on_board() {
    local rdir=/tmp/qrctest_$$
    sshpass -p "$BOARD_PW" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$BOARD" \
        "rm -rf $rdir && mkdir -p $rdir/src/icons_png"
    # 传 qrc + 源 PNG, 板子上按原结构放好再生成
    sshpass -p "$BOARD_PW" scp -o StrictHostKeyChecking=no \
        ../src/industrial_ui.qrc ../src/JEE.png \
        "$BOARD:$rdir/src/"
    sshpass -p "$BOARD_PW" scp -o StrictHostKeyChecking=no \
        ../src/icons_png/*.png \
        "$BOARD:$rdir/src/icons_png/"
    sshpass -p "$BOARD_PW" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$BOARD" \
        "cd $rdir/src && rcc --name industrial_ui -o $rdir/qrc_industrial_ui.cpp industrial_ui.qrc && ls -la $rdir/qrc_industrial_ui.cpp"
    sshpass -p "$BOARD_PW" scp -o StrictHostKeyChecking=no \
        "$BOARD:$rdir/qrc_industrial_ui.cpp" "$QRC_AUTOGEN"
    sshpass -p "$BOARD_PW" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$BOARD" \
        "rm -rf $rdir"
}
# 若 qrc 产物缺失或源 qrc 比产物新 → 重新用板子 rcc 生成
if [ ! -f "$QRC_AUTOGEN" ] || [ "$QRC_SRC" -nt "$QRC_AUTOGEN" ]; then
    echo "==> 用板子 rcc 5.12.8 生成 qrc 资源..."
    gen_qrc_on_board
    touch "$QRC_AUTOGEN"   # 时间戳置为最新, 跳过 AUTORCC 重跑
fi

make -j4
echo "==> 产物: $BUILD/Detect (aarch64)"
