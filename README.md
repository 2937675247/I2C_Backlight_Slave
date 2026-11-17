# CH32V003 I2C 从机：背光 + 屏幕/触摸复位 + 触摸调试

CH32V003 作为 3.3V 侧 I2C 从机（地址 **0x3C**），主机可控制背光、屏幕 RST、触摸 RST，并读取触摸调试数据。

## 引脚

| 功能 | 引脚 | 说明 |
|---|---|---|
| I2C1 SDA | PC1 | 3.3V 侧，外部上拉 |
| I2C1 SCL | PC2 | 3.3V 侧，外部上拉 |
| 背光 PWM | PA1 / TIM1_CH2 | 20 kHz，接 TPS61169 CTRL |
| 屏幕 RST | PD4 | 开漏输出，外部上拉，低有效 |
| TP SDA | PD0 | 1.8V/3.3V 触摸侧，软件 I2C |
| TP SCL | PD2 | 1.8V/3.3V 触摸侧，软件 I2C |
| TP INT | PD3 | 触摸中断输入 |
| TP RST | PA2 | 开漏输出，低有效 |
| 触摸 INT | PC0 | 开漏输出，低有效，有触摸时拉低给 SoC |

## 完整寄存器定义

| 地址 | 权限 | 名称 | 说明 |
|---|---|---|---|
| `0x00` | R/W | `BL_DUTY` | 背光占空比 0–100 |
| `0x01` | R | `VERSION` | 固件版本 = `0x01` |
| `0x02` | R | `STATUS` | bit0 PWM 运行，bit1 触摸 present，bit2 触摸错误，bit3 触摸有效（点数 1–5） |
| `0x03` | R | `I2C_ADDR` | 从机地址 = `0x3C` |
| `0x04` | R | `TP_PRESENT` | 1 = 0x3B 触摸有 ACK |
| `0x05` | R | `TP_ERR` | 0=OK，1=总线卡死，2=地址 NACK，3=命令 NACK，4=读地址 NACK |
| `0x06` | R | `TP_INT` | PD3 电平 |
| `0x07` | R | `TP_RST` | PA2 电平 |
| `0x08` | R | `TP_POINTS` | 触摸点数（`buf[1]` 低 4 位） |
| `0x09` | R | `TP_ESD` | ESD 状态（`buf[0]` 高 4 位） |
| `0x0A` | R | `TP_EVENT` | 触摸事件（`buf[2]` 高 4 位） |
| `0x0B` | R | `TP_X_L` | X 低字节 |
| `0x0C` | R | `TP_X_H` | X 高字节（低 4 位有效） |
| `0x0D` | R | `TP_Y_L` | Y 低字节 |
| `0x0E` | R | `TP_Y_H` | Y 高字节（低 4 位有效） |
| `0x0F` | R | `TP_ACKS` | ACK 位图：bit0 地址写，bit1 命令，bit2 读地址，bit3 读完 |
| `0x10` | W | `CMD` | bit0=立即探测；bit1=触摸复位+探测；bit2=屏幕复位 |
| `0x11` | R | `TP_PROBE_CNT` | 触摸探测次数 |
| `0x12–0x1F` | R | `TP_RAW0–13` | AXS15260 原始 14 字节 |
| `0x20` | R | `SCR_RST_LEVEL` | PD4 屏幕 RST 电平（1=释放高） |
| `0x21` | R | `SCR_RST_CNT` | 屏幕复位次数 |
| `0x22+` | — | 保留 | 读回 0 |

写 `0x10` 的命令组合：

- `0x01`：只探测触摸
- `0x02`：触摸复位 + 探测
- `0x04`：只复位屏幕
- `0x07`：屏幕复位 + 触摸复位 + 探测（屏幕复位先执行）

## 复位时序

| 信号 | 时序 |
|---|---|
| 屏幕 RST (PD4) | 释放高 10 ms → 拉低 10 ms → 释放 → 等 50 ms |
| 触摸 RST (PA2) | 拉低 10 ms → 释放 → 等 50 ms |
| 上电 | 默认先复位屏幕，再复位触摸，然后首次探测 |

`main.c` 的 `SCREEN_RESET_AT_BOOT` 默认为 `1`；不需要上电自动复位屏幕时改成 `0`。屏幕复位会让画面短暂熄灭，主机需要重新初始化屏幕时请自行处理。

## 触摸 INT（PC0）

- PC0 开漏输出、低有效，外部 3.3V 上拉；MCU 只拉低或释放，不输出高。
- MCU 每 ~10–15 ms 读一次触摸。点数为 1–5 时拉低 PC0，同时 `STATUS.bit3=1`；无触摸、点数无效或读失败时释放 PC0，`STATUS.bit3=0`。
- SoC 侧把 PC0 配成输入 + 上拉 + 下降沿中断。中断后读 0x3C 的 `0x02`（状态）、`0x08`（点数）、`0x0B–0x0E`（X/Y）。
- 这是**电平式**触摸中断：按住时 PC0 一直低，抬手释放。如果需要“每次新数据一个脉冲 + ACK 释放”，可以再加一个 CMD 位。

## 主机命令（Luckfox i2c-2）

```bash
i2cdetect -y 2                 # 应看到 3c

# 背光
i2cset -y 2 0x3c 0x00 80       # 80%
i2cset -y 2 0x3c 0x00 50       # 50%
i2cset -y 2 0x3c 0x00 0        # 关闭

# 触摸：复位 + 探测
i2cset -y 2 0x3c 0x10 0x02
sleep 0.15
i2ctransfer -y 2 w1@0x3c 0x04 r28@0x3c

# 只复位屏幕
i2cset -y 2 0x3c 0x10 0x04
sleep 0.15

# 屏幕复位 + 触摸复位 + 探测
i2cset -y 2 0x3c 0x10 0x07
sleep 0.3

# 读屏幕 RST 电平/复位次数
i2cget -y 2 0x3c 0x20
i2cget -y 2 0x3c 0x21

# 读 0x04~0x21 共 30 个寄存器
i2ctransfer -y 2 w1@0x3c 0x04 r30@0x3c
```

## 当前实测

- 触摸：`present=1`、`err=0`、`TP_ACKS=0x0F`（3.3V 改造后已能完整读取 AXS15260）
- 空闲时点数为无效值，`STATUS.bit3=0`，PC0 释放为高；触摸时点数 1–5，`STATUS.bit3=1`，PC0 拉低
- 屏幕 RST：上电复位后 `SCR_RST_LEVEL=1`、`SCR_RST_CNT=1`
- 背光：`TIM1_CCR2=0x4B0` → 20 kHz / 50%
- 从机：`I2C1_CTLR1=0x0401`、`OADDR1=0x78`（0x3C << 1）

## 停止旧的触摸驱动

触摸现在挂在 MCU 的 1.8V/3.3V 软件 I2C 上，主机不再直接访问 0x3B。旧 `axs15260` 驱动应解绑：

```bash
echo 2-003b > /sys/bus/i2c/drivers/axs15260/unbind
```

## 编译/烧录

MounRiver Studio 打开本目录 → Build。

产物：

- `obj/I2C_Backlight_Slave.hex`
- `obj/I2C_Backlight_Slave.bin`（烧录基地址 `0x00000000`）
- `obj/I2C_Backlight_Slave.elf`
