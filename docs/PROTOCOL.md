# 协议 V3

Bluetooth Mesh Vendor CID `0x02E5`、Model ID `0x0001`，群组 `0xC001`。固定使用 NetKey index 0 / AppKey index 0。所有多字节字段显式小端，禁止直接发送 C++ 内存结构。

`JobKey` = `epoch:u32 + id:u32`，两者都非零。epoch 在主节点每次启动时持久化递增；id 在请求正式接受前递增并保存。节点地址来自 Mesh 上下文，不在业务载荷内重复声明。

| Op | 方向 | 载荷，按顺序 | 字节 |
| --- | --- | --- | --- |
| C0 PREPARE | 主→从，单播 | key, ml:u32, targetPulses:u32, k:u32, version:u32, maxMs:u32, speedFactorPermille:u16, leaseMs:u32, flags:u8 | 35 |
| C1 START | 主→从，单播 | key | 8 |
| C2 LEASE | 主→从，单播 | key, sequence:u32 | 12 |
| C3 CLOSE | 主→从，单播 | key, reason:u8 | 9 |
| C4 RESULT_QUERY | 主→从，单播 | key，零值表示查询最近已保存结果 | 8 |
| C5 RESULT | 从→主，单播 | key, pulses:u32, closePulses:u32, k:u32, revision:u32, phase:u8, reason:u8, flags:u8 | 27 |
| C6 保留 | 不发送 | 兼容旧 opcode 编号；SUMMARY 已删除 | - |
| C7 RESULT_ACK | 主→从，单播 | key, revision:u32 | 12 |
| C8 REQUEST | 入口→主 | action:u8, epoch:u32, client:u64, sequence:u32, mask:u32, ml:u32, maxMs:u32, stopKey | 37 |
| C8 REQUEST 单独设置 | 入口→主 | action=1:u8, epoch:u32, client:u64, sequence:u32, mask:u32, ml=0:u32, maxMs:u32, amounts:32 bytes | 61 |
| C9 RESPONSE | 主→入口 | client:u64, sequence:u32, reply:u8, key, schedulePhase:u8, selected:u32, complete:u32, failed:u32, unknown:u32, totalMl:u32, measured:u32, reason:u8 | 47 |
| CA HELLO | 从→主 | k:u32, version:u32, commissioned:u8, buildId:u32 | 13 |
| CB HELLO_REPLY | 主→从 | epoch:u32, pumpFault:u8, schedulePhase:u8 | 6 |
| CC COMMISSION | 主→从，单播 | request:u32 | 4 |
| CD COMMISSION_REPLY | 从→主，单播 | request:u32, reply:u8, commissioned:u8, k:u32, version:u32 | 14 |
| CE LEASE_ACK | 从→主，单播 | key, sequence:u32, reply:u8 | 13 |
| CF COMMAND_ACK | 从→主，单播 | key, command:u8, reply:u8 | 10 |
| D0 CAPABILITIES_GET | 主→从，单播 | nonce:u32 | 4 |
| D1 CAPABILITIES | 从→主，单播 | nonce:u32, revision:u16, features:u16, buildId:u32 | 12 |

业务载荷之外还有 opcode、MIC 和 Mesh 层开销；业务载荷超过 8 字节时由当前传输层按分段报文处理。配置提供 16 个 RX 分段上下文、8 个 TX 上下文；它们不构成 32 节点吞吐保证。C6 不再发送任何周期流量摘要。

从机阶段枚举顺序由 `include/protocol.h` 定义：`Idle, Prepared, Opening, Running, Settling, Done, Fault`。RESULT 的 phase 只能是 `Done` 或 `Fault`；bit0=阀门输出命令、bit1=已满足静默窗口、bit2=结算截断。主机只接受阀门输出已关闭且未知 flag 未置位的终态。revision 随状态变化递增；普通脉冲计数变化不增加 revision。主机按 `JobKey + revision` 去重并回传 RESULT_ACK。

从节点每次启动在 NVS 为 revision 预留新的 16 个计数值，每项任务最多使用 5 次状态变化。恢复的终态也改用本次启动的 revision；因此重启后的故障状态不会被重启前更高阶段的旧报文覆盖。计数耗尽则保持输出关闭。空闲时修改 K 不修改已完成任务保存的 K，历史毫升数仍按执行当时的系数解释。

调度枚举：`Idle, Preparing, Opening, Supplying, Draining, Between, Complete, Failed`。complete 掩码表示终态已收到，failed 表示不能认定成功，unknown 表示未取得可信最终结果。总量只累加匹配任务的数据；measured 掩码描述数据覆盖，不能将未覆盖节点解释为已出水 0mL。

主节点本地泵阶段编号保留为 `Off=0`、`Running=2`、`Fault=4`，其中 Running 只表示泵输出命令有效，不是实际运行反馈。`pumpFault` 编号为 `NONE=0`、`BOOT_LOCK=1`、`NOT_COMMISSIONED=2`、`AUTHORIZATION=7`、`RUNTIME=8`、`TRANSPORT=11`；其余旧编号保留不用，NOT_COMMISSIONED 当前也不主动产生。该调整未改变 Mesh 报文布局。主节点不采集缺水、压力或运行反馈。

## 重试与关闭

能力缓存有效时不逐批探测；节点离线、build ID 变化或主机重启后需重新单播 CAPABILITIES_GET。能力探测必须在 PREPARE/START 前完成。从机每 10 秒发送一次 HELLO 在线心跳，主机回复 HELLO_REPLY；离线恢复或主机有未决结果时可触发按需 RESULT_QUERY。HELLO 不包含流量遥测。

PREPARE / START 按未确认节点轮询，准备阶段和打开阶段上限分别为 10s / 4s。生产 `MaxConcurrent=1`；增加并发前必须重新验证阶段时限和无线负载。开始供水前，主机等待每个已启动节点的新鲜 LEASE_ACK，之后按单播地址续租；从机只接受同任务的更新序号，重复序号可确认但不延长租期，过期租约不能被迟到报文复活。PREPARE 中的 `leaseMs` 默认 12000 ms，单调时钟到期或达到 `maxMs` 都会使从机本地关阀。

停止先本地断泵，再向本批未完成节点单播 CLOSE；取消期间每 700ms 重发关闭，未取得终态则每 300ms 补查一个节点，最多等待 10s。终态缺失标记 UNKNOWN，不自动重新出水。

从节点终态 RESULT 最多尝试 6 次，按地址错开发送；未确认的已保存终态可在后续 HELLO/恢复查询时重新上报。正常运行期间不发送 SUMMARY 或 STATUS。CLOSE、LEASE、RESULT、ACK 和停止请求使用高优先级关键队列；关键队列溢出或分段发送异常会触发本地安全停止。

已配网节点重启直接恢复 Mesh 配置，不重新启用待配网过程。固定主地址检查、AppKey 绑定和注册表共同限制业务来源。全部节点均为受信任设备；本协议不防御已掌握网络密钥且运行恶意固件的内部节点伪造主地址。

## 客户端请求身份

启动 action=1，停止 action=2，查询 action=3。每个客户端使用持久化随机非零 64 位身份与递增非零 32 位序号。最大保存 16 个客户端高水位记录，容量满时拒绝新增身份，不清除旧记录来腾位置。达到 UINT32_MAX 时需维护迁移身份，不允许回绕。

相同身份 / 序号 / 参数返回原任务；相同序号不同参数返回 CONFLICT；更小序号返回 STALE。入口节点地址不参与去重，所以换手机入口不会产生新任务。查询使用被查询启动请求的原序号；停止包含明确 stopKey，旧任务停止报文不能取消新任务。

主节点在持久化请求与任务号之后才发 PREPARE。从节点在持久化任务栅栏之后才确认准备。主节点重启将上次未完成任务标记失败 / 未知，仍不重发 START；收到从节点终态后补充最近任务的结果。

若从节点中途重启且没有保存完整终态，本次出水量标记未知，不把复位后的零计数当作最终零出水；已保存的完整结果可恢复并重新上报。

## GATT 文本层

### 单独设置水量（2026-10-19）

`HELLO` 新增可选能力 `individual=1`。缺省为不支持，APP 不向旧主机发送单独设置启动命令。

```text
start-each CLIENT_HEX SEQ EPOCH MASK_HEX MAX_MS AMOUNTS_64HEX
```

`AMOUNTS_64HEX` 按槽位 1～32 各占两位十六进制；选中槽位为 `01`～`28`，表示 50～2000 mL，单位 50 mL；未选槽位必须 `00`。例如 mask=80000001、1 号 300mL、32 号 2000mL，计划为 `06` + 30 个 `00` + `28`。无论选多少路，整条命令不超过 144 字节。`MAX_MS` 是从设置页取得的全局单路时限，范围 1～240000ms。

主机将整个计划作为一个任务先持久化，再按编号顺序逐路 PREPARE；每路独立计算目标脉冲，沿用原从机 PREPARE 布局。手机断连不影响已接受任务，重试必须保留完整计划和原身份/序号；同序号任意一路目标改变返回 CONFLICT。统一设置仍使用原 `start` 和 37 字节 REQUEST。新建任务的 APP 水量上限为 2000mL，旧已接受任务/历史记录和真实累计流量不截断。

主机 NVS 格式升级为 2：保留原格式 1 的完整字段布局并增加 16 个客户端的逐路计划。启动可原位读取旧记录，保留 PIN、登记表、客户端高水位、任务及结果，再写入格式 2；禁止擦除 NVS。降级到只支持格式 1 的固件会拒绝读取格式 2，应使用升级前备份安排维护。独立试泵仍共用原客户端记录，无需改变从机执行协议。

UUID、命令格式见 README。维护参数仍只允许 USB 修改；业务 GATT 开启前必须设置六位 PIN。使用安全连接、MITM 和加密属性权限。手机写入一条命令后需等待权威 R 响应，或查询确认。旧 Mesh 角色由 Master 启动流程自动接管。

主节点 `local` 返回 `LOCAL pump_command=N pump_phase=N fault=NAME`，不再返回传感器或运行反馈字段；客户端不能把 `pump_command=0` 解释为已通过传感器确认停泵。

### Master 独立试泵

`pump-test CLIENT_HEX SEQ EPOCH DURATION_MS` 启动 1～10000ms 的本地通水测试；`pump-dry-test CLIENT_HEX SEQ EPOCH DURATION_MS` 是兼容旧客户端的限时测试协议（APP 1.2.0 无此入口），1～3000ms、强制低档，执行中拒绝升档；均不超过板卡 `maxRunMs`。`pump-test-stop CLIENT_HEX START_SEQ EPOCH` 取消原测试；`pump-test-query CLIENT_HEX START_SEQ EPOCH` 查询。仅 Master USB/经过 PIN 验证的加密 GATT 支持，Slave 不转发。无需 Mesh 配网或 USB `pump-reset`，但必须已配置输出、冷却完成、存储正常、无活动定量任务和未知结果。测试仅允许 NONE 或 BOOT_LOCK；其他故障不绕过。若从 BOOT_LOCK 开始，停止和超时均恢复该锁定，正常定量任务依旧需要 USB 复位。

响应为 `PUMP_TEST client=HEX seq=N epoch=N reply=NAME state=UNKNOWN/RUNNING/STOPPED op=START/STOP/QUERY`。APP 匹配身份、序号和启动 epoch；GATT 写成功不等于接受。未确认的查询或停止失败不能把待确认启动标为拒绝。只允许明确 START 拒绝或权威停止状态释放该测试；确认终态后不因迟到响应恢复执行态。

接受记录与定量请求共用持久化客户端高水位；先落盘再输出。存储格式 2 兼容导入原版本 1 的记录，mask=0、ml=0 表示通水测试，mask=0、ml=1 表示快速测试；相同序号不同模式冲突。重复启动不延长时限、不再次输出。停止先到时写入同序号 tombstone，确保尚未处理的启动之后无法运行。主机本地定时关闭，不依赖手机和续租；重启关闭输出且不会恢复旧测试。

Master `LOCAL` 增加 `test_supported=1 test_active=N test_ms=N test_client=HEX test_seq=N pump_ready=N`。运行期间及结束时发布状态；测试不输出流量结果，也不自动打开阀门。`STOPPED` 仅表示该测试不再驱动输出，当前输出还需检查 `pump_command`，并非运行传感器反馈。

### Master PWM 三档调速

`local` 还返回 `PUMP_PWM epoch=N rev=N level=N duty=N hz=N output=N reply=NAME test_manual=1 dry_max_ms=3000 dry_active=N`。APP 只在当前 epoch 的新鲜快照明确声明 test_manual=1 时允许 BOOT_LOCK 下测试和选档；dry_max_ms 声明快速测试上限，dry_active 声明当前快速测试。旧固件未声明这些能力时保留原限制。`level` 为 1/2/3（低/中/高），`duty` 为所选档位占空比百分数，即使停泵也保留所选值；`output=0` 时实际 PWM 为 0%，`output=1` 表示输出该档位 PWM，不是转速传感器反馈。当前默认 50/75/100%，20 kHz 需按驱动规格确认。快速测试成功后强制低档并递增 PWM 修订号。

通过 Master USB 或已完成 PIN 配对的加密 GATT 发送 `pump-speed EPOCH EXPECTED_REV LEVEL`。使用最新快照中的启动 epoch 与全局修订号；成功才递增修订号，不匹配则拒绝。修订号每次启动从 0 开始，达到 uint32 上限后拒绝修改，不回绕。档位只保存于内存，重启恢复低档和关闭输出。Slave 不转发此命令。

调速要求硬件已启用、仅 NONE 或 BOOT_LOCK、存储健康且没有活动定量任务或未知定量结果。空闲选档不启泵，通水测试中可调速但不改变原截止时间或授权期限；快速测试中仅允许低档，定量任务运行中不允许修改。PWM 驱动失败会关闭输出并锁定 TRANSPORT 故障。APP 等待匹配 epoch、递增修订号的设备确认，GATT 写入成功不等于设置成功；超时只刷新或重连，不自动重发。档位待确认时禁止新任务和新测试，PWM 状态过期时禁止新测试，原测试停止入口仍可用。

`R client=... seq=... reply=... job=epoch:id phase=N mask=... done=... failed=... unknown=...`

`V job=epoch:id total_ml=N measured=...`

未确认转发只返回 `PENDING`，丢失响应后可重试原请求；读 TX 只能读最新一行。通知按换行拼接，不将单个 ATT 分片当作完整响应。主节点运行期间还发送 FLOW 行；其他入口目前通过 query 拉取权威汇总，没有全网遥测推送订阅。

## Master 清单与手机组网协议（1.2.0）

仅 Master USB / PIN 验证后的加密 GATT 可调用，EPOCH 必须当前，REQUEST 为非零 uint32。

```text
inventory EPOCH REQUEST
mesh-state EPOCH REQUEST
mesh-scan EPOCH REQUEST
mesh-join EPOCH REQUEST UUID32HEX
mesh-retry EPOCH REQUEST ADDRhex
```

新版 Master 不要求手机迁移。启动时若发现旧 Node 角色，固件自动清除 Mesh 凭据并重启；手机等待重连即可。为兼容旧维护工具，固件仍识别以下精确命令，但 APP 不发送，也不应把它作为现场步骤：

```text
mesh-migrate CONFIRM
```

仅旧固件仍处于 `MESH phase=LEGACY_NETWORK` 时才可能使用；新版正常启动路径会自行完成相同清理。应用 NVS（PIN、泵设置和任务记录）不受影响。

从机组网重置命令为 `mesh-reset CONFIRM`，手机在已配对、加密的 GATT 通道中即可发送（USB 维护口也保留同一协议作为现场备用）。主机要求当前为本机 Provisioner、空闲、无未确认任务或测试且存储正常。先返回 `OK mesh_reset_started`，逐个删除 Provisioner 节点表中的全部远端节点及其 Mesh 配置，清空应用层从机登记表后返回 `OK mesh_reset rebooting` 并自动重启。失败返回 `MESH_RESET_FAILED`，不会清除 PIN、泵设置、任务记录或主机 Provisioner 角色；重启后需重新扫描和加入从机。手机只在二次确认后发送该精确命令。

扫描15秒、候选最多8个；显式 join 才通过 PB-ADV 配网。Config Client 依次读取 Composition page0、AppKey Add、Vendor Model Bind及C001订阅，等待合法 HELLO 后 READY。配置请求4秒超时、最多3次尝试；握手25秒；配对90秒后取消链路。取消失败要求重启，期间禁止新输出。组网拒绝活动任务、输出和未知结算；任务/测试/调速也拒绝活动组网，停止与查询保留。

```text
MESH epoch=7 req=11 op=10 scan=10 phase=SCANNING busy=1 capable=1 reason=NONE found=1 target=0000
CAND epoch=7 scan=10 uuid=dddd504d00000000abcd020000000000
INV_BEGIN epoch=7 req=12 count=2
INV_NODE epoch=7 req=12 slot=1 addr=0002 online=1 commissioned=1
INV_NODE epoch=7 req=12 slot=0 addr=0003 online=0 commissioned=0
INV_END epoch=7 req=12 count=2
```

Provisioner 清单来自 IDF 持久化节点表，与真实控制槽位匹配；旧外部 Node 网络使用原登记表。slot=0表示已配对但尚未完成握手登记，不能选作出水节点。online依据15秒内 HELLO；commissioned还要求从机标定与登记一致。完整清单每100ms发送一行，最大32台。APP按当前epoch/request、完整BEGIN/NODE/END计数、唯一地址及槽位核验，全部完整才原子提交；缺包/超时不能推断0台。

状态只响应查询，不主动广播；组网进行时APP约2秒查询MESH，清单约10秒更新、15秒过期、8秒接收超时。自动接管期间可能短暂返回 `capable=0 phase=AUTO_MIGRATING` 或兼容的 `LEGACY_NETWORK`，主机重启后应返回 `capable=1 phase=IDLE`；应用 NVS 不受影响。
