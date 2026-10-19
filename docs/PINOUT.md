# ESP32-C3 Super Mini 引脚

下图由用户提供（2026-10-17），USB 接口朝上，按板上 GPIO 丝印接线。

![ESP32-C3 Super Mini 引脚图](images/esp32c3-super-mini.jpg)

| 图中引脚 | 工程用途 |
| --- | --- |
| GPIO4（左侧 4，A4/SCK） | 阀门驱动输入，高有效，外部下拉 |
| GPIO5（右上 5，A5/MISO） | 流量计 3.3V 脉冲，下降沿 |
| GPIO6（右侧 6，MOSI） | 可选联锁输入；当前禁用，无需接线 |
| GPIO20 / GPIO21 | UART0 RX / TX，当前维护入口未使用 |
| USB 接口 | 下载、启动日志、USB 维护命令；底层 GPIO18/19 |
| 5V / 3V3 / GND | 供电和参考地；不是阀门功率驱动输出 |

板上 SPI/I2C 标注是复用功能，本工程没有同时启用图中 SPI/I2C；4/5/6 在本工程按普通 GPIO 使用。GPIO8/9 涉及板载 LED、BOOT/启动配置，未选作驱动信号。GPIO2 也是启动配置相关脚，本工程未使用。阀门和传感器的外部电源、驱动与电平需按实际设备规格连接。

当前三线霍尔流量计接电源、GND 和 GPIO5，不需要 GPIO6 到 3V3 的跳线。`include/board_profile.h` 中 `SlaveInterlockEnabled=false`：不配置或读取 GPIO6，外部联锁检查直接放行；无流量、运行超时、通信租约及到量停阀保护继续生效。USB `local` 会额外输出 `INTERLOCK enabled=0 ready=1 pin=6`，其中 ready 表示软件许可，不表示 GPIO6 实测电平。

以后安装真实安全联锁时，将 `SlaveInterlockEnabled` 改为 `true` 并重新烧录。GPIO6 接 3.3V HIGH 许可，外加约 10kΩ 下拉到 GND，使断线变为 LOW。不要接 5V 到 GPIO6。默认 commissioned=false，仍需通过主机或 APP 显式启用，编译或入网不会打开阀门。
