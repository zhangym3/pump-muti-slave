# 协议 V1

Bluetooth Mesh Vendor CID `0x02E5`、Model ID `0x0001`，群组 `0xC001`。固定使用 NetKey index 0 / AppKey index 0。所有多字节字段显式小端，禁止直接发送 C++ 内存结构。

`JobKey` = `epoch:u32 + id:u32`，两者都非零。epoch 在主节点每次启动时持久化递增；id 在请求正式接受前递增并保存。节点地址来自 Mesh 上下文，不在业务载荷内重复声明。

| Op | 方向 | 载荷，按顺序 | 字节 |
| --- | --- | --- | --- |
| C0 PREPARE | 主→从，单播 | key, ml:u32, targetPulses:u32, k:u32, version:u32, maxMs:u32 | 28 |
| C1 START | 主→从，单播 | key | 8 |
| C2 LEASE | 主→从，群组 | key, sequence:u32 | 12 |
| C3 CLOSE | 主→从，群组 | key, reason:u8 | 9 |
| C4 STATUS_GET | 主→从，单播 | key，零值表示查询当前/最近任务 | 8 |
| C5 STATUS | 从→主 | key, pulses:u32, closePulses:u32, k:u32, revision:u32, phase:u8, reason:u8, flags:u8 | 27 |
| C6 SUMMARY | 从→主 | key, pulses:u32, revision:u32, phase:u8, valveCommand:u8 | 18 |
| C7 EVENT_ACK | 主→从 | key, revision:u32 | 12 |
| C8 REQUEST | 入口→主 | action:u8, epoch:u32, client:u64, sequence:u32, mask:u32, ml:u32, maxMs:u32, stopKey | 37 |
| C9 RESPONSE | 主→入口 | client:u64, sequence:u32, reply:u8, key, schedulePhase:u8, selected:u32, complete:u32, failed:u32, unknown:u32, totalMl:u32, measured:u32 | 46 |
| CA HELLO | 从→主 | k:u32, version:u32, commissioned:u8 | 9 |
| CB HELLO_REPLY | 主→从 | epoch:u32, pumpFault:u8, schedulePhase:u8 | 6 |

业务载荷之外还有 opcode、MIC 和 Mesh 层开销，大部分消息会分段。18 字节摘要也不是一个空口包。配置提供 16 个 RX 分段上下文、8 个 TX 上下文；它们不构成 32 节点吞吐保证。

状态枚举顺序由 `include/protocol.h` 定义：`Idle, Prepared, Opening, Running, Settling, Done, Fault`。STATUS flags：bit0=阀门输出命令，bit1=已满足静默窗口，bit2=结算截断。任务内 revision 随状态变化递增，普通周期脉冲增长不增加 revision。SUMMARY 只有任务及 revision 与详细状态一致且脉冲不回退时才进入流量缓存，不推进调度和保护状态。

从节点每次启动在 NVS 为 revision 预留新的 16 个计数值，每项任务最多使用 5 次状态变化。恢复的终态也改用本次启动的 revision；因此重启后的故障状态不会被重启前更高阶段的旧报文覆盖。计数耗尽则保持输出关闭。空闲时修改 K 不修改已完成任务保存的 K，历史毫升数仍按执行当时的系数解释。

调度枚举：`Idle, Preparing, Opening, Supplying, Draining, Between, Complete, Failed`。complete 掩码表示终态已收到，failed 表示不能认定成功，unknown 表示未取得可信最终结果。总量只累加匹配任务的数据；measured 掩码描述数据覆盖，不能将未覆盖节点解释为已出水 0mL。

主节点本地泵阶段编号保留为 `Off=0`、`Running=2`、`Fault=4`，其中 Running 只表示泵输出命令有效，不是实际运行反馈。`pumpFault` 编号为 `NONE=0`、`BOOT_LOCK=1`、`NOT_COMMISSIONED=2`、`AUTHORIZATION=7`、`RUNTIME=8`、`TRANSPORT=11`；其余旧编号保留不用，NOT_COMMISSIONED 当前也不主动产生。该调整未改变 Mesh 报文布局。主节点不采集缺水、压力或运行反馈。

## 重试与关闭

PREPARE / START 按每 150ms 一个未确认节点轮询，分别有 10s / 4s 的阶段上限。默认每批 1 个节点，不应直接把并发改成 32 而沿用这些阶段时限。群组租约每 1s 更新，从节点 3s 未收到新的序号就本地关阀；重复 START 和重复租约不会延长授权。

停止先本地断泵，再发 CLOSE；取消期间每 700ms 重发关闭，未取得终态则每 300ms 补查一个节点，最多等待 10s。关闭命令如果先于 PREPARE 到达，会在空闲从节点持久化取消栅栏，后来的 PREPARE / START 无法复活该任务。终态缺失标记 UNKNOWN，不自动重新出水。

从节点状态事件最多重试 6 次，重试间隔按地址错开；活动摘要每 2s 发送，Hello 每 10s。主节点对 Hello 补查当前结果，因此有限事件重试结束后仍可通过查询对账。Close、Lease、Stop 控制请求使用独立高优先级队列；关键队列溢出触发本地安全停止。

已配网节点重启直接恢复 Mesh 配置，不重新启用待配网过程。固定主地址检查、AppKey 绑定和注册表共同限制业务来源。全部节点均为受信任设备；本协议不防御已掌握网络密钥且运行恶意固件的内部节点伪造主地址。

## 客户端请求身份

启动 action=1，停止 action=2，查询 action=3。每个客户端使用持久化随机非零 64 位身份与递增非零 32 位序号。最大保存 16 个客户端高水位记录，容量满时拒绝新增身份，不清除旧记录来腾位置。达到 UINT32_MAX 时需维护迁移身份，不允许回绕。

相同身份 / 序号 / 参数返回原任务；相同序号不同参数返回 CONFLICT；更小序号返回 STALE。入口节点地址不参与去重，所以换手机入口不会产生新任务。查询使用被查询启动请求的原序号；停止包含明确 stopKey，旧任务停止报文不能取消新任务。

主节点在持久化请求与任务号之后才发 PREPARE。从节点在持久化任务栅栏之后才确认准备。主节点重启将上次未完成任务标记失败 / 未知，仍不重发 START；收到从节点终态后补充最近任务的结果。

若从节点中途重启且没有保存完整终态，本次出水量标记未知，不把复位后的零计数当作最终零出水；已保存的完整结果可恢复并重新上报。

## GATT 文本层

UUID、命令格式见 README。维护参数只允许 USB 修改；业务 GATT 开启前必须设置六位 PIN。使用安全连接、MITM 和加密属性权限。手机写入一条命令后需等待权威 R 响应，或查询确认。

主节点 `local` 返回 `LOCAL pump_command=N pump_phase=N fault=NAME`，不再返回传感器或运行反馈字段；客户端不能把 `pump_command=0` 解释为已通过传感器确认停泵。

### Master 独立试泵

`pump-test CLIENT_HEX SEQ EPOCH DURATION_MS` 启动 1～10000ms 的本地通水测试；`pump-dry-test CLIENT_HEX SEQ EPOCH DURATION_MS` 是兼容旧客户端的限时测试协议（APP 1.2.0 无此入口），1～3000ms、强制低档，执行中拒绝升档；均不超过板卡 `maxRunMs`。`pump-test-stop CLIENT_HEX START_SEQ EPOCH` 取消原测试；`pump-test-query CLIENT_HEX START_SEQ EPOCH` 查询。仅 Master USB/经过 PIN 验证的加密 GATT 支持，Slave 不转发。无需 Mesh 配网或 USB `pump-reset`，但必须已配置输出、冷却完成、存储正常、无活动定量任务和未知结果。测试仅允许 NONE 或 BOOT_LOCK；其他故障不绕过。若从 BOOT_LOCK 开始，停止和超时均恢复该锁定，正常定量任务依旧需要 USB 复位。

响应为 `PUMP_TEST client=HEX seq=N epoch=N reply=NAME state=UNKNOWN/RUNNING/STOPPED op=START/STOP/QUERY`。APP 匹配身份、序号和启动 epoch；GATT 写成功不等于接受。未确认的查询或停止失败不能把待确认启动标为拒绝。只允许明确 START 拒绝或权威停止状态释放该测试；确认终态后不因迟到响应恢复执行态。

接受记录与定量请求共用持久化客户端高水位；先落盘再输出。保留存储格式版本 1，mask=0、ml=0 表示通水测试，mask=0、ml=1 表示快速测试；相同序号不同模式冲突。重复启动不延长时限、不再次输出。停止先到时写入同序号 tombstone，确保尚未处理的启动之后无法运行。主机本地定时关闭，不依赖手机和续租；重启关闭输出且不会恢复旧测试。

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

扫描15秒、候选最多8个；显式 join 才通过 PB-ADV 配网。Config Client 依次读取 Composition page0、AppKey Add、Vendor Model Bind及C001订阅，等待合法 HELLO 后 READY。配置请求4秒超时、最多3次尝试；握手25秒；配对90秒后取消链路。取消失败要求重启，期间禁止新输出。组网拒绝活动任务、输出和未知结算；任务/测试/调速也拒绝活动组网，停止与查询保留。

```text
MESH epoch=7 req=11 op=10 scan=10 phase=SCANNING busy=1 capable=1 reason=NONE found=1 target=0000
CAND epoch=7 scan=10 uuid=dddd504d3076f5ba02ce020000000000
INV_BEGIN epoch=7 req=12 count=2
INV_NODE epoch=7 req=12 slot=1 addr=0002 online=1 commissioned=1
INV_NODE epoch=7 req=12 slot=0 addr=0003 online=0 commissioned=0
INV_END epoch=7 req=12 count=2
```

Provisioner 清单来自 IDF 持久化节点表，与真实控制槽位匹配；旧外部 Node 网络使用原登记表。slot=0表示已配对但尚未完成握手登记，不能选作出水节点。online依据15秒内 HELLO；commissioned还要求从机标定与登记一致。完整清单每100ms发送一行，最大32台。APP按当前epoch/request、完整BEGIN/NODE/END计数、唯一地址及槽位核验，全部完整才原子提交；缺包/超时不能推断0台。

状态只响应查询，不主动广播；组网进行时 APP 约 2 秒查询 MESH，清单约 10 秒更新、15 秒过期、8 秒接收超时。旧 Node 角色由 Master 启动流程自动处理并重启进入 Provisioner；APP 不发送迁移命令。从机只响应 Master 的 Mesh 业务，不提供手机迁移入口。
