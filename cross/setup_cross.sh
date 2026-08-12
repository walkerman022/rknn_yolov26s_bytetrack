#!/usr/bin/env bash
# 方案B: 从板子 rsync 完整 sysroot(只读板子, 不改板子), 版本 100% 一致
# 在 WSL 里运行一次:  bash cross/setup_cross.sh
set -e
SYSROOT="${RK_SYSROOT:-$HOME/rk3588_sysroot}"
BOARD="orangepi@192.168.137.137"
BOARD_PASS="orangepi"

# 若 sysroot 曾被 root 建过(目录 owner=root), 普通用户无法写入, 先改为当前用户所有
# 注意: 顶层可能显示为当前用户, 但 usr/lib 等子目录可能仍是 root — 递归检查顶层子目录
if [ -d "$SYSROOT" ]; then
  root_dirs=$(find "$SYSROOT" -maxdepth 2 -type d ! -user "$(whoami)" 2>/dev/null | head -5)
  if [ -n "$root_dirs" ]; then
    echo "==> sysroot 部分目录为 root 所有: $root_dirs"
    echo "==> 改为 $(whoami) 所有"
    sudo chown -R "$(whoami):$(id -gn)" "$SYSROOT"
  fi
fi

# rsync code 23 = 部分文件未传输(通常是无害的套接字/特殊文件), 放行继续; 其余错误退出
run_rsync() {
  set +e
  "$@"
  local rc=$?
  set -e
  if [ $rc -ne 0 ] && [ $rc -ne 23 ]; then
    echo "==> rsync 失败 (code $rc)" >&2
    exit $rc
  fi
}
echo "==> sysroot 目录: $SYSROOT"
echo "==> 数据源: 板子 $BOARD (只读, 不修改板子)"

echo "==> 安装交叉编译器 gcc-9(匹配板子 focal/gcc-9, 避免 glibc/libstdc++ 版本冲突)"
sudo apt-get update
# gcc-9 交叉包: 板上为 Ubuntu 20.04 (gcc-9), 用同版本编译器保证 libstdc++ ABI 一致
sudo apt-get install -y g++-9-aarch64-linux-gnu rsync sshpass

if ! command -v sshpass >/dev/null; then
  echo "ERROR: sshpass 安装失败, 无法自动输密码。请手动运行 rsync 或在 SSH 配置中加密钥。" >&2
  exit 1
fi

echo "==> 从板子拉取 /usr/include 和 /usr/lib/aarch64-linux-gnu"
echo "    (需板子在线; 一次拉全, 之后不用再拉)"
mkdir -p "$SYSROOT/usr/lib"

RSYNC_SSH="sshpass -p ${BOARD_PASS} ssh -o StrictHostKeyChecking=no"

# 只拉头文件与库, 排除无用的 Python/本地化/调试数据, 可显著减小体积
run_rsync rsync -avz -e "$RSYNC_SSH" \
  --exclude='python3' --exclude='python3.*' \
  --exclude='*.a' --exclude='libicuuc.a' \
  "$BOARD:/usr/include/" "$SYSROOT/usr/include/"

# /usr/lib/aarch64-linux-gnu: 头文件/库/glibc 链接脚本需要的静态文件(crt*.o, libc_nonshared.a)
# 注意: 不能排除 *.a *.o — libc.so 链接脚本会引用 libc_nonshared.a, 编译器需要 crt1.o/Scrt1.o
run_rsync rsync -avz -e "$RSYNC_SSH" \
  --exclude='python3*' \
  --exclude='*.la' \
  --exclude='libicudata*' --exclude='libicuuc*' --exclude='libicui18n*' \
  "$BOARD:/usr/lib/aarch64-linux-gnu/" "$SYSROOT/usr/lib/aarch64-linux-gnu/"

# 关键: 基础运行库(glibc/libstdc++/libpng/gstreamer/ffmpeg等)在板子 /lib/aarch64-linux-gnu, 必须一并拉取
# 否则链接 OpenCV/aravis 时缺传递依赖符号
# 注意: 不排除 *.a *.o — libc.so 链接脚本引用 libc_nonshared.a, crt*.o 编译必需
run_rsync rsync -avz -e "$RSYNC_SSH" \
  --exclude='python3*' \
  --exclude='*.la' \
  --exclude='libicudata*' --exclude='libicuuc*' --exclude='libicui18n*' \
  "$BOARD:/lib/aarch64-linux-gnu/" "$SYSROOT/lib/aarch64-linux-gnu/"

# 板子 /usr/lib 下还有非 multiarch 库(libgdal 及其依赖), OpenCV imgcodecs 链接 libgdal, 一并拉取
# 注意: 不能排除 *.so — libgdal.so.26 这类版本化 .so 也会被匹配掉
run_rsync rsync -avz -e "$RSYNC_SSH" \
  --exclude='*.a' --exclude='*.o' --exclude='*.la' \
  --exclude='python3*' \
  "$BOARD:/usr/lib/" "$SYSROOT/usr/lib/"

# 板子 /usr/local 下是自编译安装的库(aravis 在 /usr/local/lib/aarch64-linux-gnu), 必须拉取
mkdir -p "$SYSROOT/usr/local/lib" "$SYSROOT/usr/local/include"
run_rsync rsync -avz -e "$RSYNC_SSH" \
  --exclude='*.a' --exclude='*.o' --exclude='*.la' \
  --exclude='python3*' \
  "$BOARD:/usr/local/lib/" "$SYSROOT/usr/local/lib/"

run_rsync rsync -avz -e "$RSYNC_SSH" \
  --exclude='python3*' \
  "$BOARD:/usr/local/include/" "$SYSROOT/usr/local/include/"

echo "==> sysroot 拉取完成。检查关键依赖:"
ls "$SYSROOT/usr/lib/aarch64-linux-gnu/cmake/opencv4" >/dev/null 2>&1 && echo "  [OK] OpenCV cmake 配置" || echo "  [缺失] OpenCV cmake 配置 (opencv4)"
ls "$SYSROOT/usr/include/rga/" >/dev/null 2>&1 && echo "  [OK] librga 头文件" || echo "  [缺失] librga 头文件 (/usr/include/rga)"
ls "$SYSROOT/usr/lib/aarch64-linux-gnu/libaravis*" >/dev/null 2>&1 && echo "  [OK] aravis 库" || echo "  [缺失] libaravis (GigE 相机, 若无相机可忽略)"
ls "$SYSROOT/lib/aarch64-linux-gnu/libstdc++.so.6" >/dev/null 2>&1 && echo "  [OK] libstdc++ (来自 /lib)" || echo "  [缺失] libstdc++ (/lib/aarch64-linux-gnu)"
ls "$SYSROOT/lib/aarch64-linux-gnu/libgstapp-1.0.so.0" >/dev/null 2>&1 && echo "  [OK] gstreamer (来自 /lib)" || echo "  [缺失] gstreamer (/lib/aarch64-linux-gnu)"

echo "==> 完成。编译请运行: bash cross/build_cross.sh"
