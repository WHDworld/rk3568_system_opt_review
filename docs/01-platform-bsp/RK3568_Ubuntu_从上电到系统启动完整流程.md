# RK3568 Ubuntu：从上电到系统启动的完整流程（基于当前源码树）

> 文档生成日期：2026-07-26  
> 分析对象：当前目录中的 Rockchip Linux SDK、当前配置、当前生成物和 `ubuntu/binary` 根文件系统  
> 结论只描述能由当前源码树或当前产物直接证明的内容；无法由源码证明的 BootROM 内部行为、闭源固件内部细节和实际板上运行结果均明确标注。

## 1. 当前 RK3568 平台的阶段术语与完整启动链

### 1.1 当前分析对象

本文只使用当前工程实际采用的 RK3568 阶段名称。平台由以下文件共同确定：

- [`output/.config`](../output/.config)：`RK_CHIP="rk3568"`、`RK_UBOOT_CFG="rk3568"`；
- [`rockchip_rk3568_topeet_defconfig`](../device/rockchip/.chips/rk3566_rk3568/rockchip_rk3568_topeet_defconfig)：当前板级配置；
- [`topeet-rk3568-linux.dts`](../kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dts)：`TOPEET RK3568 EVB1 DDR4 V10 Board`；
- [`u-boot/.config`](../u-boot/.config)：`CONFIG_ROCKCHIP_RK3568=y`。

### 1.2 当前平台使用的阶段术语

| 顺序 | 当前平台术语 | 主要作用 | 当前源码状态 |
|---:|---|---|---|
| 1 | RK3568 BootROM | 芯片复位入口，寻找并装载 Rockchip loader | 芯片内部，无源码 |
| 2 | DDR initialization firmware | 初始化和训练外部 DDR | `rkbin` 预编译 |
| 3 | Rockchip SPL/MiniLoader | 初始化读取后续镜像所需的最小硬件，装载 `uboot.img` | 当前默认是 `rkbin` 预编译 SPL |
| 4 | BL31/ARM Trusted Firmware | EL3、安全监控、PSCI 和异常级切换 | `rkbin` 预编译 |
| 5 | BL32/OP-TEE | 安全世界可信执行环境 | `rkbin` 预编译 |
| 6 | BL33/U-Boot proper | 选择启动模式和设备，加载 Linux FIT，准备 DTB/bootargs 并跳转内核 | 当前 `u-boot/` 有源码 |
| 7 | Linux Kernel | 初始化内核子系统、内建驱动和根存储链，挂载真实 rootfs | 当前 `kernel/` 有源码 |
| 8 | systemd/PID 1 | 根据 unit 依赖拉起用户空间、server 和图形环境 | 当前 rootfs 有二进制、unit 和配置 |

### 1.3 一张图看完整启动链

```text
上电 / 复位
  │
  ▼
RK3568 BootROM（芯片内部，源码树中无源码）
  │  从启动介质找到并装载 Rockchip loader；
  │  ROM 内部精确搜索顺序无法由当前源码树证明
  ▼
MiniLoaderAll.bin
  ├─ rk3568_ddr_1560MHz_v1.21.bin：DDR 初始化/训练（预编译）
  └─ rk356x_spl_v1.13.bin：Rockchip SPL（预编译）
       │  初始化足以读取后续镜像的硬件并装载 uboot.img
       ▼
uboot.img（FIT）
  ├─ BL31：ARM Trusted Firmware（预编译）
  ├─ BL32：OP-TEE（预编译）
  ├─ BL33：U-Boot proper（当前 u-boot/ 源码编译）
  └─ U-Boot DTB
       │
       ▼
U-Boot proper
  ├─ board_init_f → relocation → board_init_r → main_loop
  ├─ 识别 SD/eMMC，读取 misc 启动模式
  ├─ 从 boot 分区读取 Linux FIT boot.img
  ├─ 为 eMMC/SD 添加 root=/dev/mmcblk[0|1]p6
  └─ 把最终 DTB 地址放入 x0 并跳转 arm64 Linux Image
       │
       ▼
Linux 5.10.160
  ├─ head.S：异常级、页表和 MMU 早期处理
  ├─ start_kernel：内存、调度、中断、时钟和 VFS
  ├─ rest_init：创建 kernel_init（PID 1）和 kthreadd
  ├─ do_initcalls：注册总线、内建驱动并按 DT probe
  ├─ 初始化 MMC/SDHCI → mmcblk → GPT → ext4 根存储链
  ├─ prepare_namespace：等待根设备，挂载并切换真实根目录
  └─ exec /sbin/init
       │
       ▼
systemd（用户空间 PID 1）
  ├─ sysinit.target：udev、日志、模块和板级早期服务
  ├─ basic.target
  ├─ multi-user.target：网络及后台 server
  └─ graphical.target → LightDM → 图形会话
```

## 2. 当前软件和产物基线

### 2.1 软件、配置和产物

| 项目 | 当前证据 | 结论 |
|---|---|---|
| SoC/板卡 | `output/.config`、板级 DTS | RK3568，TOPEET EVB1 DDR4 V10 |
| U-Boot | [`u-boot/Makefile`](../u-boot/Makefile) | Rockchip 定制 U-Boot 2017.09 |
| Kernel | [`kernel/Makefile`](../kernel/Makefile) | Linux 5.10.160，arm64 |
| Kernel 配置 | [`kernel/.config`](../kernel/.config) | `CONFIG_ARM64=y`、`CONFIG_ARCH_ROCKCHIP=y`、`CONFIG_MODULES=y` |
| 根文件系统 | [`ubuntu/binary/etc/os-release`](../ubuntu/binary/etc/os-release) | Ubuntu 20.04.6 LTS（Focal） |
| PID 1 | `ubuntu/binary/sbin/init -> /lib/systemd/systemd` | systemd 245.4-4ubuntu3.24 |
| 默认 target | `ubuntu/binary/lib/systemd/system/default.target -> graphical.target` | 默认启动图形环境 |
| 显示管理器 | `ubuntu/binary/etc/systemd/system/display-manager.service -> lightdm.service` | 实际选择 LightDM，不是 GDM |

这里还有两个必须注意的当前工作区状态：

1. [`output/.config`](../output/.config) 当前写的是 `RK_ROOTFS_SYSTEM="buildroot"`，但 `output/firmware/rootfs.img` 实际链接到 [`ubuntu/rootfs.img`](../ubuntu/rootfs.img)。这说明“当前保存的 SDK 默认选项”和“最近生成/链接的根文件系统”不一致。本文用户态部分以实际存在的 Ubuntu 根文件系统为准。
2. `output/firmware/boot.img` 当前是指向 `kernel/boot.img` 的悬空链接；内核 `Image` 和板级 DTB 当前也已不在构建输出位置。已有 `output/update/Image/update.img` 生成于 2026-07-05，而当前 Ubuntu `rootfs.img` 生成于 2026-07-18，所以不能证明现有 `update.img` 包含最新的 Ubuntu 根文件系统。重新烧录前应重新构建 kernel/boot 和 update 镜像。

### 2.2 当前源码边界

| 阶段 | 当前能直接检查的证据 | 当前不能从本树声称的内容 |
|---|---|---|
| RK3568 BootROM | loader 格式、烧录接口 | ROM 内部源码、确切介质搜索顺序和安全算法 |
| DDR 初始化 | `rk3568_ddr_1560MHz_v1.21.bin`、INI 和二进制字符串 | 内部训练算法和寄存器调用路径 |
| 当前默认 SPL | `rk356x_spl_v1.13.bin`、INI 和二进制字符串 | 它逐行执行开源 U-Boot SPL 的哪些函数 |
| BL31 | `rk3568_bl31_v1.44.elf` 及其可见字符串 | Rockchip TF-A 内部实现源码 |
| OP-TEE | `rk3568_bl32_v2.11.bin` 及其可见字符串 | Secure OS 内部实现源码 |
| U-Boot proper | 当前 `u-boot/` 源码、配置和产物 | 无法仅凭静态源码证明某次实体板启动一定成功 |
| Linux/内核驱动 | 当前 `kernel/` 源码、DTS 和 `.config` | 无板上 `dmesg` 时，不能声称每个设备 probe 均成功 |
| systemd/Ubuntu daemon | 可执行文件、unit、配置和预制 tar | systemd、sshd、NetworkManager 等程序的上游源码不在本树 |
| 实际启动耗时 | 源码日志字符串和可用的诊断参数 | 没有实体板完整串口日志时不能填写实际耗时数值 |

后文的“日志模式”只代表当前源码或二进制中确实存在相应输出字符串；带条件的日志可能因为启动分支、日志级别或设备已经提前就绪而不出现。

## 3. 烧录镜像与目标存储分区

### 3.1 这些分区是不是板子磁盘上的分区

是。这里的 `uboot`、`misc`、`boot`、`recovery`、`backup` 和 `rootfs` 是烧录目标存储设备上的 GPT 逻辑分区：

- 烧到板载 eMMC 时，它们最终表现为 `/dev/mmcblk0p1`～`/dev/mmcblk0p6`；
- 按当前 MMC alias 和 U-Boot 定制逻辑，从 SD 卡启动时对应 `/dev/mmcblk1p1`～`/dev/mmcblk1p6`；
- `MiniLoaderAll.bin` 位于 Rockchip loader 使用的磁盘前部特殊区域，不是下面 GPT 表中的普通分区；
- [`parameter-buildroot-fit.txt`](../device/rockchip/.chips/rk3566_rk3568/parameter-buildroot-fit.txt) 是烧录工具创建 GPT 的布局描述，也不是一个启动时挂载的文件系统分区。

当前 `parameter` 的单位是 512-byte sector，分区与镜像按启动顺序合并如下：

| 位置/序号 | eMMC 节点 | SD 节点 | 当前镜像或来源 | 谁读取 | 启动作用 |
|---|---|---|---|---|---|
| GPT 前的 Rockchip loader 区域 | 非普通分区 | 非普通分区 | `u-boot/rk356x_spl_loader_v1.21.113.bin`，烧录时名为 `MiniLoaderAll.bin` | RK3568 BootROM | 包含 DDR firmware 和 Rockchip SPL |
| GPT 元数据 | 不作为文件系统节点使用 | 同左 | [`parameter-buildroot-fit.txt`](../device/rockchip/.chips/rk3566_rk3568/parameter-buildroot-fit.txt) | 烧录工具、分区解析代码 | 描述下列 GPT 分区的名称、偏移和大小 |
| 1：`uboot`，8 MiB 起、4 MiB | `/dev/mmcblk0p1` | `/dev/mmcblk1p1` | [`u-boot/uboot.img`](../u-boot/uboot.img) | Rockchip SPL | FIT 中包含 BL31、OP-TEE/BL32、U-Boot/BL33 和 U-Boot DTB |
| 2：`misc`，12 MiB 起、4 MiB | `/dev/mmcblk0p2` | `/dev/mmcblk1p2` | `misc.img` | U-Boot | 保存 normal、recovery、loader 等启动控制信息 |
| 3：`boot`，16 MiB 起、64 MiB | `/dev/mmcblk0p3` | `/dev/mmcblk1p3` | `boot.img`，标准模板为 [`kernel/boot.its`](../kernel/boot.its) | U-Boot `boot_fit` | FIT 中的 Linux `Image`、板级 DTB 和 `resource.img` |
| 4：`recovery`，80 MiB 起、128 MiB | `/dev/mmcblk0p4` | `/dev/mmcblk1p4` | `recovery.img` | U-Boot recovery 路径 | 恢复系统 |
| 5：`backup`，208 MiB 起、32 MiB | `/dev/mmcblk0p5` | `/dev/mmcblk1p5` | 当前布局只定义分区；当前脚本未证明有独立同名镜像写入 | 不能由当前产物确认 | 预留 backup 区域，本文不猜测其运行用途 |
| 6：`rootfs`，240 MiB 起、扩展到剩余空间 | `/dev/mmcblk0p6` | `/dev/mmcblk1p6` | [`ubuntu/rootfs.img`](../ubuntu/rootfs.img)，已确认为 ext4 | Linux `prepare_namespace()` | 真实 Ubuntu 根文件系统，最终成为 `/` |

当前布局没有单独的 `trust` GPT 分区；BL31 和 OP-TEE 已经在实际 `uboot.img` FIT 中。虽然通用 [`rkflash.sh`](../device/rockchip/common/scripts/rkflash.sh) 仍保留 `di -trust` 调用，不能据此在当前 `parameter` 表中虚构一个不存在的 `trust` 分区。

[`rkflash.sh`](../device/rockchip/common/scripts/rkflash.sh) 先上传 loader，再下发 parameter 和各分区镜像；整包 `update.img` 的生成逻辑在 [`90-updateimg.sh`](../device/rockchip/common/build-hooks/90-updateimg.sh)。U-Boot 在 [`boot_rkimg.c`](../u-boot/arch/arm/mach-rockchip/boot_rkimg.c) 中按介质写入：

```text
eMMC：root=/dev/mmcblk0p6
SD：  root=/dev/mmcblk1p6
```

因此一旦改变 GPT 分区顺序或 MMC 编号，这段板级定制也必须同步修改。

## 4. Bootloader 与安全固件阶段

### 4.1 阶段 A：上电、复位和 RK3568 BootROM

主要功能：

1. SoC 复位后，从芯片内部 ROM 开始执行。
2. 读取启动选择并尝试从可启动介质取得首级 loader。
3. 校验/解析 Rockchip loader 格式，把后续代码放入片内 SRAM 并转交控制。
4. 在下载/救砖场景中与 Rockusb/MaskROM 工具配合。

源码位置：**无**。BootROM 固化在 RK3568 芯片中，不属于此 SDK。当前树只能从 loader 的打包格式和烧录工具侧证明 BootROM 后面接 `MiniLoaderAll.bin`，不能从此树证明 BootROM 内部代码、每一种启动介质的确切搜索顺序或安全校验算法。

可见的宿主侧相关位置：

- loader 打包描述：[`rkbin/RKBOOT/RK3568MINIALL.ini`](../rkbin/RKBOOT/RK3568MINIALL.ini)。
- loader 打包脚本：[`u-boot/scripts/loader.sh`](../u-boot/scripts/loader.sh)。
- U-Boot 总构建入口：[`u-boot/make.sh`](../u-boot/make.sh)。
- 烧录工具调用：[`device/rockchip/common/scripts/rkflash.sh`](../device/rockchip/common/scripts/rkflash.sh)。

### 4.2 阶段 B：DDR 初始化二进制

当前 `RK3568MINIALL.ini` 明确把以下文件作为 `FlashData`：

```text
rkbin/bin/rk35/rk3568_ddr_1560MHz_v1.21.bin
```

主要功能是让外部 DDR 可用，通常包括控制器/PHY 配置、内存类型识别或参数应用、训练以及切换到工作频率。**这些具体内部步骤不能由当前源码证明**，因为该文件是预编译 `data`，没有对应 RK3568 DDR 固件源码。

当前产物版本由文件名和 INI 直接证明为 `v1.21`、目标频率名为 `1560MHz`。打包配置见 [`rkbin/RKBOOT/RK3568MINIALL.ini`](../rkbin/RKBOOT/RK3568MINIALL.ini) 第 6–18 行。

U-Boot 树确实另有一个可编译 TPL 框架：

- 通用 Rockchip TPL：[`u-boot/arch/arm/mach-rockchip/tpl.c`](../u-boot/arch/arm/mach-rockchip/tpl.c)。
- RK3568 TPL 平台代码：[`u-boot/arch/arm/mach-rockchip/rk3568/rk3568.c`](../u-boot/arch/arm/mach-rockchip/rk3568/rk3568.c)。
- TPL 在 `board_init_f()` 中初始化 timer、CPU 和 DRAM，随后可 `back_to_bootrom()`。

但是当前 `MiniLoaderAll.bin` 的 `FlashData` 指向 `rkbin` 的 DDR bin，而不是 `u-boot-tpl.bin`，所以不能把上述 TPL 源码说成当前实际烧录 DDR 阶段的实现。只有显式使用 `./make.sh --tpl` 等替代打包方式时，它才可能成为实际镜像的一部分。

### 4.3 阶段 C：Rockchip SPL/MiniLoader

当前 loader 的 `FlashBoot` 是：

```text
rkbin/bin/rk35/rk356x_spl_v1.13.bin
```

其主要职责可从整个镜像接口推断为：在 DDR 已可用后，建立读取启动介质所需的最小环境，找到后续 `uboot` 内容，装载其中的 BL31/BL32/BL33/FDT 并转交控制。但这个 `v1.13` 文件同样是闭源预编译二进制，所以不能声称当前烧录的 SPL 逐行执行了 U-Boot 源码中的某个函数。

U-Boot 源码树里存在功能等价的开源 SPL 框架，可用于理解或在替换厂商 SPL 时使用：

- Rockchip SPL 前半初始化：[`u-boot/arch/arm/mach-rockchip/spl.c`](../u-boot/arch/arm/mach-rockchip/spl.c) 的 `board_init_f()`、`spl_board_init()`。
- 通用 SPL 后半流程：[`u-boot/common/spl/spl.c`](../u-boot/common/spl/spl.c) 的 `board_init_r()`、`boot_from_devices()`。
- MMC 装载：[`u-boot/common/spl/spl_mmc.c`](../u-boot/common/spl/spl_mmc.c)。
- Rockchip 固件/FIT 解析：[`u-boot/common/spl/spl_rkfw.c`](../u-boot/common/spl/spl_rkfw.c)、[`u-boot/common/spl/spl_fit.c`](../u-boot/common/spl/spl_fit.c)。
- 交给 ATF：[`u-boot/common/spl/spl_atf.c`](../u-boot/common/spl/spl_atf.c)。

当前 `u-boot/.config` 同时启用了 `CONFIG_SPL=y`、`CONFIG_TPL=y` 和 `CONFIG_SPL_ATF=y`，说明源码具备这套能力；但默认 SDK 构建钩子优先链接 `*_loader_*v*.bin`，见 [`device/rockchip/common/build-hooks/20-loader.sh`](../device/rockchip/common/build-hooks/20-loader.sh) 第 90–94 行，所以当前固件仍以 `rkbin` 厂商 loader 为准。

### 4.4 阶段 D：装载 BL31、BL32、BL33

当前实际 [`u-boot/uboot.img`](../u-boot/uboot.img) 已通过 `dumpimage` 验证为一个描述为 `FIT Image with ATF/OP-TEE/U-Boot/MCU` 的 FIT，包含：

| FIT 节点 | 角色 | 当前输入/地址 | 源码状态 |
|---|---|---|---|
| `atf-1` 等 | BL31，ARM Trusted Firmware | 主段装载到 `0x00040000`，另有多个拆分段 | `rkbin/bin/rk35/rk3568_bl31_v1.44.elf`，预编译、stripped |
| `optee` | BL32，OP-TEE secure payload | `0x08400000` | `rkbin/bin/rk35/rk3568_bl32_v2.11.bin`，预编译 |
| `uboot` | BL33，非安全世界 U-Boot proper | `0x00a00000` | 来自当前 `u-boot/` 源码编译 |
| `fdt` | U-Boot 自己使用的 DTB | `rk3568-evb` | [`u-boot/arch/arm/dts/rk3568-evb.dts`](../u-boot/arch/arm/dts/rk3568-evb.dts) |

打包选择来自 [`rkbin/RKTRUST/RK3568TRUST.ini`](../rkbin/RKTRUST/RK3568TRUST.ini) 和 [`u-boot/make.sh`](../u-boot/make.sh) 的 `pack_uboot_itb_image()`。生成后的描述可见 [`u-boot/fit/u-boot.its`](../u-boot/fit/u-boot.its)。

功能分工：

- **BL31/ATF**：驻留 EL3，负责安全监控、异常级切换以及 PSCI/安全监控调用等平台固件职责。当前树没有 RK3568 BL31 源码，不能继续给出其内部调用路径。
- **BL32/OP-TEE**：安全世界 TEE。当前树只有预编译镜像，没有 `optee_os` 源码。
- **BL33/U-Boot proper**：在非安全世界继续完整引导，源码可见。

[`u-boot/common/spl/spl_atf.c`](../u-boot/common/spl/spl_atf.c) 展示了开源 SPL 如何准备 BL31 参数、设置 BL32/BL33 入口并进入 BL31；它是理解接口的可靠源码，但再次强调，当前首级 SPL 本身来自 `rkbin`，其内部实现不能据此逐行断言。

当前 U-Boot 配置中 `CONFIG_FIT_SIGNATURE` 未启用。虽然 ITS 模板含 `signature` 节点和各镜像含 SHA-256 hash，当前实际 `dumpimage -l uboot.img` 没列出签名，且 kernel FIT 的签名验证代码也被该配置条件关闭。因此不能把当前链描述成“已完成 RSA 安全启动认证”。

### 4.5 阶段 E：U-Boot proper 初始化

U-Boot proper 的主要调用脉络为：

```text
arch/arm/cpu/armv8/start.S: reset
  → arch/arm/lib/crt0_64.S: _main
  → common/board_f.c: board_init_f()
  → relocate_code（重定位到 DDR）
  → common/board_r.c: board_init_r()
  → init_sequence_r[]
  → common/main.c: main_loop()
  → bootdelay_process()
  → autoboot_command(bootcmd)
```

各子阶段功能：

- `reset/_main`：设置异常向量、初始栈和 global data。
- `board_init_f()`：执行重定位前的 CPU、时钟、串口、DRAM 信息、设备模型早期初始化和内存布局计算。
- relocation：把 U-Boot 搬到合适的 DDR 高地址，切换新栈和 global data。
- `board_init_r()`：完成设备模型、存储、环境变量、显示、网络等重定位后初始化。
- `main_loop()`：执行 preboot，处理 0 秒 bootdelay，然后执行 `bootcmd`；失败才进入命令行。

关键源码：

- ARM64 reset：[`u-boot/arch/arm/cpu/armv8/start.S`](../u-boot/arch/arm/cpu/armv8/start.S)。
- C 运行时入口：[`u-boot/arch/arm/lib/crt0_64.S`](../u-boot/arch/arm/lib/crt0_64.S)。
- 重定位前序列：[`u-boot/common/board_f.c`](../u-boot/common/board_f.c)。
- 重定位后序列：[`u-boot/common/board_r.c`](../u-boot/common/board_r.c)。
- main loop：[`u-boot/common/main.c`](../u-boot/common/main.c)。
- autoboot：[`u-boot/common/autoboot.c`](../u-boot/common/autoboot.c)。
- RK3568 平台初始化：[`u-boot/arch/arm/mach-rockchip/rk3568/`](../u-boot/arch/arm/mach-rockchip/rk3568/)。

### 4.6 阶段 F：U-Boot 选择设备并加载 Linux FIT

当前 `CONFIG_BOOTDELAY=0`。由于 `CONFIG_FIT_SIGNATURE` 和 Android AVB 公钥验证均未启用，默认 `RKIMG_BOOTCOMMAND` 的尝试顺序由 [`u-boot/include/configs/rockchip-common.h`](../u-boot/include/configs/rockchip-common.h) 定义为：

```text
boot_android ${devtype} ${devnum};
boot_fit;
bootrkp;
run distro_bootcmd;
```

当前 `boot.img` 按板级配置应是 Linux FIT，不是 Android boot image，因此正常路径是 Android 格式尝试不成功后由 `boot_fit` 读取 FIT。`boot_fit` 的实现位于 [`u-boot/cmd/bootfit.c`](../u-boot/cmd/bootfit.c)：

1. `fit_image_load_bootables()` 从当前启动块设备取得 `boot` 分区。
2. 读取 FIT 头、kernel、FDT 和可选 ramdisk/resource。
3. 把 kernel 放到 `kernel_addr_r`，DTB 放到 `fdt_addr_r`。
4. 调用 `do_bootm_states()` 完成 `FINDOS/FINDOTHER/LOADOS/OS_PREP/OS_GO`。

Rockchip FIT 的分区读取和加载细节在 [`u-boot/arch/arm/mach-rockchip/fit.c`](../u-boot/arch/arm/mach-rockchip/fit.c)。Linux FIT 的模板在 [`device/rockchip/.chips/rk3566_rk3568/boot.its`](../device/rockchip/.chips/rk3566_rk3568/boot.its)，打包脚本为 [`device/rockchip/common/scripts/mk-fitimage.sh`](../device/rockchip/common/scripts/mk-fitimage.sh)。该 FIT 包含：

- arm64 `Image`；
- `topeet-rk3568-linux.dtb`；
- `resource.img`；
- 当前非-initrd 配置下不包含根文件系统，rootfs 是独立 ext4 分区。

### 4.7 bootargs、根分区和跳转内核

设备树 [`kernel/arch/arm64/boot/dts/rockchip/rk3568-linux.dtsi`](../kernel/arch/arm64/boot/dts/rockchip/rk3568-linux.dtsi) 提供基础命令行：

```text
earlycon=uart8250,mmio32,0xfe660000 console=ttyFIQ0 rw rootwait
```

U-Boot 在 [`u-boot/arch/arm/mach-rockchip/board.c`](../u-boot/arch/arm/mach-rockchip/board.c) 的 `board_fdt_chosen_bootargs()` 中把内核 DT 的 `/chosen/bootargs`、U-Boot 环境和存储参数合并回最终 DT。当前定制的 [`boot_rkimg.c`](../u-boot/arch/arm/mach-rockchip/boot_rkimg.c) 再根据存储介质加入：

```text
storagemedia=...
androidboot.storagemedia=...
androidboot.mode=normal
root=/dev/mmcblk0p6  # eMMC
# 或 root=/dev/mmcblk1p6  # SD
```

最后 [`u-boot/arch/arm/lib/bootm.c`](../u-boot/arch/arm/lib/bootm.c) 的 `boot_jump_linux()` 清理 cache/设备状态，并按 ARM64 启动约定把 DTB 地址放在 `x0`，其他参数为 0，通过 `armv8_switch_to_el2()` 转到 Linux `Image` 入口。

### 4.8 串口日志锚点和 Bootloader 耗时定位

下表中的字符串不是通用示例，而是从当前固件二进制或当前 U-Boot 源码/产物中找到的日志文本。变量部分随板卡和介质变化：

| 阶段 | 当前可定位的日志片段 | 证据与用途 |
|---|---|---|
| BootROM | 没有可由当前源码或产物保证的统一 banner | BootROM 内部无源码；不要凭空指定日志。串口出现第一条 DDR/SPL 文本前的时间只能靠外部串口采集时间戳估算 |
| DDR firmware 开始/版本 | `DDR ... fwver: v1.21`、`MHz(final freq)` | 从 `rk3568_ddr_1560MHz_v1.21.bin` 提取到；用于定位 DDR 初始化区间 |
| Rockchip SPL | `U-Boot SPL 2017.09... fwver: v1.13`、`Trying to boot from %s`、`Jumping to U-Boot(0x%08lx)` | 从当前 `rk356x_spl_v1.13.bin` 提取到；最后一条接近 SPL 转交后级的边界 |
| BL31 | `NOTICE: ... BL31`、`BL31: Initializing runtime services`、`BL31: Initializing BL32`、`BL31: Preparing for EL3 exit to ... world` | 从当前 `rk3568_bl31_v1.44.elf` 提取到；用于定位 EL3 初始化和转交 BL33 |
| OP-TEE | `OP-TEE version: ...`、`Primary CPU initializing`、`Primary CPU switching to normal world boot` | 从当前 `rk3568_bl32_v2.11.bin` 提取到；最后一条接近返回 normal world |
| U-Boot proper | `U-Boot 2017.09 (Jul 05 2026 - 00:40:33 +0800)`、`Model: ...`、`MMC: ...`、`boot mode: normal` | 当前 `u-boot/u-boot` 产物中的 banner 和源码日志；用于定位 U-Boot proper 初始化 |
| 跳转 Linux | `Starting kernel ...` | 当前 U-Boot [`bootm.c`](../u-boot/arch/arm/lib/bootm.c) 对应字符串；是 Bootloader 到 Kernel 的最稳定边界 |

这些早期日志通常不带统一的绝对或相对时间。当前 [`u-boot/.config`](../u-boot/.config) 中 `CONFIG_BOOTSTAGE` 和 `CONFIG_BOOTSTAGE_PRINTF_TIMESTAMP` 都未启用，因此不能从默认 U-Boot 日志直接得到每个内部子阶段耗时。最稳妥的方法是让串口采集程序给每一行添加主机时间戳，再计算上述相邻锚点差值；主机串口时间包含 UART 传输和缓冲误差，适合粗略定位，不应当作微秒级固件性能数据。

## 5. Linux：从内核入口、内建驱动到真实 rootfs

### 5.1 ARM64 早期入口

入口在 [`kernel/arch/arm64/kernel/head.S`](../kernel/arch/arm64/kernel/head.S) 的 `primary_entry`：

```text
primary_entry
  → preserve_boot_args          保存 x0 中的 FDT 地址
  → init_kernel_el              处理从 EL2/EL1 进入的状态
  → __create_page_tables        建立早期页表
  → __cpu_setup                 CPU/MMU 参数
  → __primary_switch            开启 MMU、切换虚拟地址
  → __primary_switched          清 BSS、映射早期 FDT
  → start_kernel
```

这个阶段还没有普通驱动和用户空间，目标是建立能运行 C 内核的最小 CPU、页表和内存环境。

### 5.2 `start_kernel()`：核心内核子系统

主入口是 [`kernel/init/main.c`](../kernel/init/main.c) 的 `start_kernel()`。它依次完成的功能包括：

- 解析早期命令行和设备树；
- 架构初始化、memblock、页分配器、slab/vmalloc；
- scheduler、RCU、workqueue 早期环境；
- IRQ、timer、timekeeping、softirq；
- console；
- VFS、procfs、namespace、cgroup、security 等；
- 进入 `arch_call_rest_init()`/`rest_init()`。

`rest_init()` 创建：

- `kernel_init` 内核线程，它最终成为 PID 1；
- `kthreadd`，管理后续内核线程；
- 原始启动线程随后进入 idle loop。

### 5.3 SMP、initcall 和驱动初始化

`kernel_init_freeable()` 完成 SMP、workqueue 和剩余内核初始化，然后调用：

```text
do_basic_setup()
  → driver_init()
  → do_initcalls()
```

源码仍在 [`kernel/init/main.c`](../kernel/init/main.c)。`do_initcalls()` 按 linker 收集的 initcall 等级执行 `pure/core/postcore/arch/subsys/fs/device/late` 等初始化函数。当前 `.config` 没有启用 `CONFIG_INITCALL_ASYNC`，所以这里不采用该文件中可选的多 worker 并行 initcall 路径；不过各驱动仍可自行排队异步任务或延迟 probe。

### 5.4 内建驱动的注册和 probe

当前 `kernel/.config` 中有 1809 个 `CONFIG_*=y` 条目。内建驱动已经链接进 `Image`，不存在加载 `.ko` 的动作。它们一般通过 `module_init()`/`platform_driver_register()` 等宏生成 initcall：

1. 总线/驱动的 initcall 注册 `struct device_driver`。
2. [`kernel/drivers/of/platform.c`](../kernel/drivers/of/platform.c) 的 `of_platform_default_populate_init()` 在 `arch_initcall_sync` 阶段把设备树节点创建成 platform device。
3. driver core 根据 OF `compatible`、总线 ID 等匹配设备与驱动。
4. [`kernel/drivers/base/dd.c`](../kernel/drivers/base/dd.c) 的 `really_probe()` 设置 pinctrl、DMA、power domain，然后调用总线或驱动的 `probe()`。
5. 依赖的 regulator/clock/PHY/IOMMU 尚未就绪时，驱动可返回 `-EPROBE_DEFER`，稍后重试。因此驱动 probe 顺序不是一张完全固定的线性表。

### 5.5 挂载 rootfs 前后，可加载模块的边界

当前 Ubuntu 根文件系统的 `lib/modules/5.10.160` 有：

- 159 个 `.ko` 文件；
- 663 条 `modules.builtin` 记录；
- `.config` 中 156 个 `=m` 配置项。

159 个 `.ko` 的目录分布是：154 个 media/DVB tuner/frontend 模块、3 个网络模块、1 个蓝牙模块、1 个板级蜂鸣器模块。非 media 模块为：

```text
drivers/bluetooth/rtk_btusb.ko
drivers/my_drivers/beep.ko
drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd/bcmdhd.ko
drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd/dhd_static_buf.ko
drivers/net/wireless/rockchip_wlan/rtl8723du/8723du.ko
```

这些模块存在于文件系统不代表每次开机都会全部进入内核：

- `systemd-modules-load.service` 读取 `/etc/modules` 和 `modules-load.d/*.conf`；当前显式配置只有 CUPS 的 `lp`、`ppdev`、`parport_pc`。
- `systemd-udevd` 和 `systemd-udev-trigger` 对已有设备做 coldplug，根据 modalias 调用 kmod 按需装载模块。
- 当前 `wifibt.service` 执行 [`ubuntu/binary/usr/local/bin/wifi_blue.sh`](../ubuntu/binary/usr/local/bin/wifi_blue.sh)，在识别为 RK3568 时直接 `insmod /usr/local/modules/rk3568/8723du.ko` 和 `rtk_btusb.ko`。
- DVB/tuner 模块只有对应硬件、USB 设备或上层依赖触发 modalias 时才会加载；不能说开机全部加载。
- `beep.ko` 来自 [`kernel/drivers/my_drivers/beep.c`](../kernel/drivers/my_drivers/beep.c)，当前为 `CONFIG_BEEP=m`；没有发现静态 modules-load 项，是否加载取决于实际触发或人工 `modprobe`。

模块依赖和 alias 的权威生成结果在：

- [`ubuntu/binary/lib/modules/5.10.160/modules.dep`](../ubuntu/binary/lib/modules/5.10.160/modules.dep)
- [`ubuntu/binary/lib/modules/5.10.160/modules.alias`](../ubuntu/binary/lib/modules/5.10.160/modules.alias)
- [`ubuntu/binary/lib/modules/5.10.160/modules.builtin`](../ubuntu/binary/lib/modules/5.10.160/modules.builtin)

### 5.6 当前板级设备树启用的主要硬件与驱动源码

板级总入口是 [`topeet-rk3568-linux.dts`](../kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dts)，它包含：

- [`topeet-rk3568-linux.dtsi`](../kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtsi)：板载电源、PMIC、存储、音视频等；
- [`rk3568-linux.dtsi`](../kernel/arch/arm64/boot/dts/rockchip/rk3568-linux.dtsi)：Linux 启动参数和平台调整；
- [`topeet-screen-lcds.dts`](../kernel/arch/arm64/boot/dts/rockchip/topeet-screen-lcds.dts)：屏幕配置；
- [`rk3568.dtsi`](../kernel/arch/arm64/boot/dts/rockchip/rk3568.dtsi)：SoC 控制器节点。

下表列出当前板级 DTS 中明确 `status="okay"` 或直接声明的主要设备。一个功能通常涉及主控制器、PHY、clock、reset、regulator、IOMMU 等多个驱动，因此“源码位置”列给出主驱动入口，而不是声称只有一个文件参与。

| 功能 | DTS 节点/compatible | 主驱动源码位置 |
|---|---|---|
| eMMC | `sdhci` / `rockchip,rk3568-dwcmshc` | [`kernel/drivers/mmc/host/sdhci-of-dwcmshc.c`](../kernel/drivers/mmc/host/sdhci-of-dwcmshc.c) |
| SD 卡 | `sdmmc0` / `rockchip,rk3568-dw-mshc` | [`kernel/drivers/mmc/host/dw_mmc-rockchip.c`](../kernel/drivers/mmc/host/dw_mmc-rockchip.c) |
| SFC、SPI NAND | `sfc` / `rockchip,sfc`、`spi-nand` | [`kernel/drivers/spi/spi-rockchip-sfc.c`](../kernel/drivers/spi/spi-rockchip-sfc.c)、[`kernel/drivers/mtd/nand/spi/`](../kernel/drivers/mtd/nand/spi/) |
| 并行 NAND 控制器 | `nandc0` / `rockchip,rk-nandc-v9` | [`kernel/drivers/rk_nand/rk_nand_base.c`](../kernel/drivers/rk_nand/rk_nand_base.c) |
| 双千兆网口 | `gmac0/gmac1` / `rockchip,rk3568-gmac` | [`kernel/drivers/net/ethernet/stmicro/stmmac/dwmac-rk.c`](../kernel/drivers/net/ethernet/stmicro/stmmac/dwmac-rk.c) |
| PCIe 3.0 x2、PCIe 2.0 x1 | `pcie3x2/pcie2x1` / `rockchip,rk3568-pcie` | [`kernel/drivers/pci/controller/dwc/pcie-dw-rockchip.c`](../kernel/drivers/pci/controller/dwc/pcie-dw-rockchip.c)、[`kernel/drivers/phy/rockchip/phy-rockchip-snps-pcie3.c`](../kernel/drivers/phy/rockchip/phy-rockchip-snps-pcie3.c) |
| USB 3 DRD/Host | `snps,dwc3` | [`kernel/drivers/usb/dwc3/core.c`](../kernel/drivers/usb/dwc3/core.c)，Rockchip wrapper/PHY 在 [`kernel/drivers/usb/dwc3/`](../kernel/drivers/usb/dwc3/) 和 [`kernel/drivers/phy/rockchip/`](../kernel/drivers/phy/rockchip/) |
| USB 2 EHCI/OHCI、USB2 PHY | `usb_host*_ehci/ohci`、`usb2phy*` | [`kernel/drivers/usb/host/`](../kernel/drivers/usb/host/)、[`kernel/drivers/phy/rockchip/phy-rockchip-inno-usb2.c`](../kernel/drivers/phy/rockchip/phy-rockchip-inno-usb2.c) |
| CAN1 | `rockchip,rk3568-can-2.0` | [`kernel/drivers/net/can/rockchip/rockchip_canfd.c`](../kernel/drivers/net/can/rockchip/rockchip_canfd.c) |
| UART4/7/9 | DesignWare 8250 UART | [`kernel/drivers/tty/serial/8250/8250_dw.c`](../kernel/drivers/tty/serial/8250/8250_dw.c) |
| I2C0/2/5 | Rockchip I2C 控制器 | [`kernel/drivers/i2c/busses/i2c-rk3x.c`](../kernel/drivers/i2c/busses/i2c-rk3x.c) |
| PWM0/PWM6 | Rockchip PWM | [`kernel/drivers/pwm/pwm-rockchip.c`](../kernel/drivers/pwm/pwm-rockchip.c) |
| RK809 PMIC/codec | `rockchip,rk809` | [`kernel/drivers/mfd/rk808.c`](../kernel/drivers/mfd/rk808.c)、[`kernel/drivers/regulator/rk808-regulator.c`](../kernel/drivers/regulator/rk808-regulator.c)、[`kernel/sound/soc/codecs/rk817_codec.c`](../kernel/sound/soc/codecs/rk817_codec.c) |
| TCS4525 regulator | `tcs,tcs4525` | [`kernel/drivers/regulator/fan53555.c`](../kernel/drivers/regulator/fan53555.c) |
| RX8010 RTC | `epson,rx8010` | [`kernel/drivers/rtc/rtc-rx8010.c`](../kernel/drivers/rtc/rtc-rx8010.c) |
| SARADC、ADC keys | `rockchip,rk3568-saradc`、`adc-keys` | [`kernel/drivers/iio/adc/rockchip_saradc.c`](../kernel/drivers/iio/adc/rockchip_saradc.c)、[`kernel/drivers/input/keyboard/adc-keys.c`](../kernel/drivers/input/keyboard/adc-keys.c) |
| 温度传感/热管理 | `rockchip,rk3568-tsadc` | [`kernel/drivers/thermal/rockchip_thermal.c`](../kernel/drivers/thermal/rockchip_thermal.c) |
| LED、风扇 | `gpio-leds`/`pwm-leds`、`pwm-fan` | [`kernel/drivers/leds/leds-gpio.c`](../kernel/drivers/leds/leds-gpio.c)、[`kernel/drivers/leds/leds-pwm.c`](../kernel/drivers/leds/leds-pwm.c)、[`kernel/drivers/hwmon/pwm-fan.c`](../kernel/drivers/hwmon/pwm-fan.c) |
| 摄像头 sensor | `ovti,ov5695`、`ovti,ov13850` | [`kernel/drivers/media/i2c/ov5695.c`](../kernel/drivers/media/i2c/ov5695.c)、[`kernel/drivers/media/i2c/ov13850.c`](../kernel/drivers/media/i2c/ov13850.c) |
| Camera CIF/CSI/ISP | `rockchip,rk3568-cif`、`rockchip,rk3568-rkisp` | [`kernel/drivers/media/platform/rockchip/cif/`](../kernel/drivers/media/platform/rockchip/cif/)、[`kernel/drivers/media/platform/rockchip/isp/`](../kernel/drivers/media/platform/rockchip/isp/) |
| VOP/DRM/显示输出 | `rockchip,rk3568-vop` | [`kernel/drivers/gpu/drm/rockchip/rockchip_drm_vop2.c`](../kernel/drivers/gpu/drm/rockchip/rockchip_drm_vop2.c)、[`rockchip_vop2_reg.c`](../kernel/drivers/gpu/drm/rockchip/rockchip_vop2_reg.c) |
| Mali GPU | `arm,mali-bifrost` | 当前配置选中厂商 Bifrost，入口在 [`kernel/drivers/gpu/arm/bifrost/mali_kbase_core_linux.c`](../kernel/drivers/gpu/arm/bifrost/mali_kbase_core_linux.c) |
| NPU | `rockchip,rk3568-rknpu` | [`kernel/drivers/rknpu/rknpu_drv.c`](../kernel/drivers/rknpu/rknpu_drv.c) 及同目录子模块 |
| 视频编解码 MPP | `mpp-service`、RKVDEC、RKVENC、VDPU、VEPU、JPEGD、IEP | [`kernel/drivers/video/rockchip/mpp/`](../kernel/drivers/video/rockchip/mpp/) |
| RGA | `rockchip,rga2` | 当前配置为 multi-RGA，主要在 [`kernel/drivers/video/rockchip/rga2/`](../kernel/drivers/video/rockchip/rga2/) 和 [`rga3/`](../kernel/drivers/video/rockchip/rga3/) |
| DDR devfreq/DFI | `rockchip,rk3568-dmc`、`rockchip,rk3568-dfi` | [`kernel/drivers/devfreq/rockchip_dmc.c`](../kernel/drivers/devfreq/rockchip_dmc.c)、[`kernel/drivers/devfreq/event/rockchip-dfi.c`](../kernel/drivers/devfreq/event/rockchip-dfi.c) |
| I2S/SPDIF/声卡 | I2S0/1、SPDIF、simple/multicodecs card | [`kernel/sound/soc/rockchip/`](../kernel/sound/soc/rockchip/)，codec 在 [`kernel/sound/soc/codecs/`](../kernel/sound/soc/codecs/) |
| 板级 RS485 控制 | `topeet,rs485_ctl` | [`kernel/drivers/misc/485_ctrl/rs485_ctl.c`](../kernel/drivers/misc/485_ctrl/rs485_ctl.c) |

“DTS 中启用”仍不等于 probe 一定成功。实际硬件焊接情况、供电、时钟、I2C 地址、PHY、固件文件和依赖都会影响结果。当前没有板上 `dmesg`，所以本文不能声称上述设备在某块实体板上全部 probe 成功。

### 5.7 与 initramfs first-stage init 方案的差异

高通 Android 平台常见的方案是：

```text
Kernel 解包 boot/vendor_boot 中的完整 initramfs
  → 执行 initramfs 中的 /init（first-stage userspace，PID 1）
  → 加载 UFS、device-mapper、加密或文件系统模块
  → 等待并挂载真实 system/vendor/rootfs
  → switch_root、pivot_root 或 re-exec
  → 进入 second-stage init
```

当前 RK3568 配置没有采用这个用户空间两阶段方案。它把访问根文件系统必需的 eMMC/SD、MMC block 和 ext4 驱动直接编译进 Linux `Image`，先由内核 initcall 完成驱动初始化，再由内核直接挂载真实 ext4 根分区：

```text
Linux kernel
  → do_initcalls()
  → 内建 MMC/SDHCI 驱动 probe
  → 发现 eMMC/SD 和 GPT 分区
  → 出现 mmcblk0p6 或 mmcblk1p6
  → 内核直接挂载 ext4 根文件系统
  → exec /sbin/init
  → systemd 成为用户空间 PID 1
```

还要区分三个概念：

- initramfs 是 CPIO 文件集合，本身不是 PID 1；其中的 `/init` 被 `execve()` 后才成为用户空间 PID 1。
- `kernel_init` 内核线程先取得 PID 1，再 `execve("/init")` 或 `execve("/sbin/init")`；即使发生 re-exec，PID 仍是 1，并不是同时存在两个 PID 1。
- 挂载真实 ext4 rootfs 不会把整个文件系统复制进内存；数据仍在 eMMC/SD 上，内核只在内存中维护 VFS 元数据和按需读取的 page cache。

### 5.8 当前会解包一个极小 CPIO，但它不是可运行的 first-stage initramfs

当前内核配置 [`kernel/.config`](../kernel/.config) 是：

```text
CONFIG_BLK_DEV_INITRD=y
CONFIG_INITRAMFS_SOURCE=""
# CONFIG_INITRD_ASYNC is not set
```

`CONFIG_BLK_DEV_INITRD=y` 只表示内核具有处理 initrd/initramfs 的能力，不表示当前 boot 镜像一定携带完整 initramfs。当前 [`kernel/boot.its`](../kernel/boot.its) 的 FIT `images` 和 `configurations` 只包含：

```text
kernel
fdt
resource
```

其中没有 `ramdisk` image，也没有 configuration 中的 `ramdisk = "..."` 引用，因此当前标准 FIT 设计不会由 U-Boot 额外传入一个外部 initrd。

另一方面，`CONFIG_INITRAMFS_SOURCE=""` 也不表示内核完全没有内置 CPIO。[`kernel/usr/Makefile`](../kernel/usr/Makefile) 在该配置为空时会使用 [`kernel/usr/default_cpio_list`](../kernel/usr/default_cpio_list)，其内容只有：

```text
/dev
/dev/console
/root
```

它没有 `/init`、shell、`.ko`、挂载工具或 `switch_root`。[`kernel/init/initramfs.c`](../kernel/init/initramfs.c) 的 `populate_rootfs()` 仍会作为 `rootfs_initcall` 在 initcall 阶段把这个极小 CPIO 解包到初始 `rootfs`，但不会由此进入用户空间 first-stage init。

初始 `rootfs` 可以由 ramfs 或 tmpfs 支撑。[`kernel/init/do_mounts.c`](../kernel/init/do_mounts.c) 的 `init_rootfs()` 只有在启用 tmpfs、没有 `root=` 且没有其他限制时才选择 tmpfs。当前正常启动有 `root=/dev/mmcblk...`，因此这一短暂初始根按当前代码走 ramfs，而不是高通描述中常说的 tmpfs。

这里还有一条产物边界：当前工作区的 `kernel/boot.img` 缺失，`output/firmware/boot.img` 是悬空链接，所以本文能证明的是“当前配置和标准 FIT 模板没有外部 ramdisk”，不能仅凭当前工作区证明某次历史烧录到实体板的自定义 `boot.img` 一定没有 ramdisk；实际镜像应导出后用 `dumpimage -l` 再核对。

### 5.9 根存储驱动链：为什么无需先从 rootfs 加载 eMMC 和 ext4 模块

当前 [`kernel/.config`](../kernel/.config) 中根挂载所需的主链均为内建：

| 内核配置 | 功能 | 当前形式 |
|---|---|---|
| `CONFIG_MMC=y` | MMC/SD Core | 内建 |
| `CONFIG_MMC_BLOCK=y` | 把 eMMC/SD Card 暴露为 `mmcblkN` 块设备 | 内建 |
| `CONFIG_MMC_SDHCI=y` | 通用 SDHCI Host 框架 | 内建 |
| `CONFIG_MMC_SDHCI_OF_DWCMSHC=y` | RK3568 DWC MSHC/SDHCI Host 驱动 | 内建 |
| `CONFIG_EXT4_FS=y` | ext4 文件系统 | 内建 |

`=y` 表示代码已经链接进 Linux `Image`，不需要先从 `/lib/modules` 读取 `.ko`。这是打破启动循环依赖的关键：

```text
若 eMMC/ext4 驱动是模块且模块只存在于 eMMC rootfs
  → 挂载 rootfs 需要驱动
  → 读取驱动又需要先挂载 rootfs
  → 形成循环依赖
```

解决方式要么像当前工程一样把根设备链内建，要么把相关 `.ko` 和加载工具放入带 `/init` 的完整 initramfs。

当前板载 eMMC 的设备树控制器节点在 [`kernel/arch/arm64/boot/dts/rockchip/rk3568.dtsi`](../kernel/arch/arm64/boot/dts/rockchip/rk3568.dtsi)，`compatible` 是：

```text
rockchip,rk3568-dwcmshc
rockchip,dwcmshc-sdhci
```

板级 [`topeet-rk3568-linux.dtsi`](../kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtsi) 将 `&sdhci` 设为 8-bit、`non-removable`、`status = "okay"`。主要驱动链是：

```text
MMC Core
  kernel/drivers/mmc/core/core.c
    → subsys_initcall(mmc_init)

RK3568 eMMC Host
  kernel/drivers/mmc/host/sdhci-of-dwcmshc.c
    → 匹配 rockchip,rk3568-dwcmshc
    → dwcmshc_probe()
    → sdhci_setup_host()
    → __sdhci_add_host()

MMC 扫描
  kernel/drivers/mmc/host/sdhci.c
  kernel/drivers/mmc/core/host.c
  kernel/drivers/mmc/core/core.c
    → mmc_add_host()
    → mmc_start_host()
    → mmc_rescan()

块设备
  kernel/drivers/mmc/core/block.c
    → mmc_blk_init()
    → mmc_blk_probe()
    → mmcblk0/mmcblk1 及分区节点

ext4
  kernel/fs/ext4/super.c
    → ext4_init_fs()
```

RK3568 eMMC Host 驱动设置了 `PROBE_PREFER_ASYNCHRONOUS`，所以 eMMC 分区可能在 `do_initcalls()` 返回前后的异步扫描中出现；后面的 `wait_for_device_probe()` 和 `rootwait` 正是为这种情况兜底。

### 5.10 等待根分区并挂载、切换真实 rootfs

当前分区表 [`parameter-buildroot-fit.txt`](../device/rockchip/.chips/rk3566_rk3568/parameter-buildroot-fit.txt) 的顺序是：

```text
uboot → misc → boot → recovery → backup → rootfs
```

所以 `rootfs` 是第 6 分区。[`u-boot/arch/arm/mach-rockchip/boot_rkimg.c`](../u-boot/arch/arm/mach-rockchip/boot_rkimg.c) 根据启动介质补充：

```text
eMMC：root=/dev/mmcblk0p6
SD：  root=/dev/mmcblk1p6
```

[`kernel/arch/arm64/boot/dts/rockchip/rk3568-linux.dtsi`](../kernel/arch/arm64/boot/dts/rockchip/rk3568-linux.dtsi) 的 `/chosen/bootargs` 还提供 `rw rootwait`。`rootwait` 表示根块设备尚未出现时继续等待，而不是立即报 `VFS: Unable to mount root fs`。

内核中的精确主路径为：

```text
kernel_init()
  → kernel_init_freeable()
    → do_basic_setup()
      → driver_init()
      → do_initcalls()
        → 解包没有 /init 的极小默认 CPIO
        → 注册并 probe 内建 MMC/SDHCI、block、ext4 等驱动
    → init_eaccess("/init") 失败
    → ramdisk_execute_command = NULL
    → prepare_namespace()
      → wait_for_device_probe()
      → name_to_dev_t("/dev/mmcblk0p6" 或 "/dev/mmcblk1p6")
      → initrd_load() 未接管当前正常流程
      → rootwait 循环等待分区出现和异步 probe 完成
      → mount_root()
        → create_dev("/dev/root")
        → mount_block_root()
        → do_mount_root() 把 ext4 挂到 /root
      → devtmpfs_mount()
      → init_mount(".", "/", MS_MOVE)
      → init_chroot(".")
```

对应源码集中在：

- [`kernel/init/main.c`](../kernel/init/main.c)：`do_basic_setup()`、`do_initcalls()`、`kernel_init_freeable()`、`kernel_init()`；
- [`kernel/init/initramfs.c`](../kernel/init/initramfs.c)：`populate_rootfs()` 和 CPIO 解包；
- [`kernel/init/do_mounts_initrd.c`](../kernel/init/do_mounts_initrd.c)：存在外部/传统 initrd 时的 `initrd_load()` 路径；
- [`kernel/init/do_mounts.c`](../kernel/init/do_mounts.c)：解析 `root=`/`rootwait`、等待块设备、`mount_root()`、移动并切换根目录。

`do_mount_root()` 先把真实 ext4 文件系统挂到 `/root`，随后 `prepare_namespace()` 用 `MS_MOVE` 把它移到 `/` 并 `chroot`。这是内核内部完成的根切换，不是 initramfs 中用户空间 `switch_root` 命令。

### 5.11 从内核 PID 1 直接变成 Ubuntu systemd

真实根挂载成功后，`kernel_init()` 原本会优先尝试 `ramdisk_execute_command`，其默认值是 `/init`；但前面已经确认初始 CPIO 没有 `/init`，所以当前正常路径直接按顺序尝试：

```text
/sbin/init → /etc/init → /bin/init → /bin/sh
```

当前 [`ubuntu/binary/sbin/init`](../ubuntu/binary/sbin/init) 链接到 `/lib/systemd/systemd`，所以内核通过 `kernel_execve()`/`execve` 语义把原来的 `kernel_init` PID 1 变成 systemd PID 1。当前没有临时用户空间 `/init`、用户空间 `switch_root` 和第二个 PID 1。

进入真实 rootfs 后，非根挂载必需的 `.ko` 才可以由 `systemd-modules-load`、udev/modalias 或板级脚本从 `/lib/modules`、`/usr/local/modules` 中按需加载。也就是说，当前工程的边界是：

```text
根挂载必需驱动：内建进 Kernel，在 systemd 前初始化
普通外设模块：   挂载真实 rootfs 后，由 systemd/udev/脚本加载
```

### 5.12 内核、内建驱动和根挂载的日志与耗时边界

当前 `CONFIG_PRINTK_TIME=y`，内核日志前缀 `[秒.微秒]` 是从 Kernel 自己的时间基准开始的相对时间，不包含 BootROM、DDR、SPL、BL31、OP-TEE 和 U-Boot 的耗时。当前 [`arch/arm64/kernel/setup.c`](../kernel/arch/arm64/kernel/setup.c) 中常见的 `Booting Linux on physical CPU ...` 已被注释，不能把它作为本工程保证存在的首条日志；更可靠的 Kernel banner 是 [`init/main.c`](../kernel/init/main.c) 打印 `linux_banner` 后出现的：

```text
[    0.xxxxxx] Linux version 5.10.160 ...
```

当前源码可以确定的关键日志锚点如下：

| 启动位置 | 日志模式 | 当前源码位置 | 可测量的边界 |
|---|---|---|---|
| Kernel C 主流程已开始 | `Linux version 5.10.160 ...` | [`kernel/init/main.c`](../kernel/init/main.c) 的 `pr_notice("%s", linux_banner)` | 与 U-Boot `Starting kernel ...` 配合，粗看 Bootloader→Kernel 交接 |
| devtmpfs 初始化 | `devtmpfs: initialized` | [`kernel/drivers/base/devtmpfs.c`](../kernel/drivers/base/devtmpfs.c) | 内核基础设备模型/VFS 初始化中的一个锚点 |
| 单个内建 initcall 开始 | `calling  <symbol> @ <pid>` | [`kernel/init/main.c`](../kernel/init/main.c) | 仅加 `initcall_debug` 后出现 |
| 单个内建 initcall 完成 | `initcall <symbol> returned <ret> after <usecs> usecs` | [`kernel/init/main.c`](../kernel/init/main.c) | 直接给出该 initcall 同步执行时间 |
| eMMC Card 识别 | `mmc0: new ... MMC card at address ...` | [`kernel/drivers/mmc/core/bus.c`](../kernel/drivers/mmc/core/bus.c) | eMMC Host probe 和 Card 初始化已完成到可注册 card |
| MMC 块设备创建 | `mmcblk0: <id> <name> <capacity>`，随后通常可见 `mmcblk0: p1 ... p6` | [`kernel/drivers/mmc/core/block.c`](../kernel/drivers/mmc/core/block.c) 和通用块分区扫描 | `mmcblk0p6` 可被 `name_to_dev_t()` 找到 |
| 等待根设备 | `Waiting for root device /dev/mmcblk0p6...` | [`kernel/init/do_mounts.c`](../kernel/init/do_mounts.c) | 表示 initcalls 已结束，但异步存储扫描尚未提供根分区；设备已提前出现时此行不会打印 |
| 根挂载完成 | `VFS: Mounted root (ext4 filesystem)... on device ...` | [`kernel/init/do_mounts.c`](../kernel/init/do_mounts.c) | 真实 ext4 rootfs 已挂载 |
| 进入用户空间前 | `Run /sbin/init as init process` | [`kernel/init/main.c`](../kernel/init/main.c) | Kernel 初始化结束、即将 exec systemd |

Linux 没有一条默认日志叫“所有 built-in 驱动开始加载”，也没有一条叫“所有 built-in 驱动加载完成”。原因是内建代码按多个 initcall level 注册，probe 还可能异步或返回 `-EPROBE_DEFER`。要定位这一阶段，应临时在最终 bootargs 中增加：

```text
initcall_debug
```

当前 `CONFIG_KALLSYMS=y`，因此日志能够显示 initcall 符号。分析方法是：

1. 第一条 `calling <symbol>` 代表已进入可见的 initcall 跟踪区间；
2. 最后一批 `initcall ... returned ...` 后，`kernel_init_freeable()` 才会继续检查 `/init` 和执行 `prepare_namespace()`；
3. eMMC 关键链重点搜索 `mmc`、`sdhci`、`dwcmshc`、`ext4`；
4. 用 `VFS: Mounted root` 到 `Run /sbin/init` 观察根切换到用户空间的尾部耗时。

`initcall_debug` 会明显增加串口输出，串口发送本身也会放大启动时间，只适合诊断。实体板上可保存和筛选：

```bash
dmesg -T
dmesg | grep -E 'Linux version|calling  |initcall .* returned|mmc[0-9]|mmcblk|Waiting for root|VFS: Mounted root|Run /sbin/init'
cat /sys/kernel/debug/devices_deferred 2>/dev/null
```

## 6. systemd 从 PID 1 到完整 Ubuntu

### 6.1 systemd 总体阶段

当前默认 target 是 [`graphical.target`](../ubuntu/binary/lib/systemd/system/graphical.target)，其关系是：

```text
sysinit.target
  → basic.target
    → multi-user.target
      → graphical.target
        → display-manager.service
          → lightdm.service
```

systemd 不是简单按文件名字母顺序串行运行服务。它读取 Unit 的 `Requires=`、`Wants=`、`Before=`、`After=`、socket/path/timer 激活关系，构建 transaction，并尽可能并行启动没有顺序冲突的 job。

当前 systemd 本身不是此 SDK 中可修改的源码工程：运行二进制和 unit 文件来自预制 Ubuntu Focal tar 包 [`ubuntu/ubuntu-focal-arm64.tar.xz`](../ubuntu/ubuntu-focal-arm64.tar.xz)。可研究的实际配置位于：

- 系统 unit：[`ubuntu/binary/lib/systemd/system/`](../ubuntu/binary/lib/systemd/system/)
- 启用关系：[`ubuntu/binary/etc/systemd/system/`](../ubuntu/binary/etc/systemd/system/)
- SysV 兼容脚本：[`ubuntu/binary/etc/init.d/`](../ubuntu/binary/etc/init.d/)

Ubuntu 制作脚本 [`ubuntu/mk-rootfs.sh`](../ubuntu/mk-rootfs.sh) 解开 Focal/Jammy 基础 tar、复制 overlay、进入 chroot 更新包；[`ubuntu/mk-image.sh`](../ubuntu/mk-image.sh) 调用 [`ubuntu/post-build.sh`](../ubuntu/post-build.sh) 后制作 ext4 镜像。

### 6.2 sysinit：设备节点、模块和早期板级服务

标准早期服务包括：

- `systemd-journald`：早期日志；
- `systemd-udevd` + `systemd-udev-trigger`：监听 kernel uevent、coldplug 现有设备、按需加载模块和执行 udev rules；
- `systemd-modules-load`：加载静态配置的模块；
- `systemd-sysctl`、tmpfiles、random seed、mount/fsck、pstore 等。

当前 `sysinit.target.wants` 还显式拉起以下板级服务：

| Unit | 功能和执行入口 | 当前实现位置 |
|---|---|---|
| `resize-all.service` | 启动早期扩展分区；`/usr/bin/resize-helper` | unit：[`ubuntu/binary/lib/systemd/system/resize-all.service`](../ubuntu/binary/lib/systemd/system/resize-all.service)；SDK 原始脚本：[`external/rkscript/resize-all.service`](../external/rkscript/resize-all.service) |
| `rkaiq_3A.service` | 启动相机 3A daemon | unit：[`ubuntu/binary/lib/systemd/system/rkaiq_3A.service`](../ubuntu/binary/lib/systemd/system/rkaiq_3A.service)；脚本：[`ubuntu/binary/etc/init.d/rkaiq_3A.sh`](../ubuntu/binary/etc/init.d/rkaiq_3A.sh)；`rkaiq_3A_server` 是预编译二进制，当前树无其源码 |
| `screan-sleep.service` | 延迟 40 秒后执行 `xset s off`、关闭 DPMS | unit：[`ubuntu/binary/lib/systemd/system/screan-sleep.service`](../ubuntu/binary/lib/systemd/system/screan-sleep.service)；脚本：[`ubuntu/binary/usr/bin/screan-sleep.sh`](../ubuntu/binary/usr/bin/screan-sleep.sh) |
| `usbdevice.service` | 配置 USB gadget 功能 | unit：[`external/rkscript/usbdevice.service`](../external/rkscript/usbdevice.service)；实现：[`external/rkscript/usbdevice`](../external/rkscript/usbdevice)；安装逻辑：[`device/rockchip/common/post-hooks/02-usb.sh`](../device/rockchip/common/post-hooks/02-usb.sh) |
| `wifibt.service` | 直接 insmod RTL8723DU 和 RTK BT USB 模块并拉起 HCI | unit：[`ubuntu/binary/lib/systemd/system/wifibt.service`](../ubuntu/binary/lib/systemd/system/wifibt.service)；脚本：[`ubuntu/binary/usr/local/bin/wifi_blue.sh`](../ubuntu/binary/usr/local/bin/wifi_blue.sh) |

注意：`screan-sleep.service` 在 `sysinit.target` 就被拉入，但其脚本需要图形 DISPLAY，先睡眠 40 秒后才调用 `xset`。这是真实文件状态，不代表这是理想依赖设计。

### 6.3 multi-user：后台服务和网络 server

当前根文件系统的 `multi-user.target.wants` 中，主要有效 unit 包括：

```text
ModemManager.service
NetworkManager.service
anacron.service
async.service
avahi-daemon.service
blueman-mechanism.service
console-setup.service
cron.service
cups-browsed.service
cups.path
dmesg.service
kerneloops.service
networkd-dispatcher.service
networking.service
nfs-client.target
nfs-server.service
ntp.service
ondemand.service
pppd-dns.service
remote-fs.target
rpcbind.service
rsyslog.service
snapd.* services
ssh.service
systemd-resolved.service
triggerhappy.service
ubuntu-advantage.service
unattended-upgrades.service
vsftpd.service
wpa_supplicant.service
```

用户通常关心的“server 拉起”可归纳为：

| Server/daemon | 主要作用 | Unit/配置位置 | 源码是否在本树 |
|---|---|---|---|
| `sshd` | SSH 远程登录 | [`ubuntu/binary/lib/systemd/system/ssh.service`](../ubuntu/binary/lib/systemd/system/ssh.service)、`etc/ssh/` | 否，Ubuntu 包中的预编译程序 |
| `vsftpd` | FTP server | [`ubuntu/binary/lib/systemd/system/vsftpd.service`](../ubuntu/binary/lib/systemd/system/vsftpd.service)、[`ubuntu/binary/etc/vsftpd.conf`](../ubuntu/binary/etc/vsftpd.conf) | 否 |
| `nfs-server` | NFS 导出 | [`ubuntu/binary/lib/systemd/system/nfs-server.service`](../ubuntu/binary/lib/systemd/system/nfs-server.service)、`etc/exports` | 否 |
| `rpcbind` | RPC 端口映射，供 NFS 等使用 | [`ubuntu/binary/lib/systemd/system/rpcbind.service`](../ubuntu/binary/lib/systemd/system/rpcbind.service) | 否 |
| `avahi-daemon` | mDNS/DNS-SD | [`ubuntu/binary/lib/systemd/system/avahi-daemon.service`](../ubuntu/binary/lib/systemd/system/avahi-daemon.service) | 否 |
| `cupsd`/`cups-browsed` | 打印服务和远程打印机发现 | [`ubuntu/binary/lib/systemd/system/cups.service`](../ubuntu/binary/lib/systemd/system/cups.service)、`cups-browsed.service` | 否 |
| `NetworkManager` | 网络设备和连接管理 | [`ubuntu/binary/lib/systemd/system/NetworkManager.service`](../ubuntu/binary/lib/systemd/system/NetworkManager.service) | 否 |
| `wpa_supplicant` | Wi-Fi 认证 | [`ubuntu/binary/lib/systemd/system/wpa_supplicant.service`](../ubuntu/binary/lib/systemd/system/wpa_supplicant.service) | 否 |
| `ntpd` | 时间同步 | [`ubuntu/binary/lib/systemd/system/ntp.service`](../ubuntu/binary/lib/systemd/system/ntp.service) | 否 |
| `snapd` | Snap 包管理 daemon/socket | `ubuntu/binary/lib/systemd/system/snapd.*` | 否 |
| `rsyslogd` | 系统日志持久化/转发 | [`ubuntu/binary/lib/systemd/system/rsyslog.service`](../ubuntu/binary/lib/systemd/system/rsyslog.service) | 否 |
| `cron`/`anacron` | 周期任务 | [`ubuntu/binary/lib/systemd/system/cron.service`](../ubuntu/binary/lib/systemd/system/cron.service) | 否 |

`enabled` 只表示 unit 被 target、socket、path 或 alias 引用，不保证 daemon 最终处于 `active`：`ConditionPathExists`、缺失配置、网络/硬件依赖、ExecStart 返回值都可能使其跳过或失败。例如 NFS 是否真正导出内容还取决于 `/etc/exports`，SSH 还会先执行 `sshd -t`。

### 6.4 graphical：LightDM 和桌面

`graphical.target` 要求 `multi-user.target`，并希望启动 `display-manager.service`。当前：

```text
/etc/X11/default-display-manager = /usr/sbin/lightdm
display-manager.service -> /lib/systemd/system/lightdm.service
```

因此实际图形登录入口是 [`ubuntu/binary/lib/systemd/system/lightdm.service`](../ubuntu/binary/lib/systemd/system/lightdm.service)。LightDM 启动 X server/greeter，认证后再启动所选用户桌面 session。虽然根文件系统中也安装了 `gdm3.service`，但当前 display-manager 链接没有选择 GDM。

`graphical.target.wants` 中还有 `accounts-daemon`、`switcheroo-control`、`udisks2`。此外存在一个 `bootanim.service` 启用链接，但其目标 `/lib/systemd/system/bootanim.service` 在当前 Ubuntu 根文件系统中缺失，是当前检查到的唯一悬空 wants 链接，因此它不会成功启动。

### 6.5 socket、path 和 timer 激活

并非所有 server 都在 target 到达时立即创建完整进程：

- sockets：`acpid.socket`、`avahi-daemon.socket`、`cups.socket`、`rpcbind.socket`、`snapd.socket`、`triggerhappy.socket` 等。
- paths：`acpid.path`、`apport-autoreport.path`、`cups.path`、`ntp-systemd-netif.path`。
- timers：`apt-daily*`、`fstrim`、`logrotate`、`man-db`、`motd-news`、`snapd.snap-repair`、`ua-timer` 等。

socket/path/timer 本身先激活，对应条件发生时再拉起 service，这也是“系统已经启动完成但某些 server 进程还没出现”的正常原因。

### 6.6 systemd 日志锚点和用户空间耗时

当前 `/lib/systemd/systemd` 二进制中可以直接找到以下输出格式，target 的显示名称来自各 unit 的 `Description=`：

```text
systemd 245.4-4ubuntu3.24 running in system mode...
Reached target System Initialization.
Reached target Basic System.
Reached target Multi-User System.
Reached target Graphical Interface.
Starting <Unit Description>...
Started <Unit Description>.
Startup finished in ...
```

因此各阶段可用以下日志定位：

| 用户空间边界 | 日志/状态 | 解释 |
|---|---|---|
| systemd 刚成为 PID 1 | `systemd 245.4-4ubuntu3.24 running in system mode...` | 与 Kernel 的 `Run /sbin/init as init process` 构成 Kernel→userspace 边界 |
| sysinit 完成 | `Reached target System Initialization.` | 早期 mount、udev、journald、modules-load 和板级 sysinit job 已完成到 target 条件 |
| basic 完成 | `Reached target Basic System.` | socket、path、timer 和基础服务依赖已建立 |
| 多用户阶段完成 | `Reached target Multi-User System.` | target transaction 已满足；不保证每个 enabled daemon 都健康运行 |
| 图形 target 完成 | `Reached target Graphical Interface.` | 图形 target 已到达；LightDM/桌面会话的实际可用时间还应看对应 unit 和 X/greeter 日志 |
| 单个服务 | `Starting ...`、`Started ...`、`Failed to start ...` | 名称来自 unit 的 `Description=`，可计算单个服务边界 |
| 整体摘要 | `Startup finished in ...` | 当前二进制同时支持“kernel + userspace”和“kernel + initrd + userspace”格式；当前设计没有可运行 first-stage initramfs，实体板实际格式以日志为准 |

systemd 本身已经提供比串口肉眼比较更可靠的计时接口：

```bash
systemd-analyze time
systemd-analyze critical-chain
systemd-analyze blame
systemctl --failed
systemctl --type=service --state=running
systemctl status ssh vsftpd nfs-server rpcbind NetworkManager lightdm rkaiq_3A wifibt
journalctl -b -o short-monotonic
journalctl -b -u rkaiq_3A.service -u wifibt.service -u lightdm.service
```

- `systemd-analyze time` 给出 kernel 和 userspace 总时间；
- `critical-chain` 显示影响 target 到达时间的顺序依赖链；
- `blame` 是各 unit 自身进入 active 所花时间，不能简单相加，因为 unit 可以并行；
- `short-monotonic` 与 Kernel 的相对时间更便于拼接；
- “enabled”只表示存在启用关系，实际拉起结果以 `--state=running`、`--failed` 和 journal 为准。

### 6.7 当前工程最终可证明的完整启动链

在从 eMMC 或 SD 正常启动 Ubuntu、且重新生成的 boot/update 镜像彼此一致的前提下，当前工程由源码、配置和产物共同支持的顺序是：

```text
RK3568 BootROM（芯片内部，无源码）
  → GPT 前的 Rockchip loader 区域
      → rk3568 DDR firmware v1.21（预编译）
      → rk356x SPL v1.13（预编译）
  → uboot 分区：uboot.img
      → BL31 v1.44（预编译）
      → OP-TEE/BL32 v2.11（预编译）
      → U-Boot proper/BL33 2017.09（当前 u-boot/ 源码）
  → misc 分区：确定 normal/recovery 等模式
  → boot 分区：FIT boot.img
      → Linux Image 5.10.160
      → topeet-rk3568-linux.dtb
      → resource.img
  → U-Boot 添加 root=/dev/mmcblk0p6 或 root=/dev/mmcblk1p6
  → Starting kernel ...
  → head.S → start_kernel → rest_init → kernel_init_freeable
  → do_initcalls
      → 解包没有 /init 的极小默认 CPIO
      → 注册内建总线和驱动
      → DT 匹配并 probe RK3568 设备
      → MMC/SDHCI → eMMC/SD card → mmcblk → GPT 分区
      → ext4 文件系统注册
  → prepare_namespace
      → rootwait 等待 mmcblk[0|1]p6
      → 挂载 ext4 rootfs 到 /root
      → MS_MOVE 到 / 并 chroot
  → Run /sbin/init as init process
  → /sbin/init → /lib/systemd/systemd
  → sysinit.target
  → basic.target
  → multi-user.target
  → graphical.target
  → LightDM
  → 图形登录和桌面会话
```

实体板上应先确认静态分析所依赖的实际输入：

```bash
cat /proc/cmdline
cat /proc/device-tree/model
tr '\0' '\n' </proc/device-tree/compatible
cat /sys/firmware/devicetree/base/chosen/bootargs
dmesg | grep -Ei 'mmc|mmcblk|VFS: Mounted root|Run /sbin/init|probe|defer|fail|error'
lsmod
```

当前工作区的 `boot.img` 链接悬空，且已有 `update.img` 早于最新 Ubuntu `rootfs.img`。再次烧录前应重新构建 kernel、Ubuntu rootfs 和 update image，并用 `dumpimage -l boot.img`、文件时间戳和 hash 核对同一批产物；否则静态源码链路不能证明板上运行的正是当前文件组合。
