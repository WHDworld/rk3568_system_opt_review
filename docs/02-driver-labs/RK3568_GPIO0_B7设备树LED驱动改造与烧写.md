# RK3568 GPIO0_B7 设备树 LED 驱动改造与烧写

本文记录如何把原来硬编码在 `beep.c` 中的 GPIO0_B7 抽象到设备树，再由驱动通过 Linux GPIO 描述符接口读取和控制。虽然文件名仍叫 `beep.c`，但这个驱动实际用于测试板载 LED，因此本文统一称它为“LED 测试驱动”。

本文针对当前 iTOP-RK3568 工程，相关文件为：

```text
kernel/drivers/my_drivers/beep.c
kernel/drivers/my_drivers/Makefile
kernel/drivers/my_drivers/Kconfig
kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dts
```

## 1. 原驱动为什么不是真正的设备树驱动

原来的 `beep.c` 使用：

```c
#define GPIO0_DR 0xFDD60000
```

再通过：

```c
vir_gpio0_dr = ioremap(GPIO0_DR, 4);
*vir_gpio0_dr = (1 << 31) | (1 << 15);
```

直接操作 GPIO0 数据寄存器的 bit15。GPIO bank 中每组有 8 个引脚，因此 bit15 对应：

```text
GPIO0_B7
```

设备树中的表示方法为：

```dts
<&gpio0 RK_PB7 GPIO_ACTIVE_HIGH>
```

这种直接写寄存器的方法没有告诉 Linux GPIO 子系统“GPIO0_B7 已被当前驱动使用”，也没有从设备树读取引脚、设置 pinmux、申请 GPIO 或处理资源冲突。设备树即使新增一个节点，原驱动也不会读取它。

标准的实现关系应当是：

```text
设备树描述 GPIO0_B7
        ↓
compatible 匹配 platform_driver
        ↓
驱动 probe() 被调用
        ↓
devm_gpiod_get() 读取 led-gpios
        ↓
GPIO 子系统申请并配置 GPIO0_B7
        ↓
gpiod_set_value_cansleep() 控制 LED
```

## 2. GPIO0_B7 为什么会冲突

当前板级设备树 `topeet-rk3568-linux.dts` 已经包含：

```dts
leds {
        compatible = "gpio-leds";

        work {
                gpios = <&gpio0 RK_PB7 GPIO_ACTIVE_HIGH>;
                linux,default-trigger = "heartbeat";
                default-state = "on";
        };
};
```

这个节点会让内核通用 `gpio-leds` 驱动申请 GPIO0_B7，并将它作为心跳灯使用。新的 LED 测试驱动如果再次申请 GPIO0_B7，会出现类似错误：

```text
gpio-15 is already requested
Device or resource busy
probe failed with error -16
```

设备树不会自动让两个驱动轮流使用同一个 GPIO。GPIO 作为独占硬件资源，在同一时刻只能有一个内核驱动作为所有者。

本次改造的目标是让自定义 `beep.c` 控制 GPIO0_B7，因此正确的冲突解决方式是：

```text
禁用或删除原 gpio-leds 节点
        ↓
由新的自定义设备节点描述 GPIO0_B7
        ↓
由新的 beep.c 驱动唯一申请 GPIO0_B7
```

不要保留两个 `status = "okay"` 的消费者，也不要让新驱动继续绕过 GPIO 子系统直接写寄存器。

## 3. 修改板级设备树

修改：

```text
kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dts
```

该文件顶部根节点中原来通过 `LED_PWM` 条件选择 PWM LED 或 GPIO LED：

```dts
#define LED_PWM 0

/ {
        ...

#if LED_PWM
        leds {
                ...
        };
#else
        leds {
                compatible = "gpio-leds";
                work {
                        gpios = <&gpio0 RK_PB7 GPIO_ACTIVE_HIGH>;
                        linux,default-trigger = "heartbeat";
                        default-state = "on";
                };
        };
#endif

        ...
};
```

为了让自定义驱动成为 GPIO0_B7 的唯一所有者，推荐删除上述整个 `#if LED_PWM ... #endif` LED 节点，并将它替换为：

```dts
        // GPIO0_B7 LED测试设备，由自定义beep.c驱动控制
        gpio_led_test: gpio-led-test {
                compatible = "topeet,rk3568-gpio-led-test";
                led-gpios = <&gpio0 RK_PB7 GPIO_ACTIVE_HIGH>;
                pinctrl-names = "default";
                pinctrl-0 = <&gpio_led_test_pin>;
                status = "okay";
        };
```

建议放在根节点内原 `leds` 节点所在的位置，与 `adc-keys`、`rk-485-ctl` 和 `pwm-fan` 平级。修改后的结构示意为：

```dts
/ {
        model = "TOPEET RK3568 EVB1 DDR4 V10 Board";
        compatible = "rockchip,rk3568-evb1-ddr4-v10", "rockchip,rk3568";

        adc_keys: adc-keys {
                ...
        };

        rk_485_ctl: rk-485-ctl {
                ...
        };

        // GPIO0_B7 LED测试设备
        gpio_led_test: gpio-led-test {
                compatible = "topeet,rk3568-gpio-led-test";
                led-gpios = <&gpio0 RK_PB7 GPIO_ACTIVE_HIGH>;
                pinctrl-names = "default";
                pinctrl-0 = <&gpio_led_test_pin>;
                status = "okay";
        };

        fan: pwm-fan {
                ...
        };
};
```

各属性含义如下：

```dts
compatible = "topeet,rk3568-gpio-led-test";
```

用于匹配自定义驱动的 `of_match_table`。

```dts
led-gpios = <&gpio0 RK_PB7 GPIO_ACTIVE_HIGH>;
```

表示该设备使用 GPIO0_B7，高电平为逻辑有效状态。由于 LED 原节点也是 `GPIO_ACTIVE_HIGH`，这里保持原有极性。

```dts
pinctrl-0 = <&gpio_led_test_pin>;
```

表示设备启用时应用对应 pinctrl，把引脚复用为 GPIO。

然后在同一个 `topeet-rk3568-linux.dts` 文件末尾添加：

```dts
&pinctrl {
        gpio-led-test {
                gpio_led_test_pin: gpio-led-test-pin {
                        rockchip,pins =
                                <0 RK_PB7 RK_FUNC_GPIO &pcfg_pull_none>;
                };
        };
};
```

含义为：

```text
0              GPIO bank 0
RK_PB7         B组第7个引脚
RK_FUNC_GPIO   复用为普通GPIO
pcfg_pull_none 不设置内部上拉或下拉
```

如果不想直接删除原 `gpio-leds` 节点，也可以给其父节点增加：

```dts
status = "disabled";
```

例如：

```dts
leds {
        status = "disabled";
        compatible = "gpio-leds";

        work {
                gpios = <&gpio0 RK_PB7 GPIO_ACTIVE_HIGH>;
                linux,default-trigger = "heartbeat";
                default-state = "on";
        };
};
```

不过本次已经确定由自定义驱动接管 GPIO0_B7，删除旧节点并替换成新节点更清晰，不会在设备树里同时留下两个相互矛盾的硬件描述。

## 4. 将 beep.c 改为设备树平台驱动

推荐用下面的完整内容替换原 `kernel/drivers/my_drivers/beep.c`：

```c
// SPDX-License-Identifier: GPL-2.0
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

struct gpio_led_test {
        struct gpio_desc *led_gpio;
        struct miscdevice miscdev;
};

static ssize_t gpio_led_test_read(struct file *file, char __user *buf,
                                  size_t count, loff_t *ppos)
{
        struct miscdevice *miscdev = file->private_data;
        struct gpio_led_test *led =
                container_of(miscdev, struct gpio_led_test, miscdev);
        char value;

        if (*ppos != 0)
                return 0;

        value = gpiod_get_value_cansleep(led->led_gpio) ? '1' : '0';

        if (count < 1)
                return -EINVAL;

        if (copy_to_user(buf, &value, 1))
                return -EFAULT;

        *ppos = 1;
        return 1;
}

static ssize_t gpio_led_test_write(struct file *file,
                                   const char __user *buf,
                                   size_t count, loff_t *ppos)
{
        struct miscdevice *miscdev = file->private_data;
        struct gpio_led_test *led =
                container_of(miscdev, struct gpio_led_test, miscdev);
        char value;

        if (count < 1)
                return -EINVAL;

        if (copy_from_user(&value, buf, 1))
                return -EFAULT;

        /*
         * 同时支持：
         *   echo 1 > /dev/gpio_led_test
         *   printf '\\x01' > /dev/gpio_led_test
         */
        if (value == '1' || value == 1)
                gpiod_set_value_cansleep(led->led_gpio, 1);
        else if (value == '0' || value == 0)
                gpiod_set_value_cansleep(led->led_gpio, 0);
        else
                return -EINVAL;

        return count;
}

static const struct file_operations gpio_led_test_fops = {
        .owner = THIS_MODULE,
        .read = gpio_led_test_read,
        .write = gpio_led_test_write,
};

static int gpio_led_test_probe(struct platform_device *pdev)
{
        struct gpio_led_test *led;
        int ret;

        led = devm_kzalloc(&pdev->dev, sizeof(*led), GFP_KERNEL);
        if (!led)
                return -ENOMEM;

        /*
         * 参数 "led" 对应设备树属性 led-gpios。
         * GPIOD_OUT_LOW 表示申请成功后默认输出逻辑0，防止启动时LED误亮。
         */
        led->led_gpio =
                devm_gpiod_get(&pdev->dev, "led", GPIOD_OUT_LOW);
        if (IS_ERR(led->led_gpio)) {
                ret = PTR_ERR(led->led_gpio);
                dev_err(&pdev->dev,
                        "failed to get led GPIO: %d\n", ret);
                return ret;
        }

        led->miscdev.minor = MISC_DYNAMIC_MINOR;
        led->miscdev.name = "gpio_led_test";
        led->miscdev.fops = &gpio_led_test_fops;
        led->miscdev.parent = &pdev->dev;

        ret = misc_register(&led->miscdev);
        if (ret) {
                dev_err(&pdev->dev,
                        "failed to register misc device: %d\n", ret);
                return ret;
        }

        platform_set_drvdata(pdev, led);

        dev_info(&pdev->dev,
                 "GPIO LED test driver probed successfully\n");
        return 0;
}

static int gpio_led_test_remove(struct platform_device *pdev)
{
        struct gpio_led_test *led = platform_get_drvdata(pdev);

        gpiod_set_value_cansleep(led->led_gpio, 0);
        misc_deregister(&led->miscdev);
        return 0;
}

static const struct of_device_id gpio_led_test_of_match[] = {
        {
                .compatible = "topeet,rk3568-gpio-led-test",
        },
        { }
};
MODULE_DEVICE_TABLE(of, gpio_led_test_of_match);

static struct platform_driver gpio_led_test_driver = {
        .probe = gpio_led_test_probe,
        .remove = gpio_led_test_remove,
        .driver = {
                .name = "rk3568-gpio-led-test",
                .of_match_table = gpio_led_test_of_match,
        },
};

module_platform_driver(gpio_led_test_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("WHD");
MODULE_DESCRIPTION("RK3568 GPIO LED test driver using device tree");
```

这个版本不再包含：

```c
#define GPIO0_DR
ioremap()
iounmap()
直接读写 vir_gpio0_dr
```

GPIO 的地址、编号、方向、有效电平和资源冲突都交给 GPIO/pinctrl 子系统管理。

设备树和驱动之间有两组必须严格一致的对应关系：

```text
设备树 compatible:
topeet,rk3568-gpio-led-test

驱动 of_match_table:
topeet,rk3568-gpio-led-test
```

以及：

```text
设备树属性:
led-gpios

驱动调用:
devm_gpiod_get(..., "led", ...)
```

如果字符串不同，驱动不会匹配或无法获得 GPIO。

## 5. 检查 Kconfig 和 Makefile

当前 `kernel/drivers/my_drivers/Makefile` 已经有：

```makefile
obj-$(CONFIG_BEEP) += beep.o
```

当前 `kernel/drivers/Makefile` 已经包含：

```makefile
obj-y += base/ block/ misc/ my_drivers/ mfd/ nfc/
```

当前 `kernel/drivers/Kconfig` 也已经引用：

```text
source "drivers/my_drivers/Kconfig"
```

因此通常不需要再改上层 Makefile/Kconfig。

确认 `kernel/drivers/my_drivers/Kconfig` 中定义类似：

```text
config BEEP
        tristate "RK3568 GPIO LED test driver"
        default m
        help
          Device-tree based GPIO0_B7 LED test driver for iTOP-RK3568.
```

如果希望生成模块 `beep.ko`，配置应为：

```text
CONFIG_BEEP=m
```

检查：

```bash
grep CONFIG_BEEP kernel/.config
```

预期：

```text
CONFIG_BEEP=m
```

如果没有启用，可以运行：

```bash
./build.sh kernel-config
```

在菜单中将该驱动选为模块，或者在确认配置项存在后使用：

```bash
scripts/config --file kernel/.config --module BEEP
```

随后执行：

```bash
make -C kernel ARCH=arm64 olddefconfig
```

## 6. 单独编译 DTB

进入 SDK 根目录：

```bash
cd /home/whd/experiment/rk3568_linux_5.10_20250211/rk3568_linux_5.10
```

显式选择正确板型：

```bash
./build.sh rockchip_rk3568_topeet_defconfig
```

设置交叉编译器：

```bash
export CROSS_COMPILE=$PWD/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
```

单独编译板级 DTB：

```bash
make -C kernel \
    ARCH=arm64 \
    CROSS_COMPILE="$CROSS_COMPILE" \
    rockchip/topeet-rk3568-linux.dtb
```

输出文件：

```text
kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtb
```

反编译检查新节点：

```bash
kernel/scripts/dtc/dtc \
    -I dtb \
    -O dts \
    kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtb \
    > /tmp/topeet-rk3568-linux.compiled.dts
```

查询：

```bash
grep -A12 -B2 \
    'topeet,rk3568-gpio-led-test' \
    /tmp/topeet-rk3568-linux.compiled.dts
```

同时确认编译后的 DTB 中只剩一个 GPIO0_B7 使用者。反编译后的 phandle 数值不容易直接阅读，因此还应在源码中检查：

```bash
rg -n 'RK_PB7' \
    kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dts \
    kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtsi
```

预期 GPIO0_B7 只出现在新的 `led-gpios` 和 pinctrl 配置中，不再出现在启用的 `gpio-leds` 节点中。

## 7. 单独编译 beep.ko

如果内核已经完整编译过，并且 `.config` 中有：

```text
CONFIG_BEEP=m
```

可以只编译该目录：

```bash
make -C kernel \
    ARCH=arm64 \
    CROSS_COMPILE="$CROSS_COMPILE" \
    M=drivers/my_drivers \
    modules
```

输出：

```text
kernel/drivers/my_drivers/beep.ko
```

检查模块信息：

```bash
file kernel/drivers/my_drivers/beep.ko
modinfo kernel/drivers/my_drivers/beep.ko
```

模块必须与板子当前运行的内核版本和配置一致。检查两边版本：

```bash
modinfo -F vermagic kernel/drivers/my_drivers/beep.ko
adb shell uname -r
```

若版本或 SMP/preempt/module-unload 等标志不一致，应重新编译并烧写同一套 kernel/boot.img，不要强制加载不匹配模块。

## 8. 重新生成包含 DTB 的 boot.img

当前 RK3568 的 boot 分区使用 FIT 格式 `boot.img`，里面包含：

```text
kernel
fdt
resource
```

裸 DTB 不能直接写到 boot 分区，否则会破坏启动镜像。

完成设备树修改后，推荐执行：

```bash
./build.sh kernel
```

这是增量构建。已经生成且未变化的目标通常不会从头编译，修改后的 DTB、驱动和 FIT 会按依赖关系重新生成。

也可以在已有有效内核产物的情况下执行板级镜像目标：

```bash
make -C kernel \
    ARCH=arm64 \
    CROSS_COMPILE="$CROSS_COMPILE" \
    topeet-rk3568-linux.img
```

最终输出：

```text
kernel/boot.img
```

检查 FIT 内容：

```bash
u-boot/tools/dumpimage -l kernel/boot.img
```

应看到：

```text
Image 0 (fdt)
Image 1 (kernel)
Image 2 (resource)
```

计算裸 DTB 哈希：

```bash
sha256sum \
    kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtb
```

该哈希应与 `dumpimage -l kernel/boot.img` 中 `Image 0 (fdt)` 的 SHA-256 一致，证明新 DTB 已被打包进 boot.img。

## 9. 烧写 boot.img

让板子进入 Loader：

```bash
adb reboot bootloader
```

电脑确认：

```bash
lsusb | grep 2207
```

预期类似：

```text
2207:350a Fuzhou Rockchip Electronics Company USB download gadget
```

只修改设备树和内核时，推荐只写 boot 分区：

```bash
sudo ./rkflash.sh boot kernel/boot.img
```

然后复位：

```bash
sudo ./rkflash.sh rd
```

若决定沿用全量烧写流程，可以执行：

```bash
sudo ./rkflash.sh all
```

但 `all` 会覆盖 parameter、U-Boot、trust、boot、recovery、misc、OEM、userdata 和 rootfs。仅修改 GPIO 设备树与驱动时，优先使用 `boot`，避免覆盖板子上的 rootfs 和用户数据。

如果 `beep.ko` 按模块编译，它通常不在 boot.img 内，而应安装到 rootfs 的模块目录，或者在实验阶段通过 ADB 临时传入。因此只刷 boot.img 后还需要按下一节加载新模块。

## 10. 启动后验证

等待 ADB 恢复：

```bash
adb wait-for-device
adb devices
```

检查运行设备树中的节点：

```bash
adb shell '
node=/proc/device-tree/gpio-led-test
if [ -d "$node" ]; then
    echo "node exists: $node"
    echo -n "compatible: "
    tr -d "\0" < "$node/compatible"
    echo
else
    echo "gpio-led-test node not found"
fi
'
```

预期：

```text
node exists: /proc/device-tree/gpio-led-test
compatible: topeet,rk3568-gpio-led-test
```

确认旧的通用 heartbeat LED 没有继续占用该 GPIO：

```bash
adb shell 'ls -l /sys/class/leds'
```

原来的 `work` heartbeat LED 不应再出现。

把新模块复制到板子：

```bash
adb push kernel/drivers/my_drivers/beep.ko /tmp/beep.ko
```

如果旧模块已经加载，先关闭 LED 并卸载：

```bash
adb shell 'echo 0 > /dev/gpio_led_test 2>/dev/null || true'
adb shell 'rmmod beep 2>/dev/null || true'
```

加载新模块：

```bash
adb shell 'insmod /tmp/beep.ko'
```

检查日志：

```bash
adb shell 'dmesg | tail -n 50'
```

预期出现：

```text
GPIO LED test driver probed successfully
```

检查设备节点：

```bash
adb shell 'ls -l /dev/gpio_led_test'
```

点亮 LED：

```bash
adb shell 'echo 1 > /dev/gpio_led_test'
```

关闭 LED：

```bash
adb shell 'echo 0 > /dev/gpio_led_test'
```

读取当前逻辑状态：

```bash
adb shell 'cat /dev/gpio_led_test; echo'
```

卸载驱动：

```bash
adb shell 'rmmod beep'
```

卸载时驱动会先把 LED 设置为逻辑 0，再注销 misc 设备。

## 11. 常见错误定位

如果 `insmod` 后没有调用 `probe()`，检查：

```bash
adb shell 'tr -d "\0" < /proc/device-tree/gpio-led-test/compatible'
modinfo kernel/drivers/my_drivers/beep.ko | grep alias
```

设备树和模块中必须同时出现：

```text
topeet,rk3568-gpio-led-test
```

如果出现：

```text
failed to get led GPIO: -16
```

说明 GPIO0_B7 仍被其他驱动占用。重新检查设备树中旧 `gpio-leds` 节点是否真的删除或禁用，并搜索所有引用：

```bash
rg -n 'RK_PB7' kernel/arch/arm64/boot/dts/rockchip
```

如果出现：

```text
failed to get led GPIO: -2
```

通常表示驱动找不到 `led-gpios` 属性，检查：

```text
驱动使用 devm_gpiod_get(..., "led", ...)
设备树属性名为 led-gpios
```

如果 `/proc/device-tree/gpio-led-test` 不存在，说明板子没有启动新 DTB。检查：

```bash
u-boot/tools/dumpimage -l kernel/boot.img
sha256sum kernel/arch/arm64/boot/dts/rockchip/topeet-rk3568-linux.dtb
```

并确认烧写的是刚生成的：

```text
kernel/boot.img
```

如果 `insmod` 提示：

```text
Invalid module format
```

比较：

```bash
modinfo -F vermagic kernel/drivers/my_drivers/beep.ko
adb shell uname -r
```

模块和运行内核必须由同一份源码与配置编译。

## 12. 最终结构

改造完成后的所有权关系为：

```text
GPIO0_B7
   ↓ 仅由一个设备节点描述
gpio-led-test
   ↓ compatible匹配
gpio_led_test_driver
   ↓ devm_gpiod_get("led")
Linux GPIO子系统独占申请GPIO0_B7
   ↓
/dev/gpio_led_test
   ↓
echo 1 / echo 0 控制LED
```

原来的通用 `gpio-leds` 节点必须退出 GPIO0_B7 的所有权。这样既解决冲突，也让引脚编号从驱动源代码中消失。以后如果将 LED 改接到其他 GPIO，只需修改设备树中的：

```dts
led-gpios = <&gpioX RK_PYX GPIO_ACTIVE_HIGH>;
```

以及对应 pinctrl，驱动代码无需修改。这就是将硬件差异抽象到设备树的主要意义。
