/**
 * @file esp_lcd_touch_axs15260.h
 * @brief  AXS15260 触摸屏驱动头文件
 * 
 * @note I2C 从机地址: 0x3B
 * @note 最大支持 5 点触控
 * @note 分辨率: 452x1280 (与 LCD 一致)
 * 
 * SPDX-FileCopyrightText: 2025
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//  配置宏定义
// ============================================================================

//  I2C 配置
#define AXS15260_TOUCH_I2C_ADDR         0x3B    //  I2C 从机地址
#define AXS15260_TOUCH_I2C_FREQ_HZ      100000  //  I2C 时钟频率 (100kHz)

//  触摸配置
#define AXS15260_TOUCH_MAX_POINTS       5       //  最大触摸点数
#define AXS15260_TOUCH_POINT_SIZE       6       //  单个触摸点数据长度
#define AXS15260_TOUCH_BUF_SIZE         8       //  触摸数据缓冲区大小

//  默认分辨率 (与 LCD 一致)
#define AXS15260_TOUCH_H_RES            452     //  水平分辨率
#define AXS15260_TOUCH_V_RES            1280    //  垂直分辨率

// ============================================================================
//  触摸事件类型
// ============================================================================

typedef enum {
    AXS15260_TOUCH_EVT_DOWN     = 0,    //  按下
    AXS15260_TOUCH_EVT_UP       = 1,    //  抬起
    AXS15260_TOUCH_EVT_CONTACT  = 2,    //  持续接触
} axs15260_touch_event_t;

// ============================================================================
//  数据结构定义
// ============================================================================

/**
 * @brief  单个触摸点数据
 */
typedef struct {
    uint16_t x;         //  X 坐标
    uint16_t y;         //  Y 坐标
    uint8_t id;         //  触摸点 ID (0-4)
    uint8_t event;      //  事件类型
    uint8_t weight;     //  压力值
    uint8_t area;       //  触摸面积
} axs15260_touch_point_t;

/**
 * @brief  触摸数据
 */
typedef struct {
    uint8_t point_num;                                      //  触摸点数量
    uint8_t gesture_id;                                     //  手势 ID
    axs15260_touch_point_t points[AXS15260_TOUCH_MAX_POINTS]; //  触摸点数组
} axs15260_touch_data_t;

/**
 * @brief  触摸屏配置
 */
typedef struct {
    i2c_master_bus_handle_t i2c_bus;  //  复用已有 I2C 总线 (非空时忽略 sda/scl/port，删除驱动时不销毁总线)
    gpio_num_t i2c_sda;         //  I2C SDA 引脚
    gpio_num_t i2c_scl;         //  I2C SCL 引脚
    gpio_num_t rst_gpio;        //  复位引脚 (-1 表示不使用)
    gpio_num_t int_gpio;        //  中断引脚 (-1 表示不使用)
    i2c_port_num_t i2c_port;    //  I2C 端口号
    uint32_t i2c_freq_hz;       //  I2C 时钟频率 (0 使用默认值)
    uint16_t x_max;             //  X 最大值 (0 使用默认值)
    uint16_t y_max;             //  Y 最大值 (0 使用默认值)
    struct {
        uint8_t swap_xy: 1;     //  交换 X/Y 坐标
        uint8_t mirror_x: 1;    //  镜像 X 坐标
        uint8_t mirror_y: 1;    //  镜像 Y 坐标
    } flags;
    bool isr_service_installed; // 调用方已安装共享 GPIO ISR 服务时跳过重复安装
} axs15260_touch_config_t;

/**
 * @brief  触摸屏句柄 (不透明指针)
 */
typedef struct axs15260_touch_dev *axs15260_touch_handle_t;

/**
 * @brief  触摸中断回调函数类型
 */
typedef void (*axs15260_touch_cb_t)(axs15260_touch_handle_t handle, void *user_data);

// ============================================================================
//  API 函数
// ============================================================================

/**
 * @brief  创建 AXS15260 触摸屏驱动
 * 
 * @param[in] config 触摸屏配置
 * @param[out] handle 返回的触摸屏句柄
 * @return
 *      - ESP_OK:  成功
 *      - ESP_ERR_INVALID_ARG:  参数无效
 *      - ESP_ERR_NO_MEM:  内存不足
 */
esp_err_t axs15260_touch_new(const axs15260_touch_config_t *config, 
                              axs15260_touch_handle_t *handle);

/**
 * @brief  删除 AXS15260 触摸屏驱动
 * 
 * @param[in] handle 触摸屏句柄
 * @return
 *      - ESP_OK:  成功
 *      - ESP_ERR_INVALID_ARG:  参数无效
 */
esp_err_t axs15260_touch_del(axs15260_touch_handle_t handle);

/**
 * @brief  复位触摸屏
 * 
 * @param[in] handle 触摸屏句柄
 * @return
 *      - ESP_OK:  成功
 *      - ESP_ERR_INVALID_ARG:  参数无效
 */
esp_err_t axs15260_touch_reset(axs15260_touch_handle_t handle);

/**
 * @brief  读取触摸数据
 * 
 * @param[in] handle 触摸屏句柄
 * @param[out] data 触摸数据
 * @return
 *      - ESP_OK:  成功
 *      - ESP_ERR_INVALID_ARG:  参数无效
 *      - ESP_ERR_TIMEOUT:  超时
 */
esp_err_t axs15260_touch_read(axs15260_touch_handle_t handle, 
                               axs15260_touch_data_t *data);

/**
 * @brief  读取固件版本
 * 
 * @param[in] handle 触摸屏句柄
 * @param[out] version 固件版本
 * @return
 *      - ESP_OK:  成功
 *      - ESP_ERR_INVALID_ARG:  参数无效
 */
esp_err_t axs15260_touch_get_version(axs15260_touch_handle_t handle, 
                                      uint16_t *version);

/**
 * @brief  注册触摸中断回调
 * 
 * @param[in] handle 触摸屏句柄
 * @param[in] callback 回调函数
 * @param[in] user_data 用户数据
 * @return
 *      - ESP_OK:  成功
 *      - ESP_ERR_INVALID_ARG:  参数无效
 */
esp_err_t axs15260_touch_register_cb(axs15260_touch_handle_t handle,
                                      axs15260_touch_cb_t callback,
                                      void *user_data);

/**
 * @brief  设置坐标变换
 * 
 * @param[in] handle 触摸屏句柄
 * @param[in] swap_xy 交换 X/Y 坐标
 * @param[in] mirror_x 镜像 X 坐标
 * @param[in] mirror_y 镜像 Y 坐标
 * @return
 *      - ESP_OK:  成功
 *      - ESP_ERR_INVALID_ARG:  参数无效
 */
esp_err_t axs15260_touch_set_swap_xy(axs15260_touch_handle_t handle,
                                      bool swap_xy, bool mirror_x, bool mirror_y);

/**
 * @brief  检查是否有触摸事件 (通过中断引脚)
 * 
 * @param[in] handle 触摸屏句柄
 * @return true: 有触摸, false: 无触摸
 */
bool axs15260_touch_is_pressed(axs15260_touch_handle_t handle);

#ifdef __cplusplus
}
#endif
