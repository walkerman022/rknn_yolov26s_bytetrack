# RK3588 交叉编译方案(方案 B:WSL 拉板子 sysroot)

> 状态:**已打通并验证**。不再需要板子上 make,Windows 侧改代码 → WSL 交叉编译 → scp 上板即可。

---

## 1. 方案概述

| 项 | 说明 |
|---|---|
| 目标 | 在 WSL 里交叉编译出 aarch64 的 `Detect`,替代板子上 `make` |
| 原理 | 从板子 rsync 完整 sysroot(头文件 + 库),编译器用 `aarch64-linux-gnu-g++-9`(匹配板子 gcc-9/glibc 2.31) |
| 产物 | `build_cross/Detect`(aarch64,要求 GLIBC_2.17/GLIBCXX_3.4.22)+ `build_cross/DetectUI`(Qt5 Widgets) |
| 验证 | 板子实测 Detect 输出与板编原版**逐行一致**;DetectUI offscreen 启动成功(0.09s) |

### 环境

| 机器 | 信息 |
|---|---|
| Windows 11 + WSL | Ubuntu-24.04 |
| 板子 | `192.168.137.137`,用户 `orangepi` / 密码 `orangepi`(Ubuntu 20.04 focal,glibc 2.31) |
| 编译器 | `g++-9-aarch64-linux-gnu`(WSL 里 apt 装;**勿用默认 gcc-13**,ABI 不匹配板子) |
| 工具 | `sshpass`(只在 WSL 里有,Windows 侧没有) |

### 目录

```
cross/
├── setup_cross.sh          # 一次性: 从板子 rsync 拉 sysroot
├── build_cross.sh          # 交叉编译入口 (cmake + make)
└── toolchain-rk3588.cmake  # CMake 工具链文件
~/rk3588_sysroot/           # 板子 sysroot (拉一次,之后不用重拉)
build_cross/                # 编译产物 (build_cross/Detect + build_cross/DetectUI)
```

---

## 1.5 全新电脑从零搭建(迁移/换机指南)

> 目标:新电脑上构建一套可用的交叉编译环境。**总耗时约 30-60 分钟**(主要看 sysroot 拉取速度)。
> 前提:能访问板子(WiFi/网线同一网段,板子 IP 或可改),有板子 SSH 密码(orangepi)。

### 第 1 步:准备 WSL

```powershell
# Windows PowerShell(管理员):安装 WSL + Ubuntu 24.04
wsl --install -d Ubuntu-24.04
# 重启后设置 WSL 用户名/密码(记下来,后续 sudo 用)
```

进 WSL 后更新系统:

```bash
sudo apt update && sudo apt upgrade -y
```

### 第 2 步:拷贝工程文件

把整个 `rknn_yolov8_bytetrack-main/`(含 `cross/` 4 个文件)拷到新电脑,放到与旧机器**相同的相对位置**(源码在主工作区,WSL 里用 `~/rknn_yolov8_bytetrack-main/`):

```bash
# 方式 A:Windows 侧拷贝后,在 WSL 里建立工作副本
mkdir -p ~/rknn_yolov8_bytetrack-main
cp -r /mnt/d/<你的路径>/rknn_yolov8_bytetrack-main/* ~/rknn_yolov8_bytetrack-main/
# (src/ include/ CMakeLists.txt cross/ 等)

# 方式 B:如果旧机器还在,直接 rsync 整包
rsync -avz --exclude=build --exclude=build_cross user@旧机器:~/rknn_yolov8_bytetrack-main/ ~/rknn_yolov8_bytetrack-main/
```

### 第 3 步:安装依赖(一次性)

```bash
# 交叉编译器 gcc-9(匹配板子 focal)+ 工具
sudo apt install -y g++-9-aarch64-linux-gnu rsync sshpass
# 宿主 Qt5(WSL apt 只有 5.15,仅作备用;moc 用 5.12.8 见第 0.5 步)
sudo apt install -y qtbase5-dev

# 验证
aarch64-linux-gnu-g++-9 --version   # 应显示 9.4.0
sshpass -V | head -1
```

### 第 4 步:拉取 sysroot(板子必须在线,前台跑)

```bash
cd ~/rknn_yolov8_bytetrack-main
bash cross/setup_cross.sh
```

- ⚠️ **不要后台跑**——脚本里 `sudo apt-get` 无 TTY 拿不到密码会挂死
- 脚本里的 sudo 密码 = 新 WSL 用户密码(不是板子的);板子密码写在脚本里(orangepi)
- 若板子 IP 变了,改 `setup_cross.sh` 里的 `BOARD="orangepi@192.168.137.137"`

### 第 5 步:sysroot 修补(必做!setup 拉完是"板子原样",不修补编译不过)

```bash
SYS=~/rk3588_sysroot

# ---- A. 相对符号链接(4 处 libc/libpthread 等) ----
cd $SYS/lib/aarch64-linux-gnu
ln -sfn libpthread-2.31.so libpthread.so && ln -sfn libpthread-2.31.so libpthread.so.0
ln -sfn libc-2.31.so libc.so.6 && ln -sfn libm-2.31.so libm.so.6
ln -sfn ld-2.31.so ld-linux-aarch64.so.1 && ln -sfn libstdc++.so.6 libstdc++.so
cd $SYS/usr/lib/aarch64-linux-gnu
ln -sfn libpthread-2.31.so libpthread.so && ln -sfn libpthread-2.31.so libpthread.so.0
ln -sfn libc-2.31.so libc.so.6 && ln -sfn libm-2.31.so libm.so.6
ln -sfn ld-2.31.so ld-linux-aarch64.so.1 && ln -sfn libstdc++.so.6 libstdc++.so

# ---- B. blas/lapack alternatives 悬空 → 相对链接 ----
ln -sfn blas/libblas.so.3   $SYS/usr/lib/aarch64-linux-gnu/libblas.so.3
ln -sfn lapack/liblapack.so.3 $SYS/usr/lib/aarch64-linux-gnu/liblapack.so.3

# ---- C. libc.so 链接脚本 → sysroot 绝对路径(两份!) ----
SYSABS=$(realpath $SYS)
printf '/* GNU ld script */\nOUTPUT_FORMAT(elf64-littleaarch64)\nGROUP ( %s/lib/aarch64-linux-gnu/libc.so.6 %s/usr/lib/aarch64-linux-gnu/libc_nonshared.a  AS_NEEDED ( %s/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 ) )\n' \
  "$SYSABS" "$SYSABS" "$SYSABS" > $SYS/usr/lib/aarch64-linux-gnu/libc.so
cp $SYS/usr/lib/aarch64-linux-gnu/libc.so $SYS/lib/aarch64-linux-gnu/libc.so

# ---- D. Qt5(DetectUI 用) ----
# D1. qmake wrapper(从板子拉)
sshpass -p orangepi scp orangepi@192.168.137.137:/usr/bin/aarch64-linux-gnu-qmake $SYS/usr/bin/
# D2. libEGL/libGL(补进编译器自带目录,WSL 根目录)
echo 123456 | sudo -S ln -sfn $SYS/usr/lib/aarch64-linux-gnu/libEGL.so /usr/lib/aarch64-linux-gnu/libEGL.so
echo 123456 | sudo -S ln -sfn $SYS/usr/lib/aarch64-linux-gnu/libGL.so  /usr/lib/aarch64-linux-gnu/libGL.so
# D3. 宿主 x86 moc 5.12.8(版本必须与板子 Qt 一致!5.15 不行)
#     下载 focal 的 qtbase5-dev-tools deb 解压出 moc:
mkdir -p /tmp/qtdev && cd /tmp/qtdev
wget -q 'http://mirrors.aliyun.com/ubuntu/pool/universe/q/qtbase-opensource-src/qtbase5-dev-tools_5.12.8+dfsg-0ubuntu2.1_amd64.deb' -O qtdevtools.deb
dpkg-deb -x qtdevtools.deb qtdevtools/
cp qtdevtools/usr/lib/qt5/bin/moc $SYS/usr/lib/qt5/bin/moc.x86
chmod +x $SYS/usr/lib/qt5/bin/moc.x86
# 替换 moc 为包装脚本(指向 moc.x86;路径按实际 $SYS 改!)
printf '#!/bin/sh\nexec %s/usr/lib/qt5/bin/moc.x86 "$@"\n' "$SYSABS" > $SYS/usr/lib/qt5/bin/moc
chmod +x $SYS/usr/lib/qt5/bin/moc
rm -rf /tmp/qtdev
```

### 第 6 步:首次编译验证

```bash
cd ~/rknn_yolov8_bytetrack-main
bash cross/build_cross.sh
# 应同时产出 build_cross/Detect 和 build_cross/DetectUI

# 关键验证: _start 必须正确传 init 指针(否则会崩, 见坑 #14)
aarch64-linux-gnu-objdump -d build_cross/Detect | grep -A14 "<_start>:" | grep -E "x3"
# 应看到 "ldr x3, [x3, #...]" (修复版); 若看到 "mov x3, #0x0" 则 crt 没配对
```

### 第 7 步:部署到板子测试(验收)

```bash
sshpass -p orangepi scp ~/rknn_yolov8_bytetrack-main/build_cross/Detect \
  ~/rknn_yolov8_bytetrack-main/build_cross/DetectUI \
  orangepi@192.168.137.137:~/rknn_yolov8_bytetrack-main/build/

sshpass -p orangepi ssh orangepi@192.168.137.137 \
  'cd ~/rknn_yolov8_bytetrack-main/build && ./Detect --model yolov26s_v4_int8.rknn \
   --file /home/orangepi/Videos/BSI-R完整视频.3.mp4 --no-display'
```

**验收标准**:
- Detect 能跑到 5000+ 帧,出现 `[FALSE-POS]`/`[MISS]` 事件**不崩溃**
- 若在第一个 `[FALSE-POS]` 后 `dumped core` → 说明 crt 没配对,回查第 0.5 步 A/C 和第 0.6 步验证

### 环境差异速查

| 项 | 原机器 | 新机器注意 |
|---|---|---|
| 板子 IP/密码 | 192.168.137.137 / orangepi | 变了就改 `setup_cross.sh` 和 scp/ssh 命令 |
| moc 包装脚本 | 硬编码 `/home/L1N/...` | 第 0.5 步 D3 用 `$SYSABS` 生成,不用改 |
| libEGL/libGL | 已建 | **必须重做**(WSL 根目录的链接不在 sysroot 里) |

---

## 2. 完整步骤

### 第 1 步(一次性):拉取 sysroot

```bash
# WSL 里,板子必须在线
bash cross/setup_cross.sh
```

- 6 段 rsync: `/usr/include`、`/usr/lib/aarch64-linux-gnu`、`/lib/aarch64-linux-gnu`、`/usr/lib`、`/usr/local/lib`、`/usr/local/include`
- **不排除 `*.a *.o`**(`crt*.o`、`libc_nonshared.a` 必须拉,否则链接失败)
- rsync code 23(套接字/特殊文件)无害,脚本已放行
- ⚠️ **不要在后台跑**(`&` / run_in_background):脚本里的 `sudo apt-get install` 无 TTY 拿不到密码会挂死。前台跑即可

### 第 2 步(每次改代码前):同步最新源码到 WSL

**重要!** WSL 里的源码可能过期(曾踩坑:WSL 是 8月6日旧版,缺 v4 模型 bbox/score 识别补丁,导致板子跑不出检测)。交叉编译前必须对比并同步:

```bash
# 检查差异 (src/ 和 include/ 全部对比)
for f in src/*.cpp src/*.h include/*.h; do
  a=$(md5sum ~/rknn_yolov8_bytetrack-main/$f 2>/dev/null | cut -d' ' -f1)
  b=$(md5sum /mnt/d/JEE/RK3588/rknn_yolov8_bytetrack-main/$f 2>/dev/null | cut -d' ' -f1)
  [ "$a" != "$b" ] && echo "DIFF: $f"
done

# 同步(把 D 盘最新版拷进 WSL)
cp /mnt/d/JEE/RK3588/rknn_yolov8_bytetrack-main/src/*.cpp \
   /mnt/d/JEE/RK3588/rknn_yolov8_bytetrack-main/src/*.h \
   /mnt/d/JEE/RK3588/rknn_yolov8_bytetrack-main/include/*.h \
   ~/rknn_yolov8_bytetrack-main/src/ 2>/dev/null
# 按需也同步 CMakeLists.txt 等
```

### 第 3 步:交叉编译

```bash
cd ~/rknn_yolov8_bytetrack-main
bash cross/build_cross.sh
# 产物: build_cross/Detect
```

脚本核心(不要随意改):

```bash
# 头文件: -nostdinc 禁用编译器自带 2.39 头, 全部用 sysroot 的 2.31 头
# (编译器默认 /usr/aarch64-linux-gnu/include 是 2.39, 混用会报 __isoc23_sscanf / mbstowcs)
INCLUDE_FLAGS="-nostdinc \
-I$RK_SYSROOT/usr/include/c++/9 -I$RK_SYSROOT/usr/include/aarch64-linux-gnu/c++/9 \
-I$RK_SYSROOT/usr/include/aarch64-linux-gnu/c++/9/aarch64-linux-gnu \
-I$RK_SYSROOT/usr/include/aarch64-linux-gnu/c++/9/backward \
-I$RK_SYSROOT/usr/include/aarch64-linux-gnu -I$RK_SYSROOT/usr/include \
-I/usr/lib/gcc-cross/aarch64-linux-gnu/9/include"

# 链接: -L 让 -lc/-lpthread 命中 sysroot 动态库(2.31), rpath-link 解析传递依赖
# -nostartfiles + sysroot crt: 编译器自带 Scrt1.o(glibc 2.39)的 _start 传 init/fini=NULL,
# 板子 libc 2.31 收到 NULL 跳过 init_array → 静态构造不执行 → 首次 push_event 崩 SIGSEGV (2026-08-11 已修)
# 不要加 --sysroot! (gcc 会给 -I 绝对路径加前缀导致路径翻倍)
# 不要加 -B! (会把 sysroot 插到搜索最前, 选中静态 libpthread.a → _dl_stack_flags)
CRT_FILES="$RK_SYSROOT/usr/lib/aarch64-linux-gnu/Scrt1.o $RK_SYSROOT/usr/lib/aarch64-linux-gnu/crti.o \
/usr/lib/gcc-cross/aarch64-linux-gnu/9/crtbegin.o"
CRT_END="/usr/lib/gcc-cross/aarch64-linux-gnu/9/crtend.o $RK_SYSROOT/usr/lib/aarch64-linux-gnu/crtn.o"
-L$RK_SYSROOT/lib/aarch64-linux-gnu -L$RK_SYSROOT/usr/lib/aarch64-linux-gnu \
-Wl,-rpath-link,$RK_SYSROOT/lib/aarch64-linux-gnu \
-Wl,-rpath-link,$RK_SYSROOT/usr/lib/aarch64-linux-gnu \
-Wl,-rpath-link,$RK_SYSROOT/usr/local/lib/aarch64-linux-gnu \
-Wl,-rpath-link,$RK_SYSROOT/usr/lib
```

### 第 4 步:验证产物

```bash
file build_cross/Detect                      # ELF 64-bit LSB pie executable, ARM aarch64
readelf --version-info build_cross/Detect | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -3   # 应 ≤ GLIBC_2.31
readelf --version-info build_cross/Detect | grep -oE 'GLIBCXX_[0-9.]+' | sort -uV | tail -2   # 应 ≤ GLIBCXX_3.4.28
```

### 第 5 步:部署到板子并测试

```bash
# 上传 (两个产物)
sshpass -p orangepi scp ~/rknn_yolov8_bytetrack-main/build_cross/Detect \
  ~/rknn_yolov8_bytetrack-main/build_cross/DetectUI \
  orangepi@192.168.137.137:~/rknn_yolov8_bytetrack-main/build/

# 板子上测试 Detect (模型/图片都在板子上)
sshpass -p orangepi ssh orangepi@192.168.137.137 \
  'cd ~/rknn_yolov8_bytetrack-main/build && chmod +x Detect && \
   ./Detect --model yolov26s_v4_int8.rknn --file frame.jpg --no-display'

# 板子上测试 DetectUI (无显示器环境用 offscreen 平台)
sshpass -p orangepi ssh orangepi@192.168.137.137 \
  'cd ~/rknn_yolov8_bytetrack-main/build && QT_QPA_PLATFORM=offscreen ./DetectUI --auto-start'
```

预期输出(`frames=1`,与原版一致):

```
Open video via FFMPEG backend: frame.jpg
Video: 1280x720 @ 25 fps (process 1280x720)
[INFO f1] raw_dets=0 tracked=0 | confs: (none)
[STATS] frames=1  confirmed_false_pos=0  zone_false_pos=0
```

DetectUI 预期(offscreen 启动成功):

```
[启动] 软件界面启动耗时: 0.09 秒
[细分] QApplication 构造: 0.007s, DetectUI 构造: 0.065s, show(): 0.016s
```

---

## 3. Qt5 交叉编译(DetectUI)专项

CMakeLists 已支持交叉编译 DetectUI(sysroot 含 qtbase5-dev 5.12.8)。依赖 3 处 sysroot 修补(重拉 sysroot 后需重做):

```bash
SYS=~/rk3588_sysroot
# 1. qmake wrapper (从板子拉, 611B shell 脚本)
sshpass -p orangepi scp orangepi@192.168.137.137:/usr/bin/aarch64-linux-gnu-qmake $SYS/usr/bin/

# 2. libEGL/libGL 链接 (WSL 根目录 /usr/lib/aarch64-linux-gnu 是编译器自带目录, 往里补链接)
echo 123456 | sudo -S ln -s $SYS/usr/lib/aarch64-linux-gnu/libEGL.so /usr/lib/aarch64-linux-gnu/libEGL.so
echo 123456 | sudo -S ln -s $SYS/usr/lib/aarch64-linux-gnu/libGL.so  /usr/lib/aarch64-linux-gnu/libGL.so

# 3. moc 换宿主 x86 5.12.8 (sysroot 里是 aarch64 二进制跑不了; 版本必须与板子 Qt 完全一致)
#    下载 focal 的 qtbase5-dev-tools deb (mirrors.aliyun.com), 解压出 moc 后固化进 sysroot
#    (已固化: $SYS/usr/lib/qt5/bin/moc.x86, 替换当前 moc 为包装脚本)
cp $SYS/usr/lib/qt5/bin/moc $SYS/usr/lib/qt5/bin/moc.aarch64.bak   # 备份原 aarch64 moc
# 把 focal 解压出的 moc 复制为 moc.x86, 然后:
printf '#!/bin/sh\nexec /home/L1N/rk3588_sysroot/usr/lib/qt5/bin/moc.x86 "$@"\n' > $SYS/usr/lib/qt5/bin/moc
chmod +x $SYS/usr/lib/qt5/bin/moc $SYS/usr/lib/qt5/bin/moc.x86
```

**坑**:
- WSL apt 只有 Qt 5.15,moc 5.15 生成的 `QMetaObject::SuperData` 在 5.12 头下编译不过 → 必须 5.12.8
- aqtinstall 的 qt_base 包不含 moc/qmake;`-DQt5Core_MOC_EXECUTABLE` 不生效(硬编码在 Qt5CoreConfigExtras.cmake)
- build_cross.sh 已支持 `"$@"` 透传额外 cmake 参数

---

## 4. 踩坑记录(全部已解决)

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| 1 | 链接缺 `crt1.o`/`libc_nonshared.a` | 旧 rsync 排除了 `*.a *.o` | 去掉排除,重拉 |
| 2 | setup_cross.sh 后台跑挂死 | `sudo apt-get` 无 TTY 拿不到密码 | 前台跑 |
| 3 | `libpthread.so.0: __libc_dlsym@GLIBC_PRIVATE` | 编译器自带 libc 2.39 混 sysroot libpthread 2.31 | 不用 `--sysroot`,用绝对路径 `-I`/`-L` 直指 sysroot |
| 4 | 静态 `libpthread.a` 被选中 → `_dl_stack_flags` | sysroot 里 `libpthread.so -> /lib/...` 绝对链接悬空,ld 回退 .a | sysroot 里改相对路径(`libpthread.so -> libpthread-2.31.so`) |
| 5 | `cannot find /lib/aarch64-linux-gnu/libc.so.6` | `libc.so` 链接脚本里绝对路径在 WSL 根目录不存在 | 链接脚本改写为指向 sysroot 的绝对路径(**`/lib` 和 `/usr/lib` 两份都要改**) |
| 6 | BLAS/LAPACK 符号全缺 | `libblas.so.3 -> /etc/alternatives/...` 悬空(rsync 没拉 /etc) | sysroot 里重建相对链接(`-> blas/libblas.so.3`) |
| 7 | `__isoc23_sscanf`/`mbstowcs` 未声明 | 编译器默认头 2.39 与 sysroot 头 2.31 混用 | `-nostdinc` + 全 sysroot 头路径 |
| 8 | `GDALOpen` 等 undefined | OpenCV→libgdal 传递依赖,`libgdal` 在 `/usr/lib`(非 multiarch) | rpath-link 补 `$SYSROOT/usr/lib` |
| 9 | 板子跑 `frames=0`,读不到帧 | **WSL 源码过期**(旧版缺 v4 输出识别补丁) | 编译前把 D 盘最新源码同步到 WSL |
| 10 | DetectUI 报 `Qt5::Core references .../qt5/bin/qmake 不存在` | sysroot 没拉 `/usr/bin`(qmake wrapper 缺失) | 从板子拉 `aarch64-linux-gnu-qmake` 到 `$SYS/usr/bin/` |
| 11 | DetectUI 报 `Qt5::Gui references /usr/lib/aarch64-linux-gnu/libEGL.so 不存在` | Qt5Gui cmake 硬编码绝对路径检查,文件实际在 sysroot | WSL 根目录 `/usr/lib/aarch64-linux-gnu/`(编译器自带目录)里补指向 sysroot 的 libEGL/libGL 链接 |
| 12 | moc 报 `Syntax error: Unterminated quoted string` | sysroot 的 moc 是 aarch64 二进制,在 x86 WSL 跑不了 | 宿主 x86 moc 5.12.8 固化进 sysroot(见 §3.5) |
| 13 | moc 5.15 生成代码报 `QMetaObject::SuperData` 未声明 | moc 版本必须与目标 Qt 一致 | 用 focal 的 moc 5.12.8(WSL apt 只有 5.15) |
| 14 | **交叉版首次 push_event 崩 SIGSEGV**(板编版正常),g_event_log 全 0 未构造 | 编译器自带 `Scrt1.o`(glibc 2.39)`_start` 传 `init/fini=NULL`;板子 libc 2.31 收到 NULL 跳过 init_array → 静态构造全不执行 | `-nostartfiles` + 显式 sysroot crt(`Scrt1.o/crti.o/crtn.o` + gcc `crtbegin.o/crtend.o`),见 §2 第 3 步;验证 `objdump -d | grep -A14 "<_start>:"` x3/x4 为 `ldr` 而非 `mov #0` |

### sysroot 一次性修补(重拉 sysroot 后需重做!)

以下修补是在 `~/rk3588_sysroot/` 里手工做的,**如果重跑 setup_cross.sh 重新拉取,会被板子源文件覆盖,需要重新执行**:

```bash
SYS=~/rk3588_sysroot
# 1. 绝对符号链接 → 相对
cd $SYS/lib/aarch64-linux-gnu
ln -sfn libpthread-2.31.so libpthread.so
ln -sfn libpthread-2.31.so libpthread.so.0
ln -sfn libc-2.31.so libc.so.6
ln -sfn libm-2.31.so libm.so.6
ln -sfn ld-2.31.so ld-linux-aarch64.so.1
ln -sfn libstdc++.so.6 libstdc++.so          # 缺 .so 链接, 否则 -lstdc++ 吃到编译器自带 2.39
cd $SYS/usr/lib/aarch64-linux-gnu
ln -sfn libpthread-2.31.so libpthread.so
ln -sfn libpthread-2.31.so libpthread.so.0
ln -sfn libc-2.31.so libc.so.6
ln -sfn libm-2.31.so libm.so.6
ln -sfn ld-2.31.so ld-linux-aarch64.so.1
ln -sfn libstdc++.so.6 libstdc++.so

# 2. blas/lapack alternatives 悬空 → 相对链接
ln -sfn blas/libblas.so.3 $SYS/usr/lib/aarch64-linux-gnu/libblas.so.3
ln -sfn lapack/liblapack.so.3 $SYS/usr/lib/aarch64-linux-gnu/liblapack.so.3

# 3. libc.so 链接脚本 → sysroot 绝对路径 (两份!)
SYSABS=$(realpath $SYS)
printf '/* GNU ld script */\nOUTPUT_FORMAT(elf64-littleaarch64)\nGROUP ( %s/lib/aarch64-linux-gnu/libc.so.6 %s/usr/lib/aarch64-linux-gnu/libc_nonshared.a  AS_NEEDED ( %s/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 ) )\n' \
  "$SYSABS" "$SYSABS" "$SYSABS" > $SYS/usr/lib/aarch64-linux-gnu/libc.so
cp $SYS/usr/lib/aarch64-linux-gnu/libc.so $SYS/lib/aarch64-linux-gnu/libc.so
```

---

## 5. 常用命令速查

```bash
# 拉 sysroot(板子在线,前台跑)
bash cross/setup_cross.sh

# 编译
bash cross/build_cross.sh

# 全量重编(清缓存)
rm -rf build_cross && bash cross/build_cross.sh

# 上板测试
sshpass -p orangepi scp build_cross/Detect orangepi@192.168.137.137:~/rknn_yolov8_bytetrack-main/build/Detect_cross
sshpass -p orangepi ssh orangepi@192.168.137.137 \
  'cd ~/rknn_yolov8_bytetrack-main/build && ./Detect_cross --model yolov26s_v4_int8.rknn --file frame.jpg --no-display'
```

---

## 6. 注意事项

- **模型/视频在板子上**,参数用相对路径(`../models/yolov26s_v4_int8.rknn`),不带路径前缀
- 无显示器环境测试:Detect 加 `--no-display`;DetectUI 用 `QT_QPA_PLATFORM=offscreen`(无头补丁 DETECTUI_TIMELOG/--auto-start 已内置)
- **重拉 sysroot 后 3 处修补要重做**:符号链接改相对(§4)、blas/lapack(§4)、Qt5(qmake/libEGL/libGL/moc,§3.5)
- 源码主工作区是 **D 盘**,WSL 只是编译环境——永远以 D 盘为准,编译前同步(src/*.cpp, src/*.h, include/*.h, CMakeLists.txt, detect_ui.cpp/h)
- WSL 里 `/usr/lib/qt5/bin/moc` 已被包装脚本替换(指向 moc.x86);若重拉 sysroot 会覆盖回 aarch64 原版,需重做 §3.5 第 3 步
