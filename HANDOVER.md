# ESP32-C3 Super Mini 从节点交接

## 2026-10-25 当前软件候选（实机未验收）

从机 `60d8ad44` 配套主机 `182ab692`，协议 V6、最多五路同时定量。终态保存成功后才允许 RESULT，保存失败锁定新任务且保留原计量；GPIO3 按钮默认禁开，须获主机 boot 绑定、最长 60 秒的维护许可，许可到期或退役关闭输出，主泵保持禁止。主从及固定 SDK 离线测试、固件构建通过，APP 137 项 JVM 测试通过。当前无实机测试条件，通信/出水/时序与完整 Application/NVS/RTOS 故障注入未验收。详见 [主机实施记录](../pump-muti/docs/BASIC_FIVE_OFFLINE_ACCEPTANCE_20261025.md)。下文均为历史时点记录。

## 2026-10-23 深夜：主从配网实机诊断

COM6 已烧录普通固件 `f0f1957d`，COM9 主机为 `56e9a678`，各自 dist 已更新，未清 NVS。从机普通与诊断环境改为 SDK 要求的 DATA_DEVICE 扫描去重；诊断中 PB-ADV 握手成功并获地址 `0003`，普通固件配置重试已完成 Composition 与 AppKey Add，但 Model App Bind 响应超时。随后后台 HELLO 曾到达，主机已登记 slot=1、从机 epoch 同步为 34；最后一次有限重试仍 Composition 超时，结束快照 online=1（此前为 0）、管理状态 CONFIG_TIMEOUT，尚未 READY/启用，不代表链路稳定。绑定请求可能已执行而响应丢失，不能把超时等同于从机未执行。当前从机 mesh=1、commissioned=0、valve_command=0。外设仍连接，两板 USB 供电，相距约 50 厘米；仅 USB 隔离对照暂无法进行，未确定硬件根因。此处实机进展优先于下文“未刷写”历史描述。完整证据见 [主机配网诊断](../pump-muti/docs/PAIRING_DIAGNOSIS_20261023.md)。

## 2026-10-23：按住开阀与 C3 主机

从机增加 GPIO3-GND 常开轻触按键，按住开阀、松开关阀；开机按住不启动，30ms 开启消抖，保留 5 秒首次无流、1.5 秒断流、60 秒上限、联锁及远程互斥。只开本地阀，不启动主泵。此前远程结果与 NVS 不被本地按键改写。主机同步迁移 ESP32-C3 Super Mini；接线见 docs/PINOUT.md，主机说明见 ../pump-muti/docs/ESP32_C3_MIGRATION.md。本轮未刷写；下文已刷写为历史固件记录。本机 `valve_slave_c3` 最终构建和 host tests 通过，静态 RAM 38,048B、程序 980,144B；已更新 dist 镜像/清单并核对 ESP32-C3/4MB 镜像头、分区及 SHA256。

## 2026-10-22：已配对从机本地清 Mesh 恢复

新增 USB 维护命令 `mesh-local-reset CONFIRM`。它要求节点已配网、阀门空闲且存储健康；调用 ESP-IDF 本地 Node Reset 后立即进入现有的关阀、清 `commissioned`、重启流程，重新开启 PB-ADV。只清 Mesh 网络凭据，不擦除任务记录、K 系数、从机主地址或其他 NVS。适用于主机已删除节点但从机没收到远程 Node Reset 的情形。先刷入包含该命令的从机固件，再连接 USB 执行；尚未进行实机测试。

## 2026-10-21：配套单台删除（未刷写）

已处理 Mesh Node Reset 事件：应用任务立即关阀并禁止旧业务执行，保存未启用状态后重启，重新进入 PB-ADV 待组网。主机为 ESP32-S3 N16R8，配套 APP 1.2.2。固件构建及 host tests 通过；本轮未刷写、未验证真实远程退网及重加。下文 PB-ADV 已刷写记录对应此前固件。

## 最新补充：PB-ADV 实板修复

现场 C3 从机已修复默认 Bluedroid BLE 5.0 功能与当前 Mesh 传统扫描接口不匹配的问题：关闭 CONFIG_BT_BLE_50_FEATURES_SUPPORTED，启用 CONFIG_BT_BLE_42_FEATURES_SUPPORTED，同步默认与已生成配置，移除无效的延迟重试。已编译、烧录并验证无延迟 `node provisioning enabled (PB-ADV)`，USB hello/local 正常，阀门输出为 0。主机启动及再次重启后均可扫描到从机。固件包已更新；构建和串口日志保留在本地 `.pio` 目录，不纳入版本库。手机只连主机，通过“添加设备”查看从机标签；普通手机蓝牙列表不会出现此名称。尚未执行点击组网和水路验收。下文为此前拆分记录，“未烧录”“USB 待验证”不代表本轮最新结果。

2026-10-17，从原双角色工程拆分而来。当前目录 D:/pump/pump-muti-slave 只构建 valve_slave_c3，从机芯片固定 ESP32-C3，4MB Flash，DIO / 40MHz，ESP-IDF 5.4.0。

## 工程入口

- PlatformIO 打开当前目录，选择唯一环境 valve_slave_c3。
- 主节点在 D:/pump/pump-muti，只构建 ESP32 pump_master。
- 本工程是独立源码副本，编译不引用主机目录。主机调度、泵安全和 Provisioner 管理源码不进入本工程；协议修改需要同步两边并通过固定向量测试。
- include/protocol.h、src/protocol.cpp、include/control_request.h、docs/PROTOCOL.md 与主机当前版本完全一致。

## 接线与控制

用户提供的 Super Mini 引脚图在 [docs/PINOUT.md](docs/PINOUT.md)，原图位于 docs/images/esp32c3-super-mini.jpg。

| 用途 | GPIO |
| --- | --- |
| 阀门功率驱动输入 | 4，高有效、外部下拉 |
| 流量脉冲输入 | 5，3.3V 逻辑，下降沿计数 |
| 联锁输入 | 6，3.3V HIGH 许可，外部下拉 |
| 下载、启动日志、维护命令 | 板载 USB Serial/JTAG，底层 GPIO18/19 |
| 备用 UART0 | RX20 / TX21，当前不用作命令入口 |

valveConfig().commissioned=false，须完成实际接线、驱动、联锁、K 标定和限值验证后启用并重新构建。ProfileId=0x56530002，与此前 C3 档案保持一致；不可把经典 ESP32 的 NVS 直接复制到 C3。上电先置输出低，保留本地到量关阀、无流量超时、联锁和租约保护。

从机不开放手机业务 GATT/PIN；手机通过 Master 管理。从机只使用 Mesh PB-ADV 和 Vendor Model，PB-GATT、GATT Proxy 均关闭。`PUMP_SLAVE_XXXX` 是由 UUID 推导的身份标签，不是普通手机蓝牙扫描中的 GAP 名称。Master 完成 AppKey0/模型绑定/C001/HELLO 后登记节点。入网不会自动启用阀门。新从机不要求配置手机六位 PIN。

## 构建、打包和升级

```powershell
Set-Location D:/pump/pump-muti-slave
pio run -e valve_slave_c3
python tools/package_firmware.py
```

产物在 dist/valve_slave_c3，仅包含本角色的 3 个镜像；manifest 记录目标芯片、偏移、镜像 SHA256 和源码配置哈希。引导程序=0x0，分区表=0x8000，应用=0x20000，NVS=0x9000/64KB，正常更新保留 NVS。

确认端口后使用 `pio run -e valve_slave_c3 -t upload --upload-port COMx`；不要使用主机 ESP32 镜像。原生 USB 下载/应用状态端口可能变化，需要时 BOOT+复位进入下载模式。

维护命令走板载 USB：hello、local、master 实际主地址、k 实测系数、estop、`mesh-local-reset CONFIRM`，均带换行。USB 回复等待在独立任务中有界，断开 USB 不影响控制任务继续运行。当前实现编译通过；本地清 Mesh 命令仍待实板验收。

## 验证结果

- 独立编译成功，119.56 秒；RAM=31896 字节，Flash=1017536 字节；应用镜像 1017936 字节。
- 本工程独立运行 24 组控制器 host 测试通过；测试编译器可单独指定，不随源码复制。
- 从机目标配置为 esp32c3、单核、USB Serial/JTAG 控制台、BT_ALARM_MAX_NUM=128，且不启用 Provisioner/CFG_CLI；本轮已执行 PlatformIO clean build，`valve_slave_c3` SUCCESS。构建保留 ESP-IDF Mesh 宏缺省字段和 volatile 增量警告。
- 已核对生成分区、DIO/40MHz/4MB 镜像头及 3 个发布镜像/源码 SHA256。
- 应用 SHA256：9a39ec29820980685011225a3e0bd72038474d6fff920eff8e923437cd0fe4c2。
- 证据：.pio/split-slave-build.log、.pio/split-host-tests.log、.pio/split-validation.json、dist/manifest.json。

本次没有烧录设备，没有清空任何 NVS，也没有发送泵阀输出指令。`LEGACY_NETWORK` 仅作为主机兼容状态保留；新版 Master 启动流程自动处理旧 Node 角色，APP 不提供迁移步骤。仍待验证 C3 USB、单台从机组网、重启恢复、联锁、阀门和流量标定。32 槽位容量不代表已完成 32 台无线/水路验收。

完整操作见 [README.md](README.md)，协议见 [PROTOCOL.md](docs/PROTOCOL.md)。
