# RK3588 aarch64 交叉编译工具链文件 (WSL / Linux) — 优先用 gcc-9 匹配板子(focal/gcc9)
# 用法: cmake -DCMAKE_TOOLCHAIN_FILE=cross/toolchain-rk3588.cmake -DTARGET_SOC=rk3588 ..
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# gcc-9 与板子 Ubuntu 20.04 的 libstdc++ ABI 完全一致; 若未装 gcc-9 自动回退 gcc-13
if(EXISTS "/usr/bin/aarch64-linux-gnu-g++-9")
  set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc-9)
  set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-9)
else()
  set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
  set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
endif()

# 勿设 CMAKE_SYSROOT: gcc 的 --sysroot 会把用户 -I 绝对路径也加 sysroot 前缀(路径翻倍不存在),
# 回退到编译器自带 2.39 头 → __isoc23 符号。头/库全部用绝对路径 -I/-L 指向 sysroot 即可。
set(CMAKE_FIND_ROOT_PATH $ENV{RK_SYSROOT}
                         $ENV{RK_SYSROOT}/usr/local/lib/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 头文件必须全用 sysroot 的 2.31 版本, 用 -nostdinc 禁用编译器自带 include:
# 编译器默认 /usr/aarch64-linux-gnu/include 是 2.39, 混用时 cstdlib include_next 到 2.39 stdlib.h → mbstowcs 未声明
# 注意: 不能设 CMAKE_SYSROOT — gcc 的 --sysroot 会把用户 -I 绝对路径也加 sysroot 前缀(路径翻倍不存在), 回退 2.39 头
set(SYSROOT_INCLUDES
    "-I$ENV{RK_SYSROOT}/usr/include/c++/9 -I$ENV{RK_SYSROOT}/usr/include/aarch64-linux-gnu/c++/9 \
     -I$ENV{RK_SYSROOT}/usr/include/aarch64-linux-gnu/c++/9/aarch64-linux-gnu \
     -I$ENV{RK_SYSROOT}/usr/include/aarch64-linux-gnu/c++/9/backward \
     -I$ENV{RK_SYSROOT}/usr/include/aarch64-linux-gnu -I$ENV{RK_SYSROOT}/usr/include \
     -I/usr/lib/gcc-cross/aarch64-linux-gnu/9/include")
set(CMAKE_C_FLAGS_INIT "-nostdinc ${SYSROOT_INCLUDES}")
set(CMAKE_CXX_FLAGS_INIT "-nostdinc ${SYSROOT_INCLUDES}")

# 不用 --sysroot(见上), libc.so 链接脚本已改成相对路径, 配合 -L 绝对路径即可命中 sysroot 的 2.31
# -L: 让 -lc/-lpthread 命中 sysroot 的动态库(2.31), 否则解析到编译器自带 libc 2.39, libpthread 报 GLIBC_PRIVATE
# 勿加 -B: 会把 sysroot lib 目录插到搜索最前, 优先选中静态 libpthread.a, 报 _dl_stack_flags undefined
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-L$ENV{RK_SYSROOT}/lib/aarch64-linux-gnu -L$ENV{RK_SYSROOT}/usr/lib/aarch64-linux-gnu \
     -Wl,-rpath-link,$ENV{RK_SYSROOT}/lib/aarch64-linux-gnu -Wl,-rpath-link,$ENV{RK_SYSROOT}/usr/lib/aarch64-linux-gnu")

