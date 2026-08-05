# OV5695 从 Probe 到 V4L2 开流的完整工作流程

> 状态：已结合当前 Rockchip Linux 5.10 BSP 源码核对
>
> 分析文件：`kernel/drivers/media/i2c/ov5695.c`
>
> 适用平台：iTOP-RK3568 + OV5695 MIPI CSI-2

## 1. 总体流程

OV5695 从驱动匹配到真正输出图像数据的生命周期可以概括为：

```text
设备树 compatible 匹配
        ↓
执行 ov5695_probe()
        ↓
获取 clock、GPIO、regulator 等硬件资源
        ↓
临时给 Sensor 上电
        ↓
通过 I²C 读取并验证芯片 ID
        ↓
注册 V4L2 subdev 和 Media entity
        ↓
启用 Runtime PM，空闲后关闭 Sensor
        ↓
用户态执行 VIDIOC_STREAMON
        ↓
调用 OV5695 的 .s_stream(1)
        ↓
Runtime PM 再次给 Sensor 上电
        ↓
写入模式寄存器和 V4L2 controls
        ↓
写 0x0100 = 1，Sensor 开始输出 MIPI RAW10
        ↓
DPHY 接收 → RKISP 处理 → DMA 写入 V4L2 buffer
        ↓
用户态执行 VIDIOC_STREAMOFF
        ↓
写 0x0100 = 0，Sensor 进入 standby
        ↓
Runtime PM 关闭时钟、GPIO和供电
```

关键结论是：**Probe 阶段不只是获取资源和注册 subdev，它会临时执行一次真实上电，以读取 OV5695 芯片 ID；真正配置当前图像模式并启动 MIPI 连续传输，则发生在 V4L2 STREAMON 阶段。**

## 2. 设备树匹配与 Probe

设备树中使用：

```dts
compatible = "ovti,ov5695";
```

它会匹配驱动中的：

```c
static const struct of_device_id ov5695_of_match[] = {
        { .compatible = "ovti,ov5695" },
        {},
};
```

I²C core 匹配成功后调用：

```c
ov5695_probe(struct i2c_client *client,
             const struct i2c_device_id *id)
```

## 3. Probe 阶段获取资源

`ov5695_probe()` 首先读取 Rockchip Camera 模组信息：

```text
rockchip,camera-module-index
rockchip,camera-module-facing
rockchip,camera-module-name
rockchip,camera-module-lens-name
```

然后获取设备树描述的时钟、GPIO和供电资源：

```c
ov5695->xvclk = devm_clk_get(dev, "xvclk");

ov5695->reset_gpio =
        devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);

ov5695->pwdn_gpio =
        devm_gpiod_get(dev, "pwdn", GPIOD_OUT_LOW);

ov5695_configure_regulators(ov5695);
```

它们分别对应设备树中的：

```dts
clocks = <&cru CLK_CIF_OUT>;
clock-names = "xvclk";

reset-gpios = <&gpio3 RK_PD4 GPIO_ACTIVE_LOW>;
pwdn-gpios = <&gpio3 RK_PD5 GPIO_ACTIVE_HIGH>;
```

这些 `devm_*_get()` 调用主要用于取得资源句柄，并不等于所有硬件此时已经开启。真正打开时钟、供电并切换 GPIO 的操作在 `__ov5695_power_on()` 中完成。

## 4. 初始化 V4L2 Subdev

Probe 随后初始化 V4L2 subdev：

```c
v4l2_i2c_subdev_init(sd, client, &ov5695_subdev_ops);
ov5695_initialize_controls(ov5695);
```

这里完成的主要工作包括：

- 把 OV5695 注册模型初始化为 I²C V4L2 subdev；
- 关联 OV5695 的 core、video 和 pad 操作集；
- 初始化曝光、模拟增益、数字增益、VBLANK、测试图等 V4L2 controls；
- 设置默认 Sensor mode。

此时仍未开始输出 MIPI 图像数据。

## 5. Probe 阶段临时上电

为了通过 I²C 读取芯片 ID，Probe 会直接调用：

```c
ret = __ov5695_power_on(ov5695);
```

`__ov5695_power_on()` 执行的实际硬件操作是：

```c
clk_set_rate(ov5695->xvclk, OV5695_XVCLK_FREQ);
clk_prepare_enable(ov5695->xvclk);

gpiod_set_value_cansleep(ov5695->reset_gpio, 1);
regulator_bulk_enable(OV5695_NUM_SUPPLIES, ov5695->supplies);
gpiod_set_value_cansleep(ov5695->reset_gpio, 0);
gpiod_set_value_cansleep(ov5695->pwdn_gpio, 1);

usleep_range(delay_us, delay_us * 2);
```

对应的上电顺序可以理解为：

```text
设置外部时钟为 24 MHz
→ 开启 xvclk
→ 让 Sensor 保持复位
→ 开启各路 regulator
→ 释放 reset
→ 退出 power-down
→ 等待至少 8192 个时钟周期
```

OV5695 驱动调用的是 Linux 通用 clock、GPIO 和 regulator 接口；真正操作 RK3568 CRU、GPIO 和 PMU 等硬件寄存器的是各自的子系统驱动。

## 6. 读取并验证芯片 ID

Sensor 上电稳定后，Probe 调用：

```c
ov5695_check_sensor_id(ov5695, client);
```

该函数通过 I²C 从寄存器 `0x300a` 开始读取 24 位芯片 ID：

```c
ov5695_read_reg(client,
                OV5695_REG_CHIP_ID,
                OV5695_REG_VALUE_24BIT,
                &id);
```

期望值为：

```c
CHIP_ID = 0x005695;
```

验证成功后打印：

```text
Detected OV005695 sensor
```

如果芯片 ID 读取失败，应优先检查：

```text
Sensor供电 → 24 MHz xvclk → reset/pwdn极性和时序 → I²C地址与连线
```

这时还没有必要排查 RKISP 或 DRM，因为芯片 ID 读取只依赖 Sensor 本身、I²C 和基础供电时序。

## 7. 注册 Media Entity 和 V4L2 Subdev

芯片 ID 验证成功后，驱动初始化 Sensor 的 source pad：

```c
ov5695->pad.flags = MEDIA_PAD_FL_SOURCE;
sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;

media_entity_pads_init(&sd->entity, 1, &ov5695->pad);
```

然后异步注册 Sensor subdev：

```c
v4l2_async_register_subdev_sensor_common(sd);
```

这表示 OV5695 已经作为 Camera Sensor entity 加入 V4L2/Media Controller 框架，等待 Rockchip DPHY、RKISP 等上游设备根据设备树 endpoint 完成异步绑定。

设备树中的：

```dts
remote-endpoint = <&mipi_in_ucam2>;
data-lanes = <1 2>;
```

描述的是 OV5695 source endpoint 与 Rockchip MIPI DPHY sink endpoint 的拓扑关系。它不是由 `ov5695_probe()` 单独“连接”出来的，而是由 Media Controller、异步 notifier、DPHY 和 RKISP 驱动共同完成 pipeline 建立。

## 8. Probe 结束后进入 Runtime PM 管理

注册 subdev 成功后，Probe 执行：

```c
pm_runtime_set_active(dev);
pm_runtime_enable(dev);
pm_runtime_idle(dev);
```

因为 Probe 前面已经手动上电，所以先通过 `pm_runtime_set_active()` 告诉 Runtime PM 当前设备处于 active 状态，再启用 Runtime PM。

设备没有使用者时，Runtime PM 会调用：

```c
ov5695_runtime_suspend()
        ↓
__ov5695_power_off()
```

`__ov5695_power_off()` 执行：

```c
gpiod_set_value_cansleep(ov5695->pwdn_gpio, 0);
clk_disable_unprepare(ov5695->xvclk);
gpiod_set_value_cansleep(ov5695->reset_gpio, 1);
regulator_bulk_disable(OV5695_NUM_SUPPLIES, ov5695->supplies);
```

因此 Probe 完成后的正常空闲状态通常是：

```text
I²C驱动已绑定
V4L2 subdev已注册
Media entity已创建
但Sensor处于断电/复位或power-down状态
没有输出MIPI数据
```

## 9. 枚举和设置格式阶段

用户态执行格式枚举或 `VIDIOC_S_FMT` 时，V4L2/RKISP 会在 pipeline 中协商格式。对 OV5695 来说，Sensor pad 输出格式是：

```text
MEDIA_BUS_FMT_SBGGR10_1X10
```

设置格式通常用于选择并更新驱动中的：

```c
ov5695->cur_mode
```

它确定后续 STREAMON 时应该写入哪一组寄存器，但单纯枚举或设置格式通常不会立即让 Sensor 开始连续输出数据。

## 10. STREAMON 阶段真正启动 Sensor

用户态执行：

```text
VIDIOC_STREAMON
```

RKISP/Capture 驱动启动整条 pipeline，并最终调用 OV5695 subdev 的：

```c
ov5695_s_stream(sd, 1);
```

`ov5695_s_stream()` 首先执行：

```c
pm_runtime_get_sync(&client->dev);
```

如果 Sensor 此时已经 Runtime Suspend，则会触发：

```c
ov5695_runtime_resume()
        ↓
__ov5695_power_on()
```

也就是重新开启外部时钟、regulator，并按照时序控制 reset/pwdn。

上电成功后调用：

```c
__ov5695_start_stream(ov5695);
```

该函数完成三项关键工作。

首先写入当前模式的寄存器表：

```c
ov5695_write_array(ov5695->client,
                   ov5695->cur_mode->reg_list);
```

这些寄存器配置 Sensor 的 PLL、分辨率、裁剪、行长、帧长、MIPI timing 等工作参数。

然后把当前 V4L2 controls 写入 Sensor：

```c
v4l2_ctrl_handler_setup(&ov5695->ctrl_handler);
```

相关控制项包括：

- 曝光；
- 模拟增益；
- 数字增益；
- VBLANK/VTS；
- 测试图等。

最后写 OV5695 控制寄存器 `0x0100`：

```c
ov5695_write_reg(ov5695->client,
                 OV5695_REG_CTRL_MODE,
                 OV5695_REG_VALUE_08BIT,
                 OV5695_MODE_STREAMING);
```

其含义是：

```text
0x0100 = 1 → Streaming
```

从这一刻开始，OV5695 才真正通过两条 MIPI data lane 连续发送 RAW10 图像数据。

## 11. 数据采集期间各驱动的职责

开始采集后，各模块职责如下：

```text
OV5695驱动
  负责Sensor上电、模式寄存器、曝光/增益和MIPI发送开关

Clock/GPIO/Regulator/Power Domain驱动
  负责真正操作RK3568对应的底层硬件资源

Rockchip MIPI DPHY驱动
  负责配置PHY和lane，接收MIPI CSI-2信号

RKISP驱动
  负责接收RAW10、ISP处理、格式转换和输出路径

V4L2 Capture/videobuf2
  负责buffer队列、DMA完成和QBUF/DQBUF状态流转
```

因此，OV5695 驱动不会直接配置 RKISP，也不会独自完成整个 MIPI pipeline。它只负责 Sensor 自身以及调用通用子系统接口申请和控制 Sensor 所需资源。

## 12. Controls 在断电状态下的处理

`ov5695_set_ctrl()` 会先调用：

```c
pm_runtime_get_if_in_use(&client->dev);
```

如果 Sensor 当前没有上电，函数只更新 V4L2 control 中保存的值，不为写一次 control 而强行唤醒 Sensor。

等到下一次 STREAMON 时：

```c
v4l2_ctrl_handler_setup()
```

会把这些缓存的曝光、增益、VBLANK 等值统一写入已经上电的 Sensor。

这可以避免空闲状态下频繁唤醒设备，也保证开流后使用的是用户最后设置的 controls。

## 13. STREAMOFF 阶段

用户态执行：

```text
VIDIOC_STREAMOFF
```

最终调用：

```c
ov5695_s_stream(sd, 0);
```

驱动先调用：

```c
__ov5695_stop_stream(ov5695);
```

向寄存器 `0x0100` 写入：

```text
0x0100 = 0 → Software Standby
```

Sensor 停止连续输出 MIPI 数据。随后执行：

```c
pm_runtime_put(&client->dev);
```

当 Runtime PM 使用计数降到零后：

```text
ov5695_runtime_suspend()
→ __ov5695_power_off()
→ 关闭pwdn、xvclk、reset和regulator
```

## 14. `s_power()` 与 `s_stream()` 的区别

驱动同时实现了：

```c
ov5695_s_power()
ov5695_s_stream()
```

`s_power(1)` 会通过 Runtime PM 上电，并写入 `ov5695_global_regs`；`s_power(0)` 释放 Runtime PM 引用。

`s_stream(1)` 则负责选择当前 mode、应用 controls，并最终写 `0x0100 = 1` 开始传输；`s_stream(0)` 写 `0x0100 = 0` 停止传输。

两者概念上分别表示：

```text
s_power  → Sensor是否处于可访问、已初始化的供电状态
s_stream → Sensor是否正在连续输出图像数据
```

实际 pipeline 开流的关键入口是 `s_stream()`，而底层供电状态由 Runtime PM 统一维护。

## 15. 一句话总结

当前 OV5695 驱动的完整策略是：

> Probe 时获取设备树资源并临时上电读取芯片 ID，验证成功后注册 V4L2 subdev 和 Media entity，再交给 Runtime PM 在空闲时断电；用户执行 STREAMON 时，Runtime PM 重新打开时钟和供电，驱动写入当前模式及曝光/增益等寄存器，最后写 `0x0100 = 1` 启动 MIPI RAW10 传输；STREAMOFF 时写 `0x0100 = 0` 停流，并通过 Runtime PM 再次关闭 Sensor。

## 16. 关键调用路径速查

```text
设备匹配：
ov5695_of_match
→ ov5695_probe

Probe临时上电识别：
ov5695_probe
→ __ov5695_power_on
→ ov5695_check_sensor_id

Subdev注册：
ov5695_probe
→ media_entity_pads_init
→ v4l2_async_register_subdev_sensor_common

Runtime Suspend：
pm_runtime_idle/put
→ ov5695_runtime_suspend
→ __ov5695_power_off

开始采集：
VIDIOC_STREAMON
→ pipeline调用subdev .s_stream(1)
→ ov5695_s_stream
→ pm_runtime_get_sync
→ ov5695_runtime_resume
→ __ov5695_power_on
→ __ov5695_start_stream
→ ov5695_write_array(cur_mode->reg_list)
→ v4l2_ctrl_handler_setup
→ OV5695_REG_CTRL_MODE = OV5695_MODE_STREAMING

停止采集：
VIDIOC_STREAMOFF
→ ov5695_s_stream(0)
→ __ov5695_stop_stream
→ OV5695_REG_CTRL_MODE = OV5695_MODE_SW_STANDBY
→ pm_runtime_put
→ ov5695_runtime_suspend
→ __ov5695_power_off
```
