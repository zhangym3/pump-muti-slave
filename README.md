# 阀门从节点工程（ESP32-C3 Super Mini）

2026-10-25 基础候选：协议 revision 6 / RequiredFeatures `0x15FF`，配套主机最多五路同时定量。终态保存成功才发送结果；保存失败锁定新任务，不允许新 CLOSE/PREPARE 覆盖未保存计量。本轮离线测试和构建通过，未连接实机或烧录。交付状态见主机工程 `docs/BASIC_FIVE_OFFLINE_ACCEPTANCE_20261025.md`，历史 dist/台架记录不代表本轮验收。

2026-10-22：新增 USB 维护命令 `mesh-local-reset CONFIRM`，用于主机删除未收到从机退网确认、从机仍保存旧 Mesh 凭据而无法再次被扫描的恢复场景。仅在阀门空闲、存储健康、节点已配网时接受；成功后立即关阀，清除 Mesh 凭据和启用状态并重启进入 PB-ADV。任务记录、K 系数和主机地址保留；更换主机后需用 `master ADDR` 更新主机单播地址。普通复位/长按 Reset 不清 Mesh。该命令需先刷入包含本功能的固件，尚未实机验证。

当前目录 `D:/pump-muti/pump-muti-slave` 只构建 ESP32-C3 阀门从节点，唯一构建环境为 `valve_slave_c3`，独立配网诊断环境已移除。PlatformIO 使用官方 `esp32-c3-devkitm-1` 的通用 C3 定义，按 Super Mini / 4MB Flash / DIO 40MHz 配置，ESP-IDF 5.4 / espressif32@6.10.0。

主节点独立工程：`D:/pump-muti/pump-muti`（ESP32-S3 N16R8，pump_master，PWM GPIO4）。本工程拥有独立源码、sdkconfig、编译缓存和打包脚本，不使用跨目录源码链接。

2026-10-21：配套 APP 1.2.2 可在设备页单台删除从机。此版从机收到 Mesh Node Reset 后，在应用任务中关阀、保存 `commissioned=false` 并重启恢复 PB-ADV 待组网；不在蓝牙回调中写 NVS 或阻塞。保存失败时持续关阀并重试，不处理旧业务队列。重加后需重新启用配置，K 系数和任务持久数据保留。本次已构建及通过 host tests，未刷写或实机验证。离线删除只能确认主机本地移除，普通复位/长按 Reset 不清 Mesh。

## 引脚与 USB

| 连接 | Super Mini 丝印 / GPIO | 说明 |
| --- | --- | --- |
| 常闭阀驱动输入 | 4 / GPIO4 | 高有效，外部下拉；不能直接接阀线圈 |
| 按住开阀轻触按键 | 3 / GPIO3 与 GND | 常开触点，内部上拉；需维护许可，按住开、松开关 |
| 霍尔流量计脉冲 | 5 / GPIO5 | 下降沿计数，逻辑电平 3.3V |
| 可选本地联锁输入 | 6 / GPIO6 | 当前禁用，不需要 GPIO6 跳线；安装真实联锁后可开启 |
| 维护和下载 | 板载 USB | 原生 USB Serial/JTAG，底层 GPIO18/19 |
| 备用 UART0 | RX20 / TX21 | 当前维护命令使用板载 USB，不走 UART0 |
| 电源 | 5V / GND / 3V3 | 按模块和驱动规格供电，GPIO 不耐受 5V |

[完整引脚图与接线说明](docs/PINOUT.md)。沿用用户提供的 Super Mini 引脚图；4/5/6 指 GPIO 丝印，不是排针序号。避免占用 GPIO8/9 的板载 LED、BOOT/启动配置功能；GPIO4/5/6 同时有 JTAG 复用功能，外部调试器接线须检查冲突。

`include/board_profile.h` 的 `SlaveInterlockEnabled=false` 适用于当前没有外部联锁的三线霍尔流量计安装，GPIO6 无需连接。以后安装真实联锁时改为 `true` 并重新烧录，按引脚文档提供 3.3V 许可及外部下拉。USB `local` 输出 `INTERLOCK enabled=0 ready=1 pin=6` 表示外部联锁未启用、软件许可放行。

首次默认未启用，启用状态从 NVS 恢复。接线、驱动、K 系数标定和限值核对后，通过主机或 APP 显式“启用”并保存状态，无需修改 `commissioned` 默认值。主节点 Mesh 入网不会自动打开阀门。C3 上电/重启先置阀驱动低。

执行定量或标定任务时，从机收到 START 后先将 GPIO4 置高，给常闭电磁阀驱动开阀信号，再回复 START_OK。配套新版主机收到有效开阀及租约确认，并为每路预留至少 300ms 后才启动水泵；需更新主机固件，本次从机报文和程序兼容。当前无阀位反馈，需按实阀核对动作时间。

## 本地按住开阀

GPIO3 与 GND 接四脚轻触开关的两组常开触点，先用万用表确认所选两脚未按断开、按下导通。GND 可与霍尔流量计通过分线端子共用。默认按住也不开阀；先在主机 USB 执行 `maintenance-enter ADDRhex CONFIRM` 并取得本从机本次启动的 60 秒许可，再释放并按住 30ms 开阀。松开在下一控制周期（目标 5ms）关阀；上电按住不会开。许可到期立即撤销并关阀，持续按住和迟到重复许可不能重开。结束由主机 `maintenance-exit CONFIRM` 确认全员关闭并退役，许可过期不自动解锁主机泵。

须已显式启用、存储及传输健康，且无远程任务或尾流结算占用。只开本地阀，不启动主机水泵。保留首次 5 秒无流、1.5 秒断流、最长 60 秒和可选联锁保护；因保护停阀后，持续按住不会重开。关阀后需等待尾流结算完成再重新按下。远程任务忙时按键被拒绝，松开不会关闭该远程任务。USB `local` 输出 `BUTTON`（引脚、电平、局部忙、局部阀、脉冲、原因、总输出）；旧远程 `LOCAL` 结果不表示按键状态。本地操作不覆写远程任务结果，不额外写 NVS。详见 [接线](docs/PINOUT.md)。

## 构建与打包

```powershell
Set-Location D:/pump-muti/pump-muti-slave
pio run -e valve_slave_c3
python tools/package_firmware.py
```

产物为 `.pio/build/valve_slave_c3`，打包至 `dist/valve_slave_c3`，附 `dist/manifest.json`。只包含 C3 从机镜像；引导程序偏移 **0x0**，分区表 0x8000，应用 0x20000。不要使用 ESP32 主机的 0x1000 引导地址。

需刷写时先核对实际端口，再执行 `pio run -e valve_slave_c3 -t upload --upload-port COMx`。若不能进入下载，按住 BOOT，复位/重新上电后释放；下载及应用状态端口可能变化。最新 PB-ADV 修复已烧录到 COM6，详见 HANDOVER。

原生 USB 上的启动日志及 `hello` / `local` / `master 0001` / `k 实测值` / `estop` / `mesh-local-reset CONFIRM` 维护命令已经接入 USB Serial/JTAG 驱动。PlatformIO 串口监视器设 115200，命令带换行；未接电脑时 USB 等待不会阻塞控制任务。清 Mesh 是破坏性网络操作，只在确认节点空闲并需要重新配网时使用；COM6 已验证启动日志和 hello/local 回复，新命令尚未实机验证。

## 与主节点配网

1. 给未配网 C3 从机上电；串口日志会打印 `PUMP_SLAVE_XXXX`。该标签来自 Mesh UUID，因从机只开 PB-ADV，手机系统的普通蓝牙列表不会显示这个名称；HELLO 角色为 VALVE_SLAVE。
2. 手机连接 ESP32 Master，在设备页发起“让 Master 扫描附近从机”，选择候选后显式“连接组网”。
3. Master 完成 PB-ADV 配网、Composition 校验、AppKey0/模型绑定、C001 订阅、HELLO 登记，等待 READY。
4. 校准 K 后通过 USB 读取实际 K/版本，必要时同步 Master 的节点登记。首次自动登记不代表后续改 K 会自动覆盖登记。
5. 完成现场启用后才进行阀门/水路验证。从机保留计量到量关闭、首次无流 5 秒、断流 1.5 秒、最大运行 240 秒及 12 秒租约保护；主机任一路租约证明满 8 秒即整批停泵。外部联锁仅在 `SlaveInterlockEnabled=true` 时检查 GPIO6。

从机没有手机业务 GATT/PIN 入口；Mesh 只使用 PB-ADV，PB-GATT 和 GATT Proxy 已关闭。`PUMP_SLAVE_XXXX` 只用于串口和 APP 候选项标识，不是手机普通蓝牙扫描名称。手机 PIN 只用于 Master，不作为 Mesh 密钥。新版 Master 启动流程会自动处理遗留 Node 角色并进入 Provisioner；手机不提供迁移步骤，也不自动擦除应用 NVS。确认主机 `capable=1 phase=IDLE` 后，再由 APP 请求扫描和组网。

ESP-IDF 5.4 / Bluedroid 的当前 Mesh 使用传统 LE 扫描接口。C3 必须保留 `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n`、`CONFIG_BT_BLE_42_FEATURES_SUPPORTED=y`，否则实板可能出现 `0x200b / status=0xc`、PB-ADV 启用失败。该配置同时保存在 sdkconfig.defaults 和 sdkconfig.valve_slave_c3；修改 defaults 不会自动覆盖已有 sdkconfig。

## 测试及维护

### 霍尔脉冲日志

从机通过板载 USB 输出 `HALL_CONFIG`、`HALL` 和 `HALL_JOB`，不增加 Mesh 消息。启动时记录输入配置与计数快照；开阀时输出 `event=start`，供水和关阀结算期间每秒输出 `event=sample`，结算结束输出 `event=final`。空闲时若累计计数变化，每秒最多输出一次 `event=idle_pulse`。USB 维护命令 `hall` 可随时查询，`local` 也附带同样的快照，两者不会启动泵阀。

- `HALL`：`t` 为启动后毫秒，`pin` / `level` 为 GPIO 编号及读取瞬间的电平；`raw` 为本次上电以来通过 100 µs 间隔过滤的下降沿累计数（并非未过滤的电气边沿数）；`delta` 为距上次快照的增量，`dt_ms` 为间隔，`last_ms` 为最后一次有效脉冲的启动后时间。查询也更新快照基准。
- `HALL_JOB`：相同 `t` 关联任务号、阶段、结束原因、阀门命令和当前任务的 `job_pulses` / `job_ml`。任务计数由控制器更新，和硬件快照可能相差一个控制周期；空闲时保留最近任务结果。

日志在普通任务中格式化后以零等待方式进入 USB 队列，中断只计数；队列满或 USB 未连接时诊断日志可能丢失，累计计数继续更新。`delta>0` 表示接收到有效下降沿；`level` 只是瞬时值，单次高/低电平不能证明没有脉冲。没有出水的空闲采样不能用来判断传感器故障。

```powershell
python tools/run_host_tests.py --compiler <zig.exe或clang++路径>
```

测试编译器不随工程复制；本机可用 `D:/pump/pump-muti/.pio/host-tools/ziglang/zig.exe`，也可安装自己的编译器。工程构建和固件运行无需此工具。两工程独立维护协议副本，协议变化需同步两边并核对 [PROTOCOL.md](docs/PROTOCOL.md)。

本次编译、测试和待验收事项见 [HANDOVER.md](HANDOVER.md)。
