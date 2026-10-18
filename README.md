# 阀门从节点工程（ESP32-C3 Super Mini）

当前目录 `D:/pump/pump-muti-slave` 只构建 ESP32-C3 阀门从节点，环境为 `valve_slave_c3`。PlatformIO 使用官方 `esp32-c3-devkitm-1` 的通用 C3 定义，按 Super Mini / 4MB Flash / DIO 40MHz 配置，ESP-IDF 5.4 / espressif32@6.10.0。

主节点独立工程：`D:/pump/pump-muti`（经典 ESP32，pump_master）。本工程拥有独立源码、sdkconfig、编译缓存和打包脚本，不使用跨目录源码链接。

## 引脚与 USB

| 连接 | Super Mini 丝印 / GPIO | 说明 |
| --- | --- | --- |
| 常闭阀驱动输入 | 4 / GPIO4 | 高有效，外部下拉；不能直接接阀线圈 |
| 霍尔流量计脉冲 | 5 / GPIO5 | 下降沿计数，逻辑电平 3.3V |
| 本地联锁输入 | 6 / GPIO6 | HIGH 许可，外部下拉；LOW/断线关闭 |
| 维护和下载 | 板载 USB | 原生 USB Serial/JTAG，底层 GPIO18/19 |
| 备用 UART0 | RX20 / TX21 | 当前维护命令使用板载 USB，不走 UART0 |
| 电源 | 5V / GND / 3V3 | 按模块和驱动规格供电，GPIO 不耐受 5V |

[完整引脚图与接线说明](docs/PINOUT.md)。沿用用户提供的 Super Mini 引脚图；4/5/6 指 GPIO 丝印，不是排针序号。避免占用 GPIO8/9 的板载 LED、BOOT/启动配置功能；GPIO4/5/6 同时有 JTAG 复用功能，外部调试器接线须检查冲突。

`include/board_profile.h` 的 `valveConfig().commissioned=false` 保持未启用输出。接线、驱动、联锁、K 系数标定和限值核对后才改 true 并重编译。主节点 Mesh 入网不会自动打开阀门。C3 上电/重启先置阀驱动低。

## 构建与打包

```powershell
Set-Location D:/pump/pump-muti-slave
pio run -e valve_slave_c3
python tools/package_firmware.py
```

产物为 `.pio/build/valve_slave_c3`，打包至 `dist/valve_slave_c3`，附 `dist/manifest.json`。只包含 C3 从机镜像；引导程序偏移 **0x0**，分区表 0x8000，应用 0x20000。不要使用 ESP32 主机的 0x1000 引导地址。

需刷写时先核对实际端口，再执行 `pio run -e valve_slave_c3 -t upload --upload-port COMx`。若不能进入下载，按住 BOOT，复位/重新上电后释放；下载及应用状态端口可能变化。最新 PB-ADV 修复已烧录到 COM6，详见 HANDOVER。

原生 USB 上的启动日志及 `hello` / `local` / `master 0001` / `k 实测值` / `estop` 维护命令已经接入 USB Serial/JTAG 驱动。PlatformIO 串口监视器设 115200，命令带换行；未接电脑时 USB 等待不会阻塞控制任务。COM6 已验证启动日志和 hello/local 回复，其余维护命令需按实际维护需要验证。

## 与主节点配网

1. 给未配网 C3 从机上电；串口日志会打印 `PUMP_SLAVE_XXXX`。该标签来自 Mesh UUID，因从机只开 PB-ADV，手机系统的普通蓝牙列表不会显示这个名称；HELLO 角色为 VALVE_SLAVE。
2. 手机连接 ESP32 Master，在设备页发起“让 Master 扫描附近从机”，选择候选后显式“连接组网”。
3. Master 完成 PB-ADV 配网、Composition 校验、AppKey0/模型绑定、C001 订阅、HELLO 登记，等待 READY。
4. 校准 K 后通过 USB 读取实际 K/版本，必要时同步 Master 的节点登记。首次自动登记不代表后续改 K 会自动覆盖登记。
5. 完成现场启用后才进行阀门/水路验证。从机保留本地联锁、计量到量关闭、无流量超时及 3 秒租约。

从机没有手机业务 GATT/PIN 入口；Mesh 只使用 PB-ADV，PB-GATT 和 GATT Proxy 已关闭。`PUMP_SLAVE_XXXX` 只用于串口和 APP 候选项标识，不是手机普通蓝牙扫描名称。手机 PIN 只用于 Master，不作为 Mesh 密钥。新版 Master 启动流程会自动处理遗留 Node 角色并进入 Provisioner；手机不提供迁移步骤，也不自动擦除应用 NVS。确认主机 `capable=1 phase=IDLE` 后，再由 APP 请求扫描和组网。

ESP-IDF 5.4 / Bluedroid 的当前 Mesh 使用传统 LE 扫描接口。C3 必须保留 `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n`、`CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y`，否则实板可能出现 `0x200b / status=0xc`、PB-ADV 启用失败。该配置同时保存在 sdkconfig.defaults 和 sdkconfig.valve_slave_c3；修改 defaults 不会自动覆盖已有 sdkconfig。

## 测试及维护

```powershell
python tools/run_host_tests.py --compiler <zig.exe或clang++路径>
```

测试编译器不随工程复制；本机可用 `D:/pump/pump-muti/.pio/host-tools/ziglang/zig.exe`，也可安装自己的编译器。工程构建和固件运行无需此工具。两工程独立维护协议副本，协议变化需同步两边并核对 [PROTOCOL.md](docs/PROTOCOL.md)。

本次编译、测试和待验收事项见 [HANDOVER.md](HANDOVER.md)。
