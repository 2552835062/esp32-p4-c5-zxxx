# AXS15260D 显示与触摸组件

当前 6.2 寸面板原生分辨率为 452×1280，主工程采用 RGB888、2 Lane 1000Mbps，USB 显示路径旋转 90° 后对应 Windows 1280×452。ESP-IDF 6.1 使用新的 DPI 色彩格式接口，PHY 参考时钟按芯片版本自动选择。

触摸地址为 0x3B，调用方可通过 `i2c_bus` 复用 PMU 的 I2C1。LCD 与触摸共用 GPIO26，LCD 初始化后触摸必须传 `rst_gpio=GPIO_NUM_NC`，避免屏幕被再次复位。

`axs15260_touch_config_t.isr_service_installed` 默认为 false；如果调用方已安装 GPIO ISR 服务，设为 true，可避免重复安装日志和竞争。该字段不转移 ISR 服务所有权，删除触摸驱动不会卸载共享服务。主工程通过 `board_gpio_isr_ensure()` 统一安装，并传 true。
