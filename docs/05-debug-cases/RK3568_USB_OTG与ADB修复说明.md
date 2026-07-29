# RK3568 USB OTG 与 ADB 修复说明

本文记录 iTOP-RK3568 开发板进入 Ubuntu 后无法被电脑上的 ADB 识别的问题，以及本次实际采用的修复方案。内容从 USB 的基本原理开始介绍，不要求读者预先了解 USB、设备树或者 Linux 驱动。

## 1. 先理解 USB Host、USB Device 和 ADB

USB 通信中的两端并不是完全对等的。一端必须是 **USB Host（主机）**，负责给总线提供控制、发现设备并加载驱动；另一端必须是 **USB Device（设备）**，等待主机枚举并提供具体功能。电脑上的普通 USB 接口通常是 Host，U 盘、鼠标和手机通常作为 Device。只有形成“Host 对 Device”的组合，USB 通信才能正常建立。

RK3568 的一个 USB 控制器可以根据配置充当 Host，也可以充当 Device，因此称为 OTG（On-The-Go）或 DRD（Dual-Role Device，双角色设备）。当开发板作为 Host 时，可以在它的 USB 口上连接 U 盘、鼠标等外设；当开发板作为 Device 时，可以连接电脑并提供 ADB、虚拟网卡、虚拟串口、UVC 摄像头等 USB Gadget 功能。

ADB 可以理解为运行在 USB 之上的一套调试通信协议。电脑运行 `adb` 客户端，板子运行 `adbd` 服务端，但仅仅启动 `adbd` 还不够。板子的 USB 控制器必须先处于 Device 模式，并由 Linux 注册为 UDC（USB Device Controller）；随后 USB Gadget 框架把 ADB 功能绑定到这个 UDC，电脑才能枚举出设备并建立 ADB 连接。其关系可以简化为：

```text
电脑 USB Host
    ↓ USB 枚举
RK3568 UDC（Device 模式）
    ↓ USB Gadget / FunctionFS
板子上的 adbd
    ↓
电脑上的 adb shell
```

如果 RK3568 错误地进入 Host 模式，即使 `adbd` 已经正常运行，ADB 仍然不会工作，因为两个 Host 之间没有 Device 可供枚举。

## 2. 为什么 Windows 可以烧写，Ubuntu 启动后却没有 ADB

这块 iTOP-RK3568 板的设计比较特殊：厂商把 OTG/烧写接口做成了 USB-A 母座。实际连接方式沿用厂商的烧写方式：

```text
电脑原生 USB-A 接口
        ↓
厂商烧写使用的 USB-A 公对公数据线
        ↓
板子指定的 USB-A OTG/烧写接口
```

这里必须使用板子上明确标注的 OTG/烧写口，不能把旁边的普通 USB Host 口当成 OTG 口，也尽量不要经过 Type-C 扩展坞或额外的 USB Hub。

Windows 下能够通过该接口烧写镜像，并不代表 Ubuntu 启动后它一定还是 Device 模式。板子进入 MaskROM 或 Loader 烧写模式时，运行的是芯片 BootROM/Loader 中的 USB 程序，它会把 RK3568 强制设置成 USB Device，所以电脑能够识别 `2207:350a Rockchip USB download gadget`。当 Ubuntu 内核启动后，BootROM 已经退出，USB 角色改由 Linux 设备树和驱动决定。因此，“烧写阶段能识别”和“Linux 阶段 ADB 能识别”是两套不同的软件配置。

当前板级设备树原来是：

```dts
&usbdrd_dwc3 {
        dr_mode = "otg";
        extcon = <&usb2phy0>;
        status = "okay";
};
```

其中：

- `dr_mode = "otg"` 表示允许控制器自动选择 Host 或 Device；
- `extcon = <&usb2phy0>` 表示根据 USB PHY 检测到的 ID/VBUS 等外部连接状态辅助判断角色；
- `status = "okay"` 表示启用该控制器。

由于这块板使用了非标准的 USB-A 形式作为 OTG 口，Linux 的自动角色判断把 `fcc00000.dwc3` 选择成了 Host。于是电脑是 Host，板子也是 Host，双方都试图枚举对方，最终无法建立 USB Device/ADB 通信。

## 3. 本次定位问题的证据

电脑端最初执行：

```bash
adb devices
```

设备列表为空，而且 `lsusb` 没有 Rockchip ADB 设备。板子端持续出现：

```text
usb usb6-port1: Cannot enable. Maybe the USB cable is bad?
```

这类 `usbX-portY` 日志来自 USB Root Hub，说明板子正在以 Host 身份尝试枚举端口。完整启动日志中还有更直接的证据：

```text
xhci-hcd xhci-hcd.0.auto: irq 94, io mem 0xfcc00000
```

`fcc00000` 本来是需要用于 ADB 的 OTG 控制器，现在却被 `xhci-hcd` 注册成了 Host 控制器。

与此同时，板子上的 ADB 上层软件实际上已经正常：

```text
usbdevice.service: active (running)
Starting functions: adb
Preparing instance: ffs.adb
/usr/bin/adbd
```

但下面两个检查结果为空：

```bash
ls -l /sys/class/udc
cat /sys/kernel/config/usb_gadget/rockchip/UDC
```

`/sys/class/udc` 为空说明内核没有可用的 USB Device Controller；Gadget 的 `UDC` 文件为空说明 ADB Gadget 无法绑定控制器。这形成了一条完整的证据链：

```text
usbdevice.service 和 adbd 已启动
        ↓
USB Gadget 的 ADB 功能已准备
        ↓
fcc00000.dwc3 被注册为 xHCI Host
        ↓
/sys/class/udc 为空
        ↓
ADB Gadget 无法绑定 UDC
        ↓
电脑 adb devices 为空
```

所以问题不在电脑 ADB 权限、不在 `adbd` 服务，也不在 root 权限，而在 RK3568 USB 控制器选错了角色。

## 4. 实际采用的修复

修改文件：

```text
kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtsi
```

只把 `dr_mode` 从 `otg` 改为 `peripheral`，本次保守地保留 `extcon`：

```diff
 &usbdrd_dwc3 {
-        dr_mode = "otg";
+        dr_mode = "peripheral";
         extcon = <&usb2phy0>;
         status = "okay";
 };
```

修改后的完整节点为：

```dts
&usbdrd_dwc3 {
        dr_mode = "peripheral";
        extcon = <&usb2phy0>;
        status = "okay";
};
```

`peripheral` 的意思是固定作为 USB Device。这样 DWC3 驱动启动时不再把 `fcc00000` 建立为 xHCI Host，而是把它注册成 UDC，之后 `usbdevice.service` 就能把 `ffs.adb` 绑定到该控制器。

没有在第一步删除 `extcon`，是为了保留板子原有的 VBUS/插拔检测能力。实际验证表明仅修改 `dr_mode` 已经解决问题，因此没有必要扩大修改范围。

这个修改只影响 `fcc00000.dwc3` 对应的指定 OTG口：它在 Linux 下将固定用来连接电脑，不能再通过自动切换作为 Host 接 U 盘或鼠标。板子还有独立的 `fd000000.dwc3` Host 控制器以及其他 EHCI/OHCI Host 控制器，所以其他普通 USB Host 接口不受影响。MaskROM/Loader 烧写通常也不受影响，因为烧写阶段使用 BootROM/Loader 自己的 USB Device配置，而不是 Linux 设备树。

## 5. 编译、镜像校验和烧写

为了防止构建脚本误用其他板型，先显式选择 iTOP-RK3568 配置，然后编译内核：

```bash
cd /home/whd/experiment/rk3568_linux_5.10_20250211/rk3568_linux_5.10
./build.sh rockchip_rk3568_topeet_defconfig
./build.sh kernel
```

本次成功生成：

```text
kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtb
kernel/arch/arm64/boot/Image
kernel/arch/arm64/boot/Image.lz4
kernel/boot.img
```

新生成的 `kernel/boot.img` 是 U-Boot FIT 镜像，其中包含内核、DTB 和 resource。使用 `dumpimage` 检查后，FIT 内部 FDT 的 SHA-256 与新编译 DTB 完全一致，确认设备树修改已经真正打包进 `boot.img`，而不是只生成了一个没有被使用的 DTB。

板子通过下面的命令进入 Loader：

```bash
reboot bootloader
```

电脑端能够看到：

```text
2207:350a Fuzhou Rockchip Electronics Company USB download gadget
```

本 SDK 采用的全量烧写命令是：

```bash
sudo ./rkflash.sh all
```

该命令会写入 loader、parameter、U-Boot、trust、boot、recovery、misc、OEM、userdata 和 rootfs，因此烧写期间不能断电、拔线、关闭终端或让电脑休眠。它是全量烧写，不是只更新 `boot` 分区。

## 6. 最终结果与验证方法

烧写并启动 Ubuntu 后，电脑已经能够稳定识别设备：

```bash
$ adb devices
List of devices attached
0c265e7b71750cfd    device
```

进入板子：

```bash
adb shell
```

能够正常得到 shell。实际检查身份：

```bash
adb shell 'id; whoami; echo HOME=$HOME'
```

结果为：

```text
uid=0(root) gid=0(root) groups=0(root)
root
HOME=/root
```

这说明当前 Rockchip Linux 版 `adbd` 本身已经以 root 身份运行，`adb shell` 直接就是 root shell。执行 `adb root` 时出现：

```text
adb: unable to connect for root: closed
```

不表示没有 root 权限。`adb root` 是 Android 中用于请求 `adbd` 重启并切换 root 的控制命令，而本系统是 Ubuntu，使用的 Rockchip/Linux 版 `adbd` 不一定实现这条 Android 控制协议；同时它本来已经是 UID 0，因此不需要执行 `adb root`。

修复后可以用下面的命令检查整条链路：

```bash
adb devices -l
adb shell 'ls -l /sys/class/udc'
adb shell 'cat /sys/kernel/config/usb_gadget/rockchip/UDC'
adb shell 'cat /sys/class/udc/fcc00000.dwc3/state'
adb shell 'systemctl status usbdevice.service --no-pager -l'
adb shell 'ps -ef | grep "[a]dbd"'
```

正常情况下可以看到 `fcc00000.dwc3`，连接电脑后的 UDC 状态最终为 `configured`，`usbdevice.service` 正常运行，`adbd` 进程属于 root。

现在也可以直接通过 ADB生成并下载 systemd 启动时序图：

```bash
adb shell 'systemd-analyze plot > /tmp/boot.svg'
adb pull /tmp/boot.svg ~/Downloads/boot.svg
xdg-open ~/Downloads/boot.svg
```

本次修复的核心可以概括为：板子上层 ADB 软件原本没有问题，真正的问题是 Linux 将 OTG 控制器错误地选择为 Host；通过设备树把 `fcc00000.dwc3` 固定为 `peripheral`，内核成功注册 UDC，已有的 USB Gadget 和 `adbd` 随即能够与电脑建立 ADB 通信。
