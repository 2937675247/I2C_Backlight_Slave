/********************************** (C) COPYRIGHT *******************************
 * File Name          : main.c
 * Description        : CH32V003 I2C 从机背光控制器（TPS61169）
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/

/*
 * 架构：
 *   3.3V 主机侧：PC2 = I2C1_SCL，PC1 = I2C1_SDA（外部 3.3V 上拉）
 *   1.8V TP 侧：PD0/PD2/PD3/PA2 全部开漏释放，不再透传触摸
 *   背光：PA1 = TIM1_CH2，20 kHz PWM -> TPS61169 CTRL
 *
 * I2C 从机地址：0x3C（7 位，可改 I2C_SLAVE_ADDR）
 *
 * 寄存器表：
 *   0x00  R/W  背光占空比 0~100
 *   0x01  R    版本 = 0x01
 *   0x02  R    状态 = 0x01（PWM 运行）
 *   0x03  R    I2C 地址 = 0x3C
 *
 * 主机（Luckfox i2c-2）示例：
 *   i2cdetect -y 2
 *   i2cset -y 2 0x3c 0x00 80     # 80% 亮度
 *   i2cget -y 2 0x3c 0x00        # 读回
 *   i2cset -y 2 0x3c 0x00 0      # 关闭背光
 */

#include "debug.h"

/* I2C 从机地址和速率 */
#define I2C_SLAVE_ADDR   0x3C
#define I2C_SPEED_HZ     100000u

/* 寄存器地址 */
#define REG_BL_DUTY       0x00
#define REG_VERSION       0x01
#define REG_STATUS        0x02
#define REG_I2C_ADDR      0x03
#define REG_TP_PRESENT    0x04   /* 1 = 0x3B 地址有 ACK */
#define REG_TP_ERR        0x05   /* 0=OK 1=总线卡死 2=地址NACK 3=命令NACK 4=读地址NACK */
#define REG_TP_INT        0x06   /* PD3 电平 */
#define REG_TP_RST        0x07   /* PA2 电平 */
#define REG_TP_POINTS     0x08   /* buf[1] 点数 */
#define REG_TP_ESD        0x09   /* buf[0]>>4 */
#define REG_TP_EVENT      0x0A   /* buf[2]>>4 */
#define REG_TP_X_L        0x0B
#define REG_TP_X_H        0x0C
#define REG_TP_Y_L        0x0D
#define REG_TP_Y_H        0x0E
#define REG_TP_ACKS       0x0F   /* bit0 地址写 bit1 命令 bit2 读地址 bit3 读完 */
#define REG_CMD           0x10   /* 写 1 = 立即探测一次 */
#define REG_TP_PROBE_CNT  0x11
#define REG_TP_RAW0       0x12   /* 0x12~0x1F: 原始数据前 14 字节（兼容单点） */
#define REG_SCR_RST_LEVEL 0x20   /* PD4 屏幕复位电平 */
#define REG_SCR_RST_CNT   0x21   /* 屏幕复位次数 */
#define REG_TP_TOUCH_CNT  0x22   /* PC0 触摸有效次数（排查误触发） */
#define REG_MT_POINTS     0x23   /* 多点：点数 0~5 */
#define REG_MT_POINT1     0x24   /* 0x24~0x3C: 5 点 × 5 字节（X_L,X_H,Y_L,Y_H,EVENT） */
#define REG_MT_VALID      0x3D   /* bit0~4: 对应点有效 */
#define REG_MT_ACTIVE     0x3E   /* 1 = 任一点触摸有效 */
#define I2C_REG_COUNT     0x40

/* 背光 PWM */
#define PWM_FREQ_HZ      20000u
#define PWM_DUTY_DEFAULT 50u

/* 上电是否自动给屏幕 RST（PD4）一个低脉冲；不需要改成 0 */
#define SCREEN_RESET_AT_BOOT 1

static uint32_t s_pwm_period = 0;

/* I2C 从机寄存器（中断会修改，必须 volatile） */
static volatile uint8_t s_i2c_regs[I2C_REG_COUNT] = {
    PWM_DUTY_DEFAULT, 0x01, 0x01, I2C_SLAVE_ADDR
};

static volatile uint8_t s_probe_request = 0;
static volatile uint8_t s_reset_request = 0;
static volatile uint8_t s_screen_reset_request = 0;

static volatile uint8_t s_offset = 0;
static volatile uint8_t s_position = 0;
static volatile uint8_t s_first_write = 1;

/* GPIO 直接操作宏 */
#define PIN_LOW(port, pin)         ((port)->BSHR = ((uint32_t)(pin) << 16))
#define PIN_RELEASE(port, pin)     ((port)->BSHR = ((uint32_t)(pin)))

/*********************************************************************
 * @fn      PA1_PWM_Init
 *
 * @brief   PA1 (TIM1_CH2) 输出 PWM，供 TPS61169 CTRL 调光
 *
 * @param   freq_hz       PWM 频率
 *          duty_percent  初始占空比 0~100
 *
 * @return  none
 */
static void PA1_PWM_Init(uint32_t freq_hz, uint8_t duty_percent)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure = {0};
    TIM_OCInitTypeDef TIM_OCInitStructure = {0};
    uint32_t tim_clk = SystemCoreClock;
    uint32_t period;

    if(freq_hz == 0)
        freq_hz = PWM_FREQ_HZ;

    period = (tim_clk + freq_hz / 2u) / freq_hz;
    if(period < 2u)
        period = 2u;
    if(period > 65535u)
        period = 65535u;
    s_pwm_period = period;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_TIM1 | RCC_APB2Periph_AFIO, ENABLE);

    /* PA1/PA2 作为 GPIO/AF，而不是晶振脚；TIM1 默认映射 CH2=PA1 */
    AFIO->PCFR1 &= ~((1u << 15) | (0x3u << 6));

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_30MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    TIM_TimeBaseInitStructure.TIM_Period            = (uint16_t)(period - 1u);
    TIM_TimeBaseInitStructure.TIM_Prescaler         = 0;
    TIM_TimeBaseInitStructure.TIM_ClockDivision     = TIM_CKD_DIV1;
    TIM_TimeBaseInitStructure.TIM_CounterMode       = TIM_CounterMode_Up;
    TIM_TimeBaseInitStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM1, &TIM_TimeBaseInitStructure);

    TIM_OCInitStructure.TIM_OCMode      = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_Pulse       = (uint16_t)((period * duty_percent) / 100u);
    TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_High;
    TIM_OC2Init(TIM1, &TIM_OCInitStructure);

    TIM_OC2PreloadConfig(TIM1, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(TIM1, ENABLE);
    TIM_CtrlPWMOutputs(TIM1, ENABLE);
    TIM_Cmd(TIM1, ENABLE);
}

/*********************************************************************
 * @fn      PA1_PWM_SetDuty
 *
 * @brief   修改背光 PWM 占空比
 *
 * @param   duty_percent  0~100
 *
 * @return  none
 */
static void PA1_PWM_SetDuty(uint8_t duty_percent)
{
    uint32_t ccr;

    if(duty_percent > 100u)
        duty_percent = 100u;

    ccr = (s_pwm_period * duty_percent) / 100u;
    TIM_SetCompare2(TIM1, (uint16_t)ccr);
}

/*********************************************************************
 * @fn      GPIO_OD_Init
 *
 * @brief   配置为开漏输出，并预先释放为高阻（无内部上拉）
 *
 * @return  none
 */
static void GPIO_OD_Init(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};

    /* 先把输出锁存器置 1，避免初始化瞬间拉低 */
    PIN_RELEASE(port, pin);

    GPIO_InitStructure.GPIO_Pin   = pin;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_10MHz;
    GPIO_Init(port, &GPIO_InitStructure);

    PIN_RELEASE(port, pin);
}

/*********************************************************************
 * @fn      TP_Pins_Safe_Init
 *
 * @brief   1.8V TP 侧不再透传：全部开漏释放，不开启内部上拉
 *
 * @return  none
 */
static void TP_Pins_Safe_Init(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOC |
                           RCC_APB2Periph_GPIOD, ENABLE);

    GPIO_OD_Init(GPIOD, GPIO_Pin_0);  /* TP_SDA */
    GPIO_OD_Init(GPIOD, GPIO_Pin_2);  /* TP_SCL */
    GPIO_OD_Init(GPIOD, GPIO_Pin_3);  /* TP_INT */
    GPIO_OD_Init(GPIOD, GPIO_Pin_4);  /* 屏幕 RST */
    GPIO_OD_Init(GPIOA, GPIO_Pin_2);  /* TP_RST */
    GPIO_OD_Init(GPIOC, GPIO_Pin_0);  /* 3.3V 触摸 INT 输出，低有效 */
}

/*********************************************************************
 * @fn      I2C_Slave_Init
 *
 * @brief   初始化 I2C1 从机（PC2=SCL, PC1=SDA）
 *
 * @param   speed  总线速率（从机模式主要用于滤波配置）
 *          addr7  7 位从机地址
 *
 * @return  none
 */
static void I2C_Slave_Init(uint32_t speed, uint8_t addr7)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    I2C_InitTypeDef I2C_InitStructure = {0};
    NVIC_InitTypeDef NVIC_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C1, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_2 | GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    I2C_InitStructure.I2C_ClockSpeed          = speed;
    I2C_InitStructure.I2C_Mode                = I2C_Mode_I2C;
    I2C_InitStructure.I2C_DutyCycle           = I2C_DutyCycle_16_9;
    I2C_InitStructure.I2C_OwnAddress1         = (uint16_t)(addr7 << 1);
    I2C_InitStructure.I2C_Ack                 = I2C_Ack_Enable;
    I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    I2C_Init(I2C1, &I2C_InitStructure);
    I2C_Cmd(I2C1, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel                   = I2C1_EV_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    NVIC_InitStructure.NVIC_IRQChannel                   = I2C1_ER_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority        = 1;
    NVIC_Init(&NVIC_InitStructure);

    I2C_ITConfig(I2C1, I2C_IT_BUF, ENABLE);
    I2C_ITConfig(I2C1, I2C_IT_EVT, ENABLE);
    I2C_ITConfig(I2C1, I2C_IT_ERR, ENABLE);
}

/* ---------- 1.8V 触摸侧软件 I2C 主机（PD2=SCL, PD0=SDA） ---------- */
#define TP_SCL_PORT   GPIOD
#define TP_SCL_PIN    GPIO_Pin_2
#define TP_SDA_PORT   GPIOD
#define TP_SDA_PIN    GPIO_Pin_0
#define TP_INT_PIN    GPIO_Pin_3
#define TP_RST_PORT   GPIOA
#define TP_RST_PIN    GPIO_Pin_2

#define TP_I2C_ADDR   0x3B
#define AXS_EVENT_DOWN 0x00u
#define AXS_EVENT_MOVE 0x08u

#define SDA_LOW()      PIN_LOW(TP_SDA_PORT, TP_SDA_PIN)
#define SDA_RELEASE()  PIN_RELEASE(TP_SDA_PORT, TP_SDA_PIN)
#define SCL_LOW()      PIN_LOW(TP_SCL_PORT, TP_SCL_PIN)
#define SCL_RELEASE()  PIN_RELEASE(TP_SCL_PORT, TP_SCL_PIN)
#define SDA_READ()     ((TP_SDA_PORT->INDR & TP_SDA_PIN) != 0)
#define SCL_READ()     ((TP_SCL_PORT->INDR & TP_SCL_PIN) != 0)

static void i2c_delay(void)
{
    Delay_Us(5);
}

static uint8_t i2c_bus_ready(void)
{
    SDA_RELEASE();
    SCL_RELEASE();
    Delay_Us(20);
    return (SDA_READ() && SCL_READ()) ? 1u : 0u;
}

static void i2c_start(void)
{
    SDA_RELEASE();
    SCL_RELEASE();
    i2c_delay();
    SDA_LOW();
    i2c_delay();
    SCL_LOW();
    i2c_delay();
}

static void i2c_stop(void)
{
    SCL_LOW();
    SDA_LOW();
    i2c_delay();
    SCL_RELEASE();
    i2c_delay();
    SDA_RELEASE();
    i2c_delay();
}

static uint8_t i2c_write_byte(uint8_t data)
{
    uint8_t i;
    uint8_t ack;

    for(i = 0; i < 8u; i++)
    {
        if(data & 0x80u)
            SDA_RELEASE();
        else
            SDA_LOW();
        data <<= 1;
        i2c_delay();
        SCL_RELEASE();
        i2c_delay();
        SCL_LOW();
        i2c_delay();
    }

    SDA_RELEASE();
    i2c_delay();
    SCL_RELEASE();
    i2c_delay();

    ack = SDA_READ() ? 0u : 1u;   /* 1 = ACK */

    SCL_LOW();
    i2c_delay();

    return ack;
}

static uint8_t i2c_read_byte(uint8_t ack)
{
    uint8_t i;
    uint8_t data = 0;

    SDA_RELEASE();

    for(i = 0; i < 8u; i++)
    {
        data <<= 1;
        SCL_RELEASE();
        i2c_delay();
        if(SDA_READ())
            data |= 1u;
        SCL_LOW();
        i2c_delay();
    }

    if(ack)
        SDA_LOW();
    else
        SDA_RELEASE();

    i2c_delay();
    SCL_RELEASE();
    i2c_delay();
    SCL_LOW();
    i2c_delay();
    SDA_RELEASE();

    return data;
}

static void TP_Reset_Pulse(void)
{
    PIN_LOW(TP_RST_PORT, TP_RST_PIN);
    Delay_Ms(10);
    PIN_RELEASE(TP_RST_PORT, TP_RST_PIN);
    Delay_Ms(50);
}

static void Screen_Reset_Pulse(void)
{
    /* AXS15260 面板复位时序：post_reset 10ms, reset_low 10ms, after_reset 50ms */
    PIN_RELEASE(GPIOD, GPIO_Pin_4);
    Delay_Ms(10);
    PIN_LOW(GPIOD, GPIO_Pin_4);
    Delay_Ms(10);
    PIN_RELEASE(GPIOD, GPIO_Pin_4);
    Delay_Ms(50);

    s_i2c_regs[REG_SCR_RST_LEVEL] = (GPIOD->INDR & GPIO_Pin_4) ? 1u : 0u;
    s_i2c_regs[REG_SCR_RST_CNT]++;
}

static void tp_set_result(uint8_t present, uint8_t err, uint8_t acks)
{
    s_i2c_regs[REG_TP_PRESENT] = present;
    s_i2c_regs[REG_TP_ERR] = err;
    s_i2c_regs[REG_TP_ACKS] = acks;
    if(!present || err)
    {
        s_i2c_regs[REG_TP_POINTS] = 0;
        s_i2c_regs[REG_MT_POINTS] = 0;
        s_i2c_regs[REG_MT_VALID] = 0;
    }
    s_i2c_regs[REG_STATUS] = 0x01u |
                             (present ? 0x02u : 0u) |
                             (err ? 0x04u : 0u);
    s_i2c_regs[REG_TP_PROBE_CNT]++;
}

/* 有任意一点触摸时把 PC0 拉低给 SoC；无触摸时释放。
 * MT_VALID 已在 tp_probe 里按点数/事件/坐标过滤，这里再做 2 次采样防抖。 */
static void tp_update_int(void)
{
    uint8_t valid = (s_i2c_regs[REG_MT_VALID] != 0);

    static uint8_t s_touch_active = 0;
    static uint8_t s_on_cnt = 0;
    static uint8_t s_off_cnt = 0;

    if(valid)
    {
        if(s_on_cnt < 3u)
            s_on_cnt++;
        s_off_cnt = 0;
    }
    else
    {
        if(s_off_cnt < 3u)
            s_off_cnt++;
        s_on_cnt = 0;
    }

    if(!s_touch_active && s_on_cnt >= 2u)
    {
        s_touch_active = 1;
        PIN_LOW(GPIOC, GPIO_Pin_0);        /* INT 低有效 */
        s_i2c_regs[REG_STATUS] |= 0x08u;   /* bit3: touch active */
        s_i2c_regs[REG_MT_ACTIVE] = 1;
        s_i2c_regs[REG_TP_TOUCH_CNT]++;
    }
    else if(s_touch_active && s_off_cnt >= 2u)
    {
        s_touch_active = 0;
        PIN_RELEASE(GPIOC, GPIO_Pin_0);
        s_i2c_regs[REG_STATUS] &= ~0x08u;
        s_i2c_regs[REG_MT_ACTIVE] = 0;
    }
}

static void tp_probe(void)
{
    static const uint8_t cmd[11] = {
        0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00
    };
    uint8_t buf[32] = {0};
    uint8_t acks = 0;
    uint8_t i;
    uint8_t points, ev;
    uint16_t x, y;

    /* 先记录 INT/RST/屏幕 RST 实际电平，便于判断供电/复位 */
    s_i2c_regs[REG_TP_INT] = (TP_SCL_PORT->INDR & TP_INT_PIN) ? 1u : 0u;
    s_i2c_regs[REG_TP_RST] = (TP_RST_PORT->INDR & TP_RST_PIN) ? 1u : 0u;
    s_i2c_regs[REG_SCR_RST_LEVEL] = (GPIOD->INDR & GPIO_Pin_4) ? 1u : 0u;

    if(!i2c_bus_ready())
    {
        tp_set_result(0, 1, 0);
        return;
    }

    i2c_start();
    if(!i2c_write_byte((uint8_t)(TP_I2C_ADDR << 1)))
    {
        i2c_stop();
        tp_set_result(0, 2, acks);
        return;
    }
    acks |= 0x01u;

    for(i = 0; i < sizeof(cmd); i++)
    {
        if(!i2c_write_byte(cmd[i]))
        {
            i2c_stop();
            tp_set_result(1, 3, acks);
            return;
        }
    }
    acks |= 0x02u;
    i2c_stop();

    Delay_Us(200);

    if(!i2c_bus_ready())
    {
        tp_set_result(1, 1, acks);
        return;
    }

    i2c_start();
    if(!i2c_write_byte((uint8_t)((TP_I2C_ADDR << 1) | 1u)))
    {
        i2c_stop();
        tp_set_result(1, 4, acks);
        return;
    }
    acks |= 0x04u;

    for(i = 0; i < sizeof(buf); i++)
        buf[i] = i2c_read_byte(i < (sizeof(buf) - 1u));

    i2c_stop();
    acks |= 0x08u;

    points = buf[1];
    s_i2c_regs[REG_TP_POINTS] = points;   /* 原始点数，有效 1~5 */
    s_i2c_regs[REG_TP_ESD] = (buf[0] >> 4) & 0x0Fu;
    s_i2c_regs[REG_MT_POINTS] = (points <= 5u) ? points : 0u;
    s_i2c_regs[REG_MT_VALID] = 0;

    /* AXS15260 多点数据：2 字节头 + 每点 6 字节。
     * 每点前 4 字节：x_h/event、x_l、y_h、y_l；后 2 字节忽略。 */
    for(i = 0; i < 5u; i++)
    {
        uint8_t base = (uint8_t)(2u + i * 6u);

        ev = (buf[base] >> 4) & 0x0Fu;
        x = ((uint16_t)(buf[base] & 0x0Fu) << 8) | buf[base + 1u];
        y = ((uint16_t)(buf[base + 2u] & 0x0Fu) << 8) | buf[base + 3u];

        s_i2c_regs[REG_MT_POINT1 + i * 5u + 0u] = (uint8_t)(x & 0xFFu);
        s_i2c_regs[REG_MT_POINT1 + i * 5u + 1u] = (uint8_t)((x >> 8) & 0xFFu);
        s_i2c_regs[REG_MT_POINT1 + i * 5u + 2u] = (uint8_t)(y & 0xFFu);
        s_i2c_regs[REG_MT_POINT1 + i * 5u + 3u] = (uint8_t)((y >> 8) & 0xFFu);
        s_i2c_regs[REG_MT_POINT1 + i * 5u + 4u] = ev;

        if(i < points && points <= 5u &&
           (ev == AXS_EVENT_DOWN || ev == AXS_EVENT_MOVE) &&
           x < 480u && y < 800u)
        {
            s_i2c_regs[REG_MT_VALID] |= (uint8_t)(1u << i);
        }

        if(i == 0u)
        {
            s_i2c_regs[REG_TP_EVENT] = ev;
            s_i2c_regs[REG_TP_X_L] = (uint8_t)(x & 0xFFu);
            s_i2c_regs[REG_TP_X_H] = (uint8_t)((x >> 8) & 0xFFu);
            s_i2c_regs[REG_TP_Y_L] = (uint8_t)(y & 0xFFu);
            s_i2c_regs[REG_TP_Y_H] = (uint8_t)((y >> 8) & 0xFFu);
        }
    }

    /* 兼容旧单点调试：只保留前 14 字节原始数据 */
    for(i = 0; i < 14u; i++)
        s_i2c_regs[REG_TP_RAW0 + i] = buf[i];

    tp_set_result(1, 0, acks);
}

/*********************************************************************
 * @fn      I2C1_EV_IRQHandler
 *
 * @brief   I2C1 从机事件中断（参考 ch32fun i2c_slave 库的状态机）
 *
 * @return  none
 */
void I2C1_EV_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void I2C1_EV_IRQHandler(void)
{
    uint16_t star1 = I2C1->STAR1;
    uint16_t star2 = I2C1->STAR2;  /* 读 STAR2 清 ADDR */
    (void)star2;

    if(star1 & I2C_STAR1_ADDR)
    {
        s_first_write = 1;
        s_position = s_offset;
    }

    if(star1 & I2C_STAR1_RXNE)
    {
        uint8_t data = (uint8_t)I2C1->DATAR;

        if(s_first_write)
        {
            /* 第一个字节是寄存器地址 */
            s_offset = data & 0x0F;
            s_position = s_offset;
            s_first_write = 0;
        }
        else
        {
            if(s_position == REG_BL_DUTY)
            {
                if(data > 100u)
                    data = 100u;
                s_i2c_regs[REG_BL_DUTY] = data;
                s_position++;
            }
            else if(s_position == REG_CMD)
            {
                if(data & 0x01u)
                    s_probe_request = 1;
                if(data & 0x02u)
                    s_reset_request = 1;
                if(data & 0x04u)
                    s_screen_reset_request = 1;
                s_position++;
            }
            else if(s_position < I2C_REG_COUNT)
            {
                /* 只读寄存器：忽略写入，指针继续走 */
                s_position++;
            }
        }
    }

    if(star1 & I2C_STAR1_TXE)
    {
        if(s_position < I2C_REG_COUNT)
        {
            I2C1->DATAR = s_i2c_regs[s_position];
            s_position++;
        }
        else
        {
            I2C1->DATAR = 0x00;
        }
    }

    if(star1 & I2C_STAR1_STOPF)
    {
        /* 读 STAR1 后写 CTLR1 清 STOPF */
        I2C1->CTLR1 &= ~I2C_CTLR1_STOP;

        /* 把最新亮度写入 PWM */
        PA1_PWM_SetDuty(s_i2c_regs[REG_BL_DUTY]);

        s_first_write = 1;
        s_position = s_offset;
    }

    /* 保险：清除孤立的 BTF，避免中断反复进入 */
    if((star1 & I2C_STAR1_BTF) && !(star1 & (I2C_STAR1_RXNE | I2C_STAR1_TXE)))
    {
        (void)I2C1->DATAR;
    }
}

/*********************************************************************
 * @fn      I2C1_ER_IRQHandler
 *
 * @brief   I2C1 错误中断
 *
 * @return  none
 */
void I2C1_ER_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void I2C1_ER_IRQHandler(void)
{
    uint16_t star1 = I2C1->STAR1;

    if(star1 & (I2C_STAR1_BERR | I2C_STAR1_ARLO |
                I2C_STAR1_AF | I2C_STAR1_OVR))
    {
        /* WCH I2C 错误标志：写 0 清除对应位 */
        I2C1->STAR1 = (uint16_t)~(I2C_STAR1_BERR | I2C_STAR1_ARLO |
                                   I2C_STAR1_AF | I2C_STAR1_OVR);
    }
}

int main(void)
{
    uint32_t tick = 0;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    SystemCoreClockUpdate();
    Delay_Init();

    /* 背光 PWM：PA1 = TIM1_CH2，20 kHz，默认 50% */
    PA1_PWM_Init(PWM_FREQ_HZ, PWM_DUTY_DEFAULT);

    /* 1.8V TP 侧：开漏释放，并由软件 I2C 主机主动探测 */
    TP_Pins_Safe_Init();

    /* 3.3V 侧 I2C1 从机，地址 0x3C */
    I2C_Slave_Init(I2C_SPEED_HZ, I2C_SLAVE_ADDR);

    /* 复位屏幕和触摸，然后先探测一次 */
#if SCREEN_RESET_AT_BOOT
    Screen_Reset_Pulse();
#endif
    TP_Reset_Pulse();
    tp_probe();
    tp_update_int();

    while(1)
    {
        if(s_screen_reset_request)
        {
            /* 主机写 REG_CMD bit2：复位屏幕 */
            s_screen_reset_request = 0;
            Screen_Reset_Pulse();
        }

        if(s_reset_request)
        {
            /* 主机写 REG_CMD bit1：先复位触摸，再探测 */
            s_reset_request = 0;
            s_probe_request = 0;
            tick = 0;
            TP_Reset_Pulse();
            tp_probe();
            tp_update_int();
        }
        else if(s_probe_request || tick >= 1u)
        {
            /* 约 10~15ms 探测一次，保证触摸中断/坐标及时 */
            s_probe_request = 0;
            tick = 0;
            tp_probe();
            tp_update_int();
        }

        Delay_Ms(10);
        tick++;
    }
}
