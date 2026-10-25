# 协议 V6（基础五路候选）

Bluetooth Mesh Vendor CID `0x02E5`、Model ID `0x0001`，群组 `0xC001`。固定使用 NetKey index 0 / AppKey index 0。所有多字节字段显式小端，禁止直接发送 C++ 内存结构。

当前 Mesh 能力为 `revision=6, RequiredFeatures=0x15FF`（含维护互锁 `0x1000`）；标定扩展位 `0x0200/0x0800` 仍保留旧会话兼容。主从必须配套 V6，旧固件不能参与新任务；手机请求与最终结果格式保持兼容。产品最多 5 台，32 槽只保留存储/掩码 ABI。默认主机并发 1，五路候选环境并发 5，同一任务统一每路目标、各路独立结束。实施与验收状态见主机工程 `docs/BASIC_FIVE_OFFLINE_ACCEPTANCE_20261025.md`。

`JobKey` = `epoch:u32 + id:u32`，两者都非零。epoch 在主节点每次启动时持久化递增；id 在请求正式接受前递增并保存。节点地址来自 Mesh 上下文，不在业务载荷内重复声明。

| Op | 方向 | 载荷，按顺序 | 字节 |
| --- | --- | --- | --- |
| C0 PREPARE | 主→从，单播 | key, ml:u32, targetPulses:u32, k:u32, version:u32, maxMs:u32, speedFactorPermille:u16, leaseMs:u32, flags:u8, leaseFloor:u32 | 39 |
| C1 START | 主→从，单播 | key | 8 |
| C2 LEASE | 主→从，单播 | epoch:u32, sequence:u32 | 8 |
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
| CE LEASE_ACK OK | 从→主，单播 | epoch:u32, sequence:u32；长度 8 表示 OK | 8 |
| CE LEASE_ACK 拒绝 | 从→主，单播 | epoch:u32, sequence:u32, reply:u8（非 OK 的有效枚举） | 9 |
| CF COMMAND_ACK 拒绝 | 从→主，单播 | key, command:u8（C0/C1）, reply:u8（仅非 OK） | 10 |
| D0 CAPABILITIES_GET | 主→从，单播 | nonce:u32 | 4 |
| D1 CAPABILITIES | 从→主，单播 | nonce:u32, revision:u16, features:u16, buildId:u32 | 12 |
| D2 TARGET_CLOSED | 从→主，单播 | key，仅证明 Target 输出关阀 | 8 |
| D3 TARGET_CLOSED_ACK | 主→从，单播 | key，独立于最终结果确认 | 8 |
| D4 PREPARE_OK | 从→主，单播 | key，opcode 表示 Prepare 成功 | 8 |
| D5 START_OK | 从→主，单播 | key，opcode 表示 Start 成功 | 8 |
| D6 PROGRESS | 从→主，单播 | key, pulses:u32, runtimeMs:u32 | 16 |
| D7 MEASURE_MARK | 主→从，单播 | key，旧交互式标定停止计数 | 8 |
| D8 MEASURE_MARK_ACK | 从→主，单播 | key | 8 |
| D9 MODE_GET | 主→从，单播 | token:JobKey；关闭本地保持并退役该边界 | 8 |
| DA NODE_STATE | 从→主，单播 | token:JobKey, boot:u32, flags:u8, localReason:u8 | 14 |
| DB MAINTENANCE_GRANT | 主→从，单播 | token:JobKey, boot:u32, ttlMs:u32 | 16 |
| DC MAINTENANCE_RETIRE | 主→从，单播 | token:JobKey, boot:u32 | 12 |

PREPARE flags bit0 表示主机为本任务提供已认证的 K，bit1 启用 D6 实时快照；未知位拒绝。D6 仅使用从机保存的主节点单播地址，绝不发往 C001 群组或其他广播地址；主机校验任务和节点状态后计算毫升及流速，并通过现有加密 GATT 通知发送 `LIVE addr=NNNN job=E:I pulses=N ml=N runtime_ms=N rate_ml_min=N`。遥测是可丢弃的监控快照，不确认、不改变调度状态、不替代 RESULT。APP 仅在当前任务页显示，6 秒无新样本即显示过期；最终 FLOW/RESULT 仍是结算依据。

业务载荷之外还有 opcode、MIC 和 Mesh 层开销；业务载荷超过 8 字节时由当前传输层按分段报文处理。配置提供 16 个 RX 分段上下文、8 个 TX 上下文；它们不构成 32 节点吞吐保证。C6 不再发送周期流量摘要；D6 仅在 PREPARE 显式启用后、阀门打开期间每约 2 秒按节点单播发送。

从机阶段枚举顺序由 `include/protocol.h` 定义：`Idle, Prepared, Opening, Running, Settling, Done, Fault`。RESULT 的 phase 只能是 `Done` 或 `Fault`；bit0=阀门输出命令、bit1=已满足静默窗口、bit2=结算截断。主机只接受阀门输出已关闭且未知 flag 未置位的终态。revision 随状态变化递增；普通脉冲计数变化不增加 revision。主机按 `JobKey + revision` 去重并回传 RESULT_ACK。

从节点每次启动在 NVS 为 revision 预留新的 16 个计数值，每项任务最多使用 5 次状态变化。恢复的终态也改用本次启动的 revision；因此重启后的故障状态不会被重启前更高阶段的旧报文覆盖。计数耗尽则保持输出关闭。空闲时修改 K 不修改已完成任务保存的 K，历史毫升数仍按执行当时的系数解释。

调度枚举：`Idle, Preparing, Opening, Supplying, Draining, Between, Complete, Failed`。complete 掩码表示终态已收到，failed 表示不能认定成功，unknown 表示未取得可信最终结果。总量只累加匹配任务的数据；measured 掩码描述数据覆盖，不能将未覆盖节点解释为已出水 0mL。

主节点本地泵阶段编号保留为 `Off=0`、`Running=2`、`Fault=4`，其中 Running 只表示泵输出命令有效，不是实际运行反馈。`pumpFault` 编号为 `NONE=0`、`BOOT_LOCK=1`、`NOT_COMMISSIONED=2`、`AUTHORIZATION=7`、`RUNTIME=8`、`TRANSPORT=11`；其余旧编号保留不用，NOT_COMMISSIONED 当前也不主动产生。该调整未改变 Mesh 报文布局。主节点不采集缺水、压力或运行反馈。


`speedFactorPermille` 保留兼容旧从机的档位标识：低/中/高为 500/750/1000，不代表当前 PWM 占空比；实际占空比以 `PUMP_PWM duty` 为准，当前为 65/80/100%。

## 重试与关闭

能力缓存有效时不逐批探测；节点离线、build ID 变化或主机重启后需重新单播 CAPABILITIES_GET。能力探测必须在 PREPARE/START 前完成。从机每 10 秒发送一次 HELLO 在线心跳，主机回复 HELLO_REPLY；离线恢复或主机有未决结果时可触发按需 RESULT_QUERY。HELLO 不包含流量遥测。

主机按协议版本和必要功能位判断兼容性，不按 build ID 白名单限制启动。build ID 为非零固件追踪标识，用于日志和能力缓存失效；同次 CAPABILITIES 与近期 HELLO 的 build ID 必须一致。来源、nonce、在线时效和系数检查继续生效。build ID 变化本身不会导致兼容拒绝；V6 兼容性由 revision 与 features 决定。

能力探测和 PREPARE 各有固定 15s 期限；从机未开阀的 Prepared 窗口为 20s，重复 Prepare 不延长期限。Prepare 使用滚动准入：普通分段最多一条在途，额外待发 Prepare 至多一条；首发先于重试，按未确认节点公平轮询。Busy 不计实际提交次数，实际提交后 1500ms 才能重试，每命令最多提交 3 次。

Opening 阶段在尚无 Start 实际提交时最多等 1000ms；首条 Start 实际提交后启动固定 4000ms 预算，重试不滑动。所有未关闭节点都须具有 Start 实际提交、SDK accepted、D5 和匹配实际签发记录的 CE；Lease 签发不得早于本路 D5 被接受，启泵时证明年龄必须 <2000ms。早到的业务 ACK 可暂存，SDK accepted 后才应用；SDK 拒绝不能被业务 ACK 覆盖。普通状态报告不能代替命令成功证明。

定量与标定使用相同启动顺序：PREPARE → PREPARE_OK → START → 从机 GPIO4 置高开阀 → START_OK → 新鲜 LEASE_ACK → 启泵。主机还要求本批每个未关闭节点距首次有效 START_OK 至少 **300ms**；并发时以最后满足条件的节点为准，重复 START_OK 不重置等待时间。等待不阻塞续租，不延长固定 4000ms 启动预算，启泵瞬间仍重新检查租约证明年龄；取消、故障或全部阀门关闭均禁止启泵。START_OK 仅证明驱动输出命令已执行，不代表阀位反馈，300ms 动作余量需在实阀上验证。独立限时试泵仍走单独入口。

运行时每路实际 Lease 提交至少间隔 1000ms，按本批选中节点顺序使用 0/200/400/600/800ms 相位，稀疏注册槽位同样错开；错过周期不追赶突发。主机每路保存 10 条签发记录，运行证明期限 8000ms，等于期限立即保护停泵。时间基于实际签发，不能按 ACK 接收时刻虚增租约。从机 `leaseMs` 默认 12000ms；无流启动等待 5000ms、已有脉冲后无流 1500ms、静默 500ms、尾流结算上限 3000ms 均保持原值。

停止先本地断泵，再向本批未完成节点单播 CLOSE；取消期间每 700ms 重发关闭。Draining 最初 3500ms 等待主动最终结果，之后全局每 1000ms 只补查一个缺失节点，总收尾期限 10000ms。终态缺失标记 UNKNOWN，不自动重新出水。

Target 本地关输出后立即发 D2，后续每 `250+(slot%5)*10` ms 重试，最多 6 次，D3 或最终 C7 停止重试。主机校验来源、完整 key、本批及该路 Start 已实际提交；只锁存本路 closed 并清理该路待发 Start/Lease，不设置 final、成功或水量。最后一路关闭证明到达即停泵；一路先完成不影响其他路。机械阀位置没有传感器反馈，D2 只证明输出命令关闭。D2 全丢且结果也未到时，保护上界仍是最后有效 Lease 签发后 8000ms 加控制循环余量。

从节点终态 RESULT 最多尝试 6 次，按地址错开发送；未确认的已保存终态可在后续 HELLO/恢复查询时重新上报。正常运行期间不发送 SUMMARY 或 STATUS。CLOSE、LEASE、RESULT、ACK 和停止请求使用高优先级关键队列；关键队列溢出或分段发送异常会触发本地安全停止。

已配网节点重启直接恢复 Mesh 配置，不重新启用待配网过程。固定主地址检查、AppKey 绑定和注册表共同限制业务来源。全部节点均为受信任设备；本协议不防御已掌握网络密钥且运行恶意固件的内部节点伪造主地址。

## 租约作用域与紧凑报文隔离

新任务在与 worker 提交共用的传输锁内调用 `beginLeaseScope`：清理旧任务未提交授权、切换完整 JobKey，并取得全局最后分配序号 +1 作为非零 `leaseFloor`。所有 Prepare 带同一下界；重复 Prepare 改下界返回 Conflict。序号在实际提交时全局严格递增，所有节点和后续任务共用；UINT32_MAX 后拒绝继续分配，不回绕。已提交 SDK ticket 保留至真实回调。

从机只在当前开阀状态、epoch 匹配且 sequence≥floor 时接受新租约。首次 sequence 大于 2³¹ 合法，后续须严格增大；重复序号仅确认、不续期，旧序号与过期租约不能复活输出。主机按来源、epoch、sequence 查实际签发账本恢复完整 key；迟到的旧任务 ACK 无法成为新任务证明。重启先持久化新 epoch；从机重启关输出，不恢复开阀。

`Packet.context` 只保存本地完整 JobKey，不上无线、不落 NVS。C2/CE 序号位于 offset4。严格拒绝旧 C0=35、C2=12、CE=13、CF=OK，不能按前缀接收。D4/D5 只合并完全相同的在途成功副本；真实结束后仍能回复重试。合法 Start 清理待发 D4，合法 Lease 清理待发 D5；同命令拒绝清除待发旧 OK，不能撤回已在途消息。

## 资源限制、诊断与部署

主 SAR limit=2、从 SAR limit=1，普通分段在途最多 1；Pending=64、Reserved=16、Tickets=8，不扩大 SDK SAR 上下文。正常 C1/C2/CE(OK)/D2/D3/D4/D5 业务长度均 8，当前短 MIC 下应为非分段；必须用设备 `mesh_tx seg=0` 另行验证。SDK 调用在锁外，缺失真实完成回调不能伪释放 ticket；故障恢复仅允许诊断，不恢复业务授权。

每 10s 输出有界 `mesh_diag`、`MESH_STATE`、`runtime_mem`。`heap_free/heap_min/largest_free` 及 `app_stack_min_bytes/mesh_stack_min_bytes` 是字节，后两者为 ESP-IDF 任务栈历史最小剩余量；不能用静态 RAM 使用率代替运行趋势。`start_guard` 和 `start_proof` 在阶段/拒绝原因变化时记录实际签发、StartAck、证明年龄以及 closed/final。跨设备 uptime 不可直接相减。

生产默认环境 `pump_master` 的 `PUMP_MAX_CONCURRENT=1`；`pump_master_five_candidate` 显式设置为 5，供台架验证。只有第 9.2 节五台实机和水量/长时间运行门槛通过后才提升生产默认值。本轮 V4 不新增持久化结构迁移；主机现有格式为 3，历史格式 1/2 的说明应结合迁移代码解释。保留配网/标定与 NVS，回退不能恢复会复用 epoch 的旧 NVS 快照。

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

主机将整个计划作为一个任务先持久化，再按编号分批 PREPARE（批并发由主机环境配置决定）；每路独立计算目标脉冲，主从使用上述 V4 PREPARE 布局。手机断连不影响已接受任务，重试必须保留完整计划和原身份/序号；同序号任意一路目标改变返回 CONFLICT。统一设置仍使用原 `start` 和 37 字节 REQUEST。新建任务的 APP 水量上限为 2000mL，旧已接受任务/历史记录和真实累计流量不截断。

主机 NVS 格式为 3，保留旧布局和 16 个客户端计划，可导入格式 1/2，补全终止原因；维护锁使用独立 NVS 项，写入后读回验证。升级保留 PIN、登记表、客户端高水位、任务及结果。旧固件可能拒绝格式 3，降级需状态迁移及备份，不能清空 fence。

## 基础版本门禁与维护

基础版本拒绝新的 `start-each`、交互式标定和独立试泵，保留旧会话查询/停止。`hello` 附带 `CAPS epoch=N max_nodes=5 max_concurrent=N basic=1 maintenance=1` 和 `MAINT locked=0|1 addr=NNNN token=E:G boot=N pending=N`（0=无请求，1=自动启动证明，2=维护进入证明，3=维护退出证明）。APP 只用当前启动的 CAPS，未收到可信维护状态不放行启动。

自动启动前以新 token 向所有登记节点获取 DA。flags 位为 permit=1、output=2、busy=4、pressed=8、quiet=16、healthy=32、retired=64；只有严格等于 112 的关闭、健康、静流、退役组合才可通过。证明总截止 12 秒，失败不启动，不能仅检查本次 selected mask。

仅主机 USB 接受 `maintenance-enter ADDRhex CONFIRM`、`maintenance-exit CONFIRM`、`maintenance-status`。进入先全员证明、持久化主机锁，再向目标授予绑定其 boot 的 60 秒单次许可；重复 grant 不续期，retire 后旧 grant 永不复活。维护期间主机不启泵，从机按钮仍受释放再按、无流与最长时限保护。退出须全员确认关闭/退役并清除待发 grant 后持久化解锁。超时、从机离线、主机重启不自动解锁，不能通过删节点/换址绕开。

从机终态保存成功才发送 RESULT；主机先验证 revision/计量一致性，保存真实终态后才发 FLOW/RESULT_ACK。保存失败持续关闭并锁定新任务，不允许新 CLOSE/PREPARE 覆盖未落盘结果；重复终态不重复写。重启升 revision 只允许相同计量，合成 Reboot 未知可由可信真实终态补齐，但不撤销本轮已发生故障。

配置状态另发 `MESH_BUDGET epoch=N op=N remaining_ms=N recovery=NONE|RESTART_REQUIRED`。APP 将预算绑定当前 epoch/operation，显示最近报告值而非本地保证倒计时。单回调 18 秒、单步骤 75 秒、配置总 330 秒、配网总 420 秒；缺回调、事件溢出或 SDK 上下文不明进入隔离，迟到事件不能解锁。SDK 无请求 token 的旧回调归属和 RF/SAR 行为仍需实机验证。

以下独立试泵/高级标定条目为旧会话协议兼容说明，不表示基础版本允许新启动。

UUID、命令格式见 README。维护参数仍只允许 USB 修改；业务 GATT 开启前必须设置六位 PIN。使用安全连接、MITM 和加密属性权限。手机写入一条命令后需等待权威 R 响应，或查询确认。旧 Mesh 角色由 Master 启动流程自动接管。

主节点 `local` 返回 `LOCAL pump_command=N pump_phase=N fault=NAME`，不再返回传感器或运行反馈字段；客户端不能把 `pump_command=0` 解释为已通过传感器确认停泵。

### Master 独立试泵

`pump-test CLIENT_HEX SEQ EPOCH DURATION_MS` 启动 1～10000ms 的本地通水测试；`pump-dry-test CLIENT_HEX SEQ EPOCH DURATION_MS` 是兼容旧客户端的限时测试协议（APP 1.2.0 无此入口），1～3000ms、强制低档，执行中拒绝升档；均不超过板卡 `maxRunMs`。`pump-test-stop CLIENT_HEX START_SEQ EPOCH` 取消原测试；`pump-test-query CLIENT_HEX START_SEQ EPOCH` 查询。仅 Master USB/经过 PIN 验证的加密 GATT 支持，Slave 不转发。无需 Mesh 配网或 USB `pump-reset`，但必须已配置输出、冷却完成、存储正常、无活动定量任务和未知结果。测试仅允许 NONE 或 BOOT_LOCK；其他故障不绕过。若从 BOOT_LOCK 开始，停止和超时均恢复该锁定，正常定量任务依旧需要 USB 复位。

响应为 `PUMP_TEST client=HEX seq=N epoch=N reply=NAME state=UNKNOWN/RUNNING/STOPPED op=START/STOP/QUERY`。APP 匹配身份、序号和启动 epoch；GATT 写成功不等于接受。未确认的查询或停止失败不能把待确认启动标为拒绝。只允许明确 START 拒绝或权威停止状态释放该测试；确认终态后不因迟到响应恢复执行态。

接受记录与定量请求共用持久化客户端高水位；先落盘再输出。存储格式 2 兼容导入原版本 1 的记录，mask=0、ml=0 表示通水测试，mask=0、ml=1 表示快速测试；相同序号不同模式冲突。重复启动不延长时限、不再次输出。停止先到时写入同序号 tombstone，确保尚未处理的启动之后无法运行。主机本地定时关闭，不依赖手机和续租；重启关闭输出且不会恢复旧测试。

Master `LOCAL` 增加 `test_supported=1 test_active=N test_ms=N test_client=HEX test_seq=N pump_ready=N`。运行期间及结束时发布状态；测试不输出流量结果，也不自动打开阀门。`STOPPED` 仅表示该测试不再驱动输出，当前输出还需检查 `pump_command`，并非运行传感器反馈。

### Master PWM 三档调速

`local` 还返回 `PUMP_PWM epoch=N rev=N level=N duty=N hz=N output=N reply=NAME test_manual=1 dry_max_ms=3000 dry_active=N`。APP 只在当前 epoch 的新鲜快照明确声明 test_manual=1 时允许 BOOT_LOCK 下测试和选档；dry_max_ms 声明快速测试上限，dry_active 声明当前快速测试。旧固件未声明这些能力时保留原限制。`level` 为 1/2/3（低/中/高），`duty` 为所选档位占空比百分数，即使停泵也保留所选值；`output=0` 时实际 PWM 为 0%，`output=1` 表示输出该档位 PWM，不是转速传感器反馈。当前默认 65/80/100%，20 kHz 需按驱动规格确认。快速测试成功后强制低档并递增 PWM 修订号。

通过 Master USB 或已完成 PIN 配对的加密 GATT 发送 `pump-speed EPOCH EXPECTED_REV LEVEL`。使用最新快照中的启动 epoch 与全局修订号；成功才递增修订号，不匹配则拒绝。修订号每次启动从 0 开始，达到 uint32 上限后拒绝修改，不回绕。档位只保存于内存，重启恢复低档和关闭输出。Slave 不转发此命令。

调速要求硬件已启用、仅 NONE 或 BOOT_LOCK、存储健康且没有活动定量任务或未知定量结果。空闲选档不启泵，通水测试中可调速但不改变原截止时间或授权期限；快速测试中仅允许低档，定量任务运行中不允许修改。PWM 驱动失败会关闭输出并锁定 TRANSPORT 故障。APP 等待匹配 epoch、递增修订号的设备确认，GATT 写入成功不等于设置成功；超时只刷新或重连，不自动重发。档位待确认时禁止新任务和新测试，PWM 状态过期时禁止新测试，原测试停止入口仍可用。

`R client=... seq=... reply=... job=epoch:id phase=N mask=... done=... failed=... unknown=...`

`V job=epoch:id total_ml=N measured=...`

未确认转发只返回 `PENDING`，丢失响应后可重试原请求；读 TX 只能读最新一行。通知按换行拼接，不将单个 ATT 分片当作完整响应。任务运行期间，主节点在收到并校验从机单播 D6 后，通过现有加密 GATT 通知发送 `LIVE` 行；它只包含当前已启动任务节点的最新快照，不使用 Mesh 群组/广播，也不代替最终 FLOW/RESULT。其他入口仍通过 query 拉取权威汇总。

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
