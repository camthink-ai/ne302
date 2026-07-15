# 抓拍上传增强功能 — 实施计划

> 范围：在 ne301 现有抓拍/上传链路上叠加「抓拍模式 / 保存位置 / 存储策略 / 上传协议 / 上传数量 / 上传时间 / 重传机制」等行为；Web 端独立菜单「抓拍设置」，承载所有相关配置。
> 
> 不在范围（重要约束）：[Custom/Hal/Quick_Bootstrap/](../Custom/Hal/Quick_Bootstrap/) 下的 `quick_snapshot_*` 及 [system_service.c#L3351](../Custom/Services/System/system_service.c#L3351) 内 `requires_time_optimized_mode` 分支（boot-time RTC 唤醒快拍 + AI）不重构，沿用其上传方式。新机制只作用于「常规抓拍 / 非快拍」路径以及后台 store-and-forward 协调器。

---

## 1. 现状梳理（与新需求的衔接点）

### 1.1 抓拍/上传主入口（要复用）

- [system_service_capture_request()](../Custom/Services/System/system_service.c#L3084)：唯一抓拍请求入口，做并发互斥 / OTA 互斥，最后转 `system_service_capture_and_upload_mqtt()`。
- [system_service_capture_and_upload_mqtt()](../Custom/Services/System/system_service.c#L3313)：
  - Step 1 拍照（fast 分支：RTC/PIR 等需要时间优化 → quick_snapshot；非 fast：常规拍照）。
  - Step 1.1 直接调用 `sd_write_file()` 把 `image_<timestamp>_<frame_id>.jpg / _inference.jpg / _inference.json` 写到 SD 卡。
  - Step 3.1 / Step 4 同步阻塞 MQTT 上传。
  - Step 4.5 同步触发 Webhook 推送任务（已是异步队列）。
  - 失败直接返回错误，**无任何持久化重试**。
- [default_capture_callback()](../Custom/Services/System/system_service.c#L1163)：所有唤醒源（RTC/PIR/Button）统一进 `wakeup_task_async()` → 调用 `system_service_capture_request()`。

### 1.2 配置/持久化（要扩展）

- [image_config_t](../Custom/Core/System/json_config_mgr.h#L375)：当前包含 `fast_capture_*` / `capture_disable_comm` / `capture_storage_ai`。
- 全局配置 `aicam_global_config_t` 持久化到 `/config/aicam_config.json`，函数 `json_config_get_*` / `json_config_set_*`。
- [device_service_storage_set_cyclic_overwrite()](../Custom/Services/Device/device_service.h#L187)：已有循环覆盖钩子（但只是 placeholder）。

### 1.3 存储原语（要复用）

- 抽象层 [generic_file.h](../Custom/Common/Utils/generic_file.h)：`file_fopen/fwrite/remove/opendir/readdir/stat` 已对 `FS_FLASH (littlefs)` 和 `FS_SD (FileX/exFAT)` 各实现一份。
- `disk_file_*` 系列 → 显式指定 `FS_Type_t`（不依赖当前 active FS handle）。
- [sd_write_file()](../Custom/Services/Device/device_service.c#L1078)：硬编码写 SD，根目录文件名 `image_<ts>_<frame_id>.jpg`，没有重命名/状态字段。

### 1.4 上传通道（要扩展，不替换）

- MQTT：[mqtt_service_publish_image_with_ai / _chunked](../Custom/Services/MQTT/mqtt_service.h#L421)，元数据结构 `mqtt_image_metadata_t`、`mqtt_ai_result_t`。
- Webhook：[webhook_service_push_capture()](../Custom/Services/Webhook/webhook_service.h#L38)，已是「投队列 + 后台 task」+ `wait_pending` 拦截睡眠。

### 1.5 Web 现状

- 路由 [Web/src/router/index.tsx](../Web/src/router/index.tsx) + 菜单 [layout/pc/menu.tsx](../Web/src/layout/pc/menu.tsx)：`hardware-management` → `system-settings` 之间无菜单项；新增 `/capture-settings` 即可。
- 当前 `hardwareManagement/graphics.tsx` 中「抓拍配置」段（[graphics.tsx#L618-L818](../Web/src/pages/hardwareManagement/graphics.tsx#L618-L818)）需整体迁移到新页面，并扩展。

---

## 2. 总体方案

引入一个全新的 **Upload Coordinator（上传协调器）** 服务，负责：

1. 落地（store）：抓拍完成立即把图片+元数据 JSON 写到选定存储（SD / Flash）。
2. 调度：根据「抓拍模式」决定立即上传 / 入队 / 定时触发。
3. 重传：扫描存储中状态为 PENDING 的 record，逐条尝试上传，成功后改名标记。
4. 清理：按「存储策略」选择「循环覆盖（旧→新删）」或「存满即停」。

`system_service_capture_and_upload_mqtt()` 改造为「拍 + 落地 + 委托给 coordinator」，不再阻塞等待上传完成（除非模式 = 即拍即传 + 不保存）。

**fast-capture 分支保持原行为**（即拍即传 + 直传 MQTT/Webhook，失败即丢，没有重传），boot-time 路径不进 coordinator。

---

## 3. 数据/文件模型设计

### 3.1 目录布局与状态机

**所有产物落在独立目录 `/captures/` 之下，绝不放文件系统根**。状态通过子目录承载（不再用后缀方案）：

```
/captures/
├── pending/   cap_<unix_ts>_<seq>.json   ← 仅放 metadata（状态 = 所在子目录）
├── sent/      cap_<unix_ts>_<seq>.json
├── failed/    cap_<unix_ts>_<seq>.json
├── local/     cap_<unix_ts>_<seq>.json
└── data/
    ├── cap_<unix_ts>_<seq>_p.jpg        ← 原图
    ├── cap_<unix_ts>_<seq>_i.jpg        ← 推理图（AI 有命中才生成）
    └── cap_<unix_ts>_<seq>_a.json       ← AI result
```

设计要点：
- 状态变更 = **单次 `file_rename`** 把 metadata 在 `pending/` → `sent/` / `failed/` 间挪动，`data/` 不动。原子、O(1)。
- 重传循环只遍历 `pending/`，不被大量 sent 拖慢。
- 用户在 Web 端「重试 failed」= `rename(failed/X.json → pending/X.json)` + `upload_coordinator_kick()`。
- 「只存不传」`mode == LOCAL_ONLY` 的 metadata 直接落到 `local/`，永远不被重传扫到。
- 命名：`<unix_ts>`（绝对秒，RTC 未校时则用 0 + boot offset 兜底，metadata 内同时记 `boot_offset_ms`，校时后可纠序），`<seq>` 启动后 RAM 单调自增解决同秒冲突。
- coordinator 启动时通过 `disk_file_mkdir` 在目标 FS 上自动建齐 5 个子目录。
- 选 `storage == AUTO` 时，**优先 SD**：若 SD 就绪，目标 = `/<SD>/captures/`；否则 = `/<flash>/captures/`。已落到 SD 的 record 中途 SD 拔出 → 留在 SD，下次插入时 coordinator 在 enqueue 前会扫两边目录，因此 record 不会丢，只是暂不可上传。

### 3.2 Metadata JSON 内容（`_m.json`）

```json
{
  "version": 1,
  "image_id": "cam01-1735200001-3",
  "timestamp": 1735200001,
  "wakeup_source": "pir",
  "trigger_type": "pir",
  "battery_voltage_mv": 3920,
  "battery_percent": 82,
  "device_name": "cam01",
  "serial_number": "...",
  "width": 1920, "height": 1080,
  "size": 245678, "quality": 85, "format": "jpeg",
  "primary_image": "cap_1735200001_3_p.jpg",
  "inference_image": "cap_1735200001_3_i.jpg",
  "ai_result_file": "cap_1735200001_3_a.json",
  "upload_protocol": "mqtt|webhook",
  "retry_count": 0,
  "last_error": null
}
```

抓拍当下采集的所有「现场信息」（电量、唤醒源、ts）写进 metadata，上传时直接读 metadata 而非现取，**确保上传内容与抓拍时刻一致**（重传的关键不变量）。

### 3.3 「保存位置」选择规则

| 配置值 | 行为 |
|---|---|
| `auto` | 启动/抓拍前探测：`device_service_storage_is_sd_connected()` ＆ `sd_mode == NORMAL` → 用 SD；否则用 littlefs。 |
| `flash`| 始终用 littlefs；空间不足按存储策略。 |
| `sd`   | 始终用 SD；SD 未就绪 → 抓拍失败（或降级 fallback，按需求 4「即拍即传 + 不保存」例外）。 |
| `none` | 不写盘；仅在「即拍即传」模式下可选。RAM buffer 直接送 upload → 成功/失败都释放，不重传。 |

约束：`none` 只在「抓拍模式 = 即拍即传」时可选，前端 disable + 后端校验。

### 3.4 「存储策略」

| 配置值 | 行为 |
|---|---|
| `wrap_around`（循环覆盖） | 写入前若可用空间 < 文件大小 + 安全水位（512KB），按 `<unix_ts>` 升序遍历 captures 子目录，**优先级 `sent/ → local/ → failed/ → pending/`** 删除（删 metadata 同时删 `data/cap_<ts>_<seq>_*`）。删够空间停止。 |
| `stop_when_full`（存满即停） | 空间不足直接放弃这次保存，返回错误，并触发一次告警事件（可走 MQTT 状态主题）。 |

---

## 4. 后端实现

### 4.1 配置结构扩展

文件：[Custom/Core/System/json_config_mgr.h](../Custom/Core/System/json_config_mgr.h)

在 `image_config_t` **不动**（保留 `fast_capture_*` 给 fast 分支用），新增独立段：

```c
typedef enum {
    CAPTURE_MODE_INSTANT    = 0,  // 即拍即传（默认）
    CAPTURE_MODE_BATCH      = 1,  // 批量上传
    CAPTURE_MODE_SCHEDULED  = 2,  // 定时上传
    CAPTURE_MODE_LOCAL_ONLY = 3,  // 只存不传
} capture_mode_t;

typedef enum {
    CAPTURE_STORE_AUTO  = 0, // 自动选择（SD 优先）
    CAPTURE_STORE_FLASH = 1,
    CAPTURE_STORE_SD    = 2,
    CAPTURE_STORE_NONE  = 3, // 不保存（仅 INSTANT 可用）
} capture_storage_t;

typedef enum {
    STORAGE_POLICY_WRAP   = 0,
    STORAGE_POLICY_STOP   = 1,
} storage_policy_t;

typedef enum {
    UPLOAD_PROTO_MQTT    = 0,
    UPLOAD_PROTO_WEBHOOK = 1,
} upload_proto_t;

#define CAPTURE_SCHEDULE_MAX_NODES  8

typedef struct {
    uint32_t        version;                 // schema version
    capture_mode_t  mode;                    // 抓拍模式
    capture_storage_t storage;               // 保存位置
    storage_policy_t  policy;                // 存储策略（storage != NONE 时生效）
    upload_proto_t  upload_protocol;         // 上传协议（mode != LOCAL_ONLY）

    // 重传机制（mode != LOCAL_ONLY）
    aicam_bool_t    retry_enable;            // 开启失败重传
    uint8_t         retry_max_attempts;      // 单条 record 最大尝试次数（默认 5）
    uint32_t        retry_backoff_sec;       // 重试退避基础值（默认 30s，指数倍增）

    // 批量上传
    uint16_t        batch_count;             // 达到 N 条触发上传（mode == BATCH）

    // 定时上传
    uint8_t         schedule_node_count;
    uint16_t        schedule_minutes[CAPTURE_SCHEDULE_MAX_NODES]; // 一天内分钟数 0-1439

    // 通用
    uint32_t        keep_sent_hours;         // .sent 文件保留时长（小时，0=立即删，默认 168=7天）
    uint32_t        max_pending_records;     // 队列容量上限（默认 200，防止失控）
} capture_upload_config_t;
```

挂载点（在 `aicam_global_config_t` 内追加，[json_config_mgr.h#L546](../Custom/Core/System/json_config_mgr.h#L546)）：

```c
capture_upload_config_t capture_upload;
```

新增 getter/setter（参考 webhook 同款）：

```c
aicam_result_t json_config_get_capture_upload_config(capture_upload_config_t *cfg);
aicam_result_t json_config_set_capture_upload_config(const capture_upload_config_t *cfg);
```

默认值：`mode=INSTANT, storage=AUTO, policy=WRAP, upload_protocol=MQTT, retry_enable=TRUE, batch_count=10, schedule_node_count=0, keep_sent_hours=168, retry_max_attempts=5, retry_backoff_sec=30, max_pending_records=200`。

### 4.1.1 与休眠/唤醒的关系（必读）

**核心区分**：「定时抓拍」和「定时上传」是两套独立的时间表，由两类不同的 RTC 唤醒承载 —

| 维度 | 定时抓拍 | 定时上传 |
|---|---|---|
| 配置位置 | 功能调试 → 唤醒源 `timer_trigger` | 抓拍设置 → 抓拍模式 = SCHEDULED |
| 配置承载 | `work_mode_config_t.timer_trigger`（既有） | `capture_upload_config_t.schedule_minutes[]`（新增） |
| 醒来后干什么 | **拍一张** + 按 `capture_upload.mode` 决定是否传 | **只 flush `pending/` 队列** — 不抓拍 |
| 物理唤醒源 | U0 RTC（已有 `PWR_WAKEUP_FLAG_RTC_TIMING` / `_ALARM_A` / `_ALARM_B`） | U0 RTC（**新增** `PWR_WAKEUP_FLAG_UPLOAD_FLUSH`） |
| 与其它唤醒关系 | 与 PIR / 按键并列 | **独立**，不蹭别的唤醒事件 |

**关键规则**：定时上传**不**机会式地搭别的唤醒便车 — 你不开 schedule 就完全不会因为「定时上传」唤醒；你开了就老老实实在节点时间唤醒一次，干完上传就睡。

`mode == BATCH` 仍然按"积累 N 张再传"，靠下次任意唤醒顺带触发；不引入新的唤醒类型。`mode == INSTANT` 在抓拍唤醒后同步上传完才睡，行为不变。

### 4.1.2 定时上传的实现：N6 本地时间表查表（不改 U0）

**设计前提**：U0 只负责"按 `sleep_second` 把 N6 叫醒"，对它来说 RTC 唤醒就是 RTC 唤醒，不区分语义。区分留在 N6 本地：醒来后拿当前 RTC 时间，去合并后的时间表里查"这一刻该做什么"。**不需要新的 wakeup flag、不需要 U0 固件改动、不需要 ms_bridging 协议升级**。

#### 4.1.2.1 统一查表器 `wake_scheduler`

新增 [Custom/Core/System/wake_scheduler.h/c](../Custom/Core/System/) 作为纯 utility（不引入新服务），把两类时间表合并成统一接口：

```c
typedef enum {
    WAKE_DUTY_CAPTURE,        // 来源：work_mode_config.timer_trigger（既有）
    WAKE_DUTY_UPLOAD_FLUSH,   // 来源：capture_upload_config.schedule_minutes[]（新增）
    WAKE_DUTY_MAX,
} wake_duty_t;

typedef struct {
    wake_duty_t duty;
    uint64_t    due_unix_sec;  // 绝对时间戳
} wake_event_t;

/// 算出 [now, now + horizon_sec] 内最近一个事件（入睡前算 sleep_second 用）
uint64_t wake_scheduler_next_event(uint64_t now_unix_sec,
                                   uint32_t horizon_sec,
                                   wake_event_t *out);

/// 查表：[from, to] 窗口内命中的所有事件（按 due 升序，跳过已处理）
/// 用于唤醒后判断这次 RTC 是谁触发的
int  wake_scheduler_due_events(uint64_t from_unix_sec,
                               uint64_t to_unix_sec,
                               wake_event_t *out_events,
                               int max_events);

/// 标记某 duty 在某时刻已处理（持久化到 NVS，防重复触发）
void wake_scheduler_mark_handled(wake_duty_t duty, uint64_t at_unix_sec);

/// RTC 大幅校时后调用，清掉 handled-at 状态
void wake_scheduler_reset_state(void);

/// 配置变更后调用，使下次 next_event 重新计算
void wake_scheduler_invalidate(void);
```

数据源：
- `WAKE_DUTY_CAPTURE`：从 `work_mode_config.timer_trigger`（`enable / interval_sec / time_node[] / weekdays / start_time`，沿用既有结构）。
- `WAKE_DUTY_UPLOAD_FLUSH`：从 `capture_upload_config.schedule_minutes[]`（mode == SCHEDULED 时生效）。

时区参考既有 `scheduler_manager.timezone`。

#### 4.1.2.2 入睡路径接入

[system_service_enter_sleep()](../Custom/Services/System/system_service.c) 调用 `u0_module_enter_sleep_mode_ex()` 之前的位置改造：

```c
uint64_t now = rtc_get_timeStamp();

// 兜底：入睡前把已过期但未处理的节点当场处理（防 PIR 处理跨过节点）
wake_event_t overdue[4];
int n_overdue = wake_scheduler_due_events(0, now, overdue, 4);
for (int i = 0; i < n_overdue; i++) handle_due_event(&overdue[i]);

// 算下一个事件 → sleep_second
wake_event_t ev;
uint64_t next = wake_scheduler_next_event(now, /*horizon=*/24*3600, &ev);
uint32_t sleep_sec = next ? (uint32_t)(next - now) : 0;

// 不下发 intent —— 完全沿用既有 API
u0_module_enter_sleep_mode_ex(allowed_wakeup_mask, switch_bits, sleep_sec, NULL, NULL);
```

接口完全不变 —— 之前只考虑 timer_trigger，现在把 upload schedule 也纳入合并计算。

#### 4.1.2.3 唤醒后查表分流

`process_wakeup_event()` / [default_capture_callback()](../Custom/Services/System/system_service.c#L1163) 上游路径中：

```c
#define WAKE_TOLERANCE_SEC  60  // 容忍 RTC 抖动 + sleep_second 取整偏差

uint32_t wakeup_flag = 0;
u0_module_get_wakeup_flag(&wakeup_flag);

// 1) 非 RTC 唤醒走既有路径
if (wakeup_flag & (PWR_WAKEUP_FLAG_PIR_RISING | PWR_WAKEUP_FLAG_PIR_FALLING |
                   PWR_WAKEUP_FLAG_PIR_HIGH   | PWR_WAKEUP_FLAG_PIR_LOW)) {
    default_capture_callback(CAPTURE_TRIGGER_PIR, controller);
    return;
}
if (wakeup_flag & PWR_WAKEUP_FLAG_CONFIG_KEY) { /* 按键 既有路径 */ }
// ... WUFI / REMOTE ...

// 2) RTC 唤醒 → 本地查表确定 duty
if (wakeup_flag & (PWR_WAKEUP_FLAG_RTC_TIMING |
                   PWR_WAKEUP_FLAG_RTC_ALARM_A |
                   PWR_WAKEUP_FLAG_RTC_ALARM_B)) {
    uint64_t now = rtc_get_timeStamp();
    wake_event_t evs[4];
    int n = wake_scheduler_due_events(now - WAKE_TOLERANCE_SEC,
                                      now + WAKE_TOLERANCE_SEC,
                                      evs, 4);
    if (n == 0) {
        LOG_SVC_WARN("RTC wake but no schedule hit — possible clock drift");
        system_service_task_completed();
        return;
    }

    aicam_bool_t need_capture = AICAM_FALSE;
    aicam_bool_t need_flush   = AICAM_FALSE;
    for (int i = 0; i < n; i++) {
        if (evs[i].duty == WAKE_DUTY_UPLOAD_FLUSH) need_flush   = AICAM_TRUE;
        if (evs[i].duty == WAKE_DUTY_CAPTURE)      need_capture = AICAM_TRUE;
        wake_scheduler_mark_handled(evs[i].duty, evs[i].due_unix_sec);
    }

    if (need_flush && !need_capture) {
        // 纯定时上传：不抓拍，flush 完入睡
        upload_coordinator_kick();
        upload_coordinator_drain(30000);
        system_service_task_completed();
        return;
    }
    if (need_capture) {
        // 抓拍路径按 capture_upload.mode 决定是否同步上传；
        // 若 need_flush 同时为 true，coordinator 会在抓拍后顺手把 pending 一起 flush
        // （节点已 mark_handled，下次查表不会再触发）
        default_capture_callback(CAPTURE_TRIGGER_RTC, controller);
        return;
    }
}
```

#### 4.1.2.4 `last_handled_at` 持久化

防重复触发用 NVS 一条小 key（既有 `storage_nvs_write()`）：

```c
struct {
    uint32_t magic;                  // 0x57414B45 "WAKE"
    uint64_t capture_handled_at;     // 最近一次成功执行 capture duty 的 due_unix_sec
    uint64_t flush_handled_at;       // 最近一次成功执行 flush duty 的 due_unix_sec
} wake_state_nvs_t;
// NVS key: "wake/state"
```

`wake_scheduler_due_events()` 查表时跳过 `due_unix_sec <= *_handled_at` 的项，杜绝同一节点在 tolerance 窗口左右各被触发一次的边缘 case。

#### 4.1.2.5 工作模式 / 不进 sleep 的场景

- 全速模式 / 设备保持唤醒时：coordinator 自己起一个 ThreadX 软定时器，按 `wake_scheduler_next_event` 到点 kick，**不依赖 U0**。
- 唤醒后到下次 sleep 之间，如果 schedule 节点落在这段时间内：软定时器一并触发，行为与低功耗下一致。
- 入口都是 `wake_scheduler` + `upload_coordinator_kick()`，低功耗/全速两条路最终复用同一段处理逻辑。

#### 4.1.2.6 容差与边界

| 场景 | 行为 |
|---|---|
| PIR 在 schedule 节点前 30s 唤醒 | 走 PIR 抓拍路径，**不触发 flush**；节点的 RTC 唤醒在原定时间仍生效。若 PIR 处理跨过节点 → 入睡前 `due_events(0, now)` 兜底处理掉过期节点。 |
| RTC 大幅校时（NTP / Web 设时间） | 在校时回调里调 `wake_scheduler_reset_state()`，清掉 `*_handled_at`，按新时间重新调度。 |
| 多节点撞同一秒 | due_events 返回多个事件，按 duty 去重为两个布尔，逐一处理。 |
| sleep_second 受 U0 上限（如 24h） | next_event 加 horizon=24h，超过按 24h 睡，醒来重算 —— 与既有 timer_trigger 行为一致。 |
| 配置改了（Web 改 schedule） | `upload_coordinator_reload_config()` → `wake_scheduler_invalidate()`。设备此刻在睡眠 → 等本觉醒来生效。 |
| RTC 漂移 < 60s | tolerance 容忍。 |
| 时间表全空 | next_event 返回 0 → sleep_sec=0 → U0 走纯电平/PIR 唤醒，行为同既有"无定时任务"路径。 |

#### 4.1.2.7 与备份寄存器方案的对比（保留备查）

| 点 | BKP 方案（已弃） | 时间表查表方案（采用） |
|---|---|---|
| U0 固件 | 必须发版 + 协议升级 | **零改动** |
| ms_bridging 协议 | 加 OP code | 不动 |
| 区分依据 | 显式 intent 位图（精确） | 本地查表 + tolerance |
| RTC 校时干扰 | 不受影响 | 需 reset_state 配合（NTP 回调改一行） |
| 防重复触发 | 寄存器读完即清 | NVS `*_handled_at` |
| 调试 | 看 U0 寄存器 | 看 N6 日志和 NVS |
| 兼容旧 U0 | 需回退分支 | 不存在该问题 |

唯一实质代价：依赖 RTC 精度 + 一个 16 字节左右的 NVS 记录。在 U0/N6 RTC 已同步、有校时回调可挂钩的前提下，可接受。

<!-- 旧版本（BKP 方案，仅作记录）

#### 4.1.2.1 寄存器分配

[Custom/Hal/drtc.c](../Custom/Hal/drtc.c) 已用 `RTC_BKP_DR0` 存全局 magic（`RTC_BKP_FLAG = 0x5A5A5A5A`）做 RTC 已初始化标识。U0 上**新分配 1 个 RTC 备份寄存器（例如 `RTC_BKP_DR1`）作为 "wake intent" 位图**，由 N6 在进入睡眠前写、U0 在唤醒后读，读完即清。位定义（U0 与 N6 共用的头文件）：

```c
// 位图：可同时有多种意图（一次 RTC 唤醒可能同时撞上抓拍节点和上传节点）
#define WAKE_INTENT_NONE            0x00000000u
#define WAKE_INTENT_CAPTURE         (1u << 0)   // 定时抓拍（既有 timer_trigger）
#define WAKE_INTENT_UPLOAD_FLUSH    (1u << 1)   // 定时上传
#define WAKE_INTENT_MAGIC           0xA5000000u // 高字节做有效性 magic，避免冷启动脏数据被识别
#define WAKE_INTENT_MASK            0x00FFFFFFu // 低 24 位是真正的 intent 位
```

#### 4.1.2.2 N6 → U0：进入睡眠前下发 intent

进入睡眠之前，N6 已经在算 `sleep_second` / `alarm_a` / `alarm_b`（最近的 RTC 唤醒点是哪个）。在调用 `u0_module_enter_sleep_mode_ex()` 之前**先写一次备份寄存器**，告诉 U0「这一觉的下次 RTC 唤醒应该归类为哪种 intent」：

- 仅 timer_trigger 在最近 → 写 `WAKE_INTENT_CAPTURE | MAGIC`
- 仅 upload schedule 在最近 → 写 `WAKE_INTENT_UPLOAD_FLUSH | MAGIC`
- 两者在同一时刻 → 写 `WAKE_INTENT_CAPTURE | WAKE_INTENT_UPLOAD_FLUSH | MAGIC`，醒来一次顶两件事
- 没有任何 RTC 节点（纯 PIR/按键等待） → 写 `WAKE_INTENT_NONE` 即可

写入接口需要新增（不能复用 N6 侧 BKP，因为掉电后 N6 自己的 BKP 也丢；要落到 U0 端的 RTC BKP）：

```c
/// @brief Write next-wake intent to U0's RTC backup register
/// @param intent  bitmap of WAKE_INTENT_* flags (with MAGIC byte)
int u0_module_set_next_wake_intent(uint32_t intent);
```

实现走 [Custom/Common/Lib/ms_bridging/ms_bridging.c](../Custom/Common/Lib/ms_bridging/ms_bridging.c) 既有 request/ack 机制，在 ms_bridging 协议里加一个 OP code `MS_BR_OP_SET_WAKE_INTENT`。U0 firmware 收到后把 32 位值写入 `RTC_BKP_DR1`。

#### 4.1.2.3 U0：唤醒触发时翻译 intent → flag

U0 内部 RTC 中断 / 唤醒处理路径里，在原有的「设置 `RTC_TIMING` / `RTC_ALARM_A` bit」逻辑之后，额外读 `RTC_BKP_DR1`：

```c
uint32_t intent = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR1);
if ((intent & 0xFF000000u) == WAKE_INTENT_MAGIC) {
    intent &= WAKE_INTENT_MASK;
    if (intent & WAKE_INTENT_UPLOAD_FLUSH) {
        wakeup_flag |= PWR_WAKEUP_FLAG_UPLOAD_FLUSH;
    }
    // WAKE_INTENT_CAPTURE 不需要单独 bit —— 走既有 RTC_TIMING/_ALARM_x 即可表达
}
// 读完清掉，避免下次冷启动错认
HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR1, 0);
```

这样 U0 上报给 N6 的 wakeup_flag 同时包含 `RTC_TIMING + UPLOAD_FLUSH`（两者并不互斥）。

#### 4.1.2.4 N6：新 flag 定义和回调分支

[Custom/Hal/u0_module.h](../Custom/Hal/u0_module.h#L10) 增一行（位 14 未用，下一个 ROM 兼容位）：

```c
#define PWR_WAKEUP_FLAG_UPLOAD_FLUSH    (1 << 14)       // upload-flush wakeup (RTC + intent register)
```

[default_capture_callback()](../Custom/Services/System/system_service.c#L1163) 之上、`process_wakeup_event()` 中判断 wakeup flag 的位置（[system_service.c](../Custom/Services/System/system_service.c)），在 RTC 分支前加优先判断：

```c
if (wakeup_flag & PWR_WAKEUP_FLAG_UPLOAD_FLUSH) {
    LOG_SVC_INFO("Upload-flush wakeup — flushing pending uploads, no capture");
    upload_coordinator_kick();
    upload_coordinator_drain(30000);
    // 同一次唤醒如果还带 RTC_TIMING + WAKE_INTENT_CAPTURE，继续走抓拍 callback；
    // 否则直接 task_completed 入睡。
    if (!(wakeup_flag & (PWR_WAKEUP_FLAG_RTC_TIMING |
                          PWR_WAKEUP_FLAG_RTC_ALARM_A |
                          PWR_WAKEUP_FLAG_RTC_ALARM_B)) ||
        !timer_trigger_due_now()) {
        system_service_task_completed();
        return;
    }
    // fall-through to capture flow
}
```

#### 4.1.2.5 coordinator 侧的工作

`upload_coordinator` 维护 `schedule_minutes[]`，**它本身不调用 `system_controller_register_rtc_trigger()`**（那个是 timer_trigger 用的，会进抓拍 callback）。改为：

- 暴露 `upload_coordinator_get_next_wake_at(uint64_t now, uint64_t *out_unix_sec)`，返回最近的下一个 upload schedule 节点（或 0 表示无）。
- 入睡决策模块（在 `system_service_enter_sleep()` / quick_bootstrap 计算 `sleep_second` 的地方）调用：
  1. 既有逻辑算出 timer_trigger 下次时间 `t_cap`
  2. coordinator 算出下次 upload schedule 时间 `t_up`
  3. 取 `min(t_cap, t_up)` 作为实际的 `sleep_second` / `alarm_a`
  4. 根据 `t_cap == sleep_end` 与 `t_up == sleep_end` 计算 `intent = WAKE_INTENT_CAPTURE * (t_cap==end) | WAKE_INTENT_UPLOAD_FLUSH * (t_up==end) | MAGIC`
  5. `u0_module_set_next_wake_intent(intent)`，然后 `u0_module_enter_sleep_mode_ex(...)`

#### 4.1.2.6 工作模式 / 不进 sleep 的场景

- 全速模式 / 设备保持唤醒时：coordinator 自己起一个软定时器（ThreadX timer）到点 kick，不走 U0。`set_next_wake_intent` 不调用。
- 唤醒后到下次 sleep 之间，如果 schedule 节点在这段时间内：软定时器也会触发一次 flush（无论低功耗还是全速都走同一个上层入口）。

#### 4.1.2.7 兼容性

`PWR_WAKEUP_FLAG_UPLOAD_FLUSH` 是新增位（bit 14），未升级 U0 firmware 的设备永远不会置这个位，N6 端代码对该 flag 走 `if (...)` 旁路，无副作用。如果 N6 端配置开了 SCHEDULED 但 U0 是旧固件 → N6 侧能检测（首次 `set_next_wake_intent()` 返回 unsupported），在 Web UI 弹一次"需要升级协处理器固件 vX.Y 才能启用定时上传"的告警。

-->


### 4.2 上传协调器服务

新增文件：

- `Custom/Services/Upload/upload_coordinator.h`
- `Custom/Services/Upload/upload_coordinator.c`

#### 4.2.1 API

```c
aicam_result_t upload_coordinator_init(void *config);
aicam_result_t upload_coordinator_start(void);
aicam_result_t upload_coordinator_stop(void);
aicam_result_t upload_coordinator_deinit(void);
service_state_t upload_coordinator_get_state(void);

/// 拍完调用：把 record 落地并按模式调度上传
/// jpeg_buffer 在此函数返回前已被复制/接管，由 coordinator 在写盘后释放
aicam_result_t upload_coordinator_enqueue_capture(
    uint8_t *jpeg_buffer,            // primary jpeg
    uint32_t jpeg_size,
    uint8_t *inference_jpeg,         // 可空
    uint32_t inference_jpeg_size,
    const char *ai_result_json,      // 可空，所有权交出
    const mqtt_image_metadata_t *meta_in,  // 包含 trigger_type、timestamp 等
    aicam_capture_trigger_t trigger,
    wakeup_source_type_t wakeup_src,
    const battery_data_t *bat);

/// 主动触发一次「现在尝试上传所有 pending 记录」，可被外部调用：
/// - 即拍即传完成后立即调用一次
/// - 批量模式达到阈值
/// - 定时模式 RTC 触发
/// - 网络从断开恢复连接事件
aicam_result_t upload_coordinator_kick(void);

/// 配置变更后通知（重新加载 cfg，必要时重排定时任务）
aicam_result_t upload_coordinator_reload_config(void);

/// 让 coordinator 在睡眠前把当前一批 pending 处理完
aicam_result_t upload_coordinator_drain(uint32_t timeout_ms);

/// 查询状态（给 web /api/v1/capture/queue 用）
typedef struct {
    capture_mode_t mode;
    uint32_t pending_count;
    uint32_t sent_count;
    uint32_t failed_count;
    uint32_t local_count;
    uint64_t next_scheduled_at;     // 0 = N/A
    uint64_t bytes_used;
    uint64_t bytes_available;
    aicam_bool_t storage_full;
} upload_coordinator_status_t;
aicam_result_t upload_coordinator_get_status(upload_coordinator_status_t *out);
```

#### 4.2.2 内部线程模型

- 单独 ThreadX 任务 `upload_coordinator_task`（优先级低于 MQTT/Webhook，避免抢占）。
- 一个 osMessageQueue 接收事件：`KICK` / `RELOAD_CONFIG` / `SCHEDULE_TICK` / `DRAIN`。
- 一个 osTimer（基于 ThreadX timer）做定时上传调度，绑定到 `capture_upload.schedule_minutes[]`：每天最近的下一个节点。
- 重传退避：每个 record 在内存中维护 `next_retry_at`，列表按时间排序；任务 wake 时只看 `next_retry_at <= now` 的 record。

#### 4.2.3 上传循环（伪码）

```
for each *_m.json.pending in captures/ sorted by ts ascending:
    record = parse_meta(path)
    if record.retry_count >= cfg.retry_max_attempts:
        rename → *_m.json.failed; emit MQTT alarm; continue
    if !network_ready_for(record.upload_protocol):
        if drain_mode: break               // 整体退出
        wait_for_ready_or_timeout()        // 走 service_wait_for_ready
        if still not ready: requeue, break
    load primary_image / inference / ai
    result = do_upload(record)             // 见下
    if result == OK:
        rename → *_m.json.sent, retry_count=0
    else:
        record.retry_count++; record.next_retry_at = now + backoff(retry_count)
        write_meta_back()
```

`do_upload()` 根据 `record.upload_protocol` 选择：

- `MQTT`: 直接调用 `mqtt_service_publish_image_with_ai/_chunked`（保持 1MB 阈值切片）。
- `WEBHOOK`: 调用 `webhook_service_push_capture()`，由于现有 API 异步且占用 jpeg 所有权，需要：
  - 改造或新增同步版本 `webhook_service_push_capture_sync(timeout_ms)`（推荐方案：扩 API）；
  - 或保持异步并由 coordinator 等 `webhook_service_wait_pending()` 后查结果（需为单次 push 引入结果回调，见 §4.5）。

#### 4.2.4 清理与循环覆盖

- 进入写入前 `precheck_space()`：
  - 拿存储 `disk_file_stat` / `sd_get_disk_info / storage_get_disk_info` 计算 free。
  - 若 free < jpeg_size + 512KB：
    - `policy == WRAP`：调 `cleanup_oldest_until(jpeg_size + 512KB)`，优先级 `.sent > .local > .failed > .pending`。
    - `policy == STOP`：返回 `AICAM_ERROR_NO_SPACE`，调用方丢弃这次抓拍并报警。
- 后台周期任务（每 5 分钟）：删除 `.sent` 中 `now - ts > keep_sent_hours`。

#### 4.2.5 与现有 capture flow 集成

改造 [system_service_capture_and_upload_mqtt()](../Custom/Services/System/system_service.c#L3313)：

- 入口先读 `capture_upload_config_t`。
- **保留 `requires_time_optimized_mode == TRUE`（fast）分支的旧行为不变**：拍完直接 MQTT/Webhook，不进 coordinator，不持久化。⚠️ 项目规则。
- 非 fast 分支：
  - Step 1 拍到 jpeg / nn_result。
  - Step 2 组装 `mqtt_image_metadata_t`。
  - 如果 `storage == NONE` ＆ `mode == INSTANT`：直走旧的 Step 3.1/4/4.5（一次性同步上传），失败即丢。
  - 否则：调用 `upload_coordinator_enqueue_capture(...)`，jpeg 所有权交给 coordinator。coordinator 在内部写盘 + 决定是否立即 upload（`mode == INSTANT` → 直接调 `do_upload` 同步等结果；其它模式入队并 return OK）。
- 旧 Step 1.1（直接 `sd_write_file`）整段删除，被 coordinator 的落地逻辑取代。

修改 `wakeup_task_async()`：现在 `system_service_capture_request` 返回前 INSTANT 模式仍同步上传；其它模式提前返回 OK，由 sleep 流程在 `system_service_execute_pending_sleep` 之前 `upload_coordinator_drain(...)` 把这次的 record 处理完再睡。

#### 4.2.6 网络/连接事件钩子

- MQTT/HTTPClient 在连接成功时发 `upload_coordinator_kick()`（已有 service_set_ready_flag 钩子可挂）。
- WiFi/Halow 链路恢复事件同上。

### 4.3 cyclic_overwrite 接口收敛

[device_service_storage_set_cyclic_overwrite()](../Custom/Services/Device/device_service.c) 当前几乎空实现 → 转发到 `upload_coordinator_reload_config()` 即可，让所有删除/覆盖在 coordinator 中执行（单一处置点）。

### 4.4 Web API

新增 [Custom/Services/Web/api/api_capture_module.c/h](../Custom/Services/Web/api/)：

| Method | Path | 说明 |
|---|---|---|
| GET    | `/api/v1/capture/upload-config`  | 读取 `capture_upload_config_t`（JSON） |
| POST   | `/api/v1/capture/upload-config`  | 写入并热重载（`upload_coordinator_reload_config()`） |
| GET    | `/api/v1/capture/queue`          | 读取 coordinator 状态（pending/sent/failed counts、next_scheduled_at、storage 空间） |
| GET    | `/api/v1/capture/records`        | 列出 records（分页 / 状态过滤），用于「待上传 / 已上传 / 失败」三 tab |
| POST   | `/api/v1/capture/records/retry`  | 手动触发：对一个或全部 failed → 重置 retry_count + kick |
| DELETE | `/api/v1/capture/records`       | 删除指定 record 组（清空所有相关文件） |

请求/响应字段镜像 `capture_upload_config_t` + status 结构。校验：

- `storage == NONE` 必须 `mode == INSTANT`；否则 400。
- `mode == LOCAL_ONLY` 时 `upload_protocol` / `retry_enable` 忽略并回写规范化。
- `batch_count` 范围 1..50；`schedule_minutes` 元素 0..1439 且去重升序。

注册到 `web_api_register_capture_module()`，在 [web_server.c](../Custom/Services/Web/web_server.c) 的 `web_server_register_modules()` 中调用。

### 4.5 Webhook 服务最小改造

[webhook_service_push_capture()](../Custom/Services/Webhook/webhook_service.c#L306) 现在是「投队列即返回」，coordinator 需要一个「等本次推送结果」的回执：

方案 A（推荐）：扩出一个同步版本

```c
aicam_result_t webhook_service_push_capture_sync(
    const uint8_t *jpeg_data, uint32_t jpeg_size,
    const mqtt_image_metadata_t *metadata,
    const mqtt_ai_result_t *ai_result,
    uint32_t timeout_ms);
```

内部仍走同一个 push_task，但带 `osEventFlags` 通知完成 + 状态。

### 4.6 服务注册

[service_init.c](../Custom/Services/service_init.c)：在 webhook 之后插入 upload_coordinator 注册，确保启动顺序：`storage → device → mqtt → webhook → upload_coordinator → system`。

---

## 5. 前端实现

### 5.1 菜单 + 路由

- [Web/src/layout/pc/menu.tsx](../Web/src/layout/pc/menu.tsx)：在 `hardware-management` 与 `system-settings` 之间插入：
  ```ts
  { path: '/capture-settings', key: 'sys.menu.capture_settings' },
  ```
- 同步移动版菜单 [Web/src/layout/mobile/nav-right.tsx](../Web/src/layout/mobile/nav-right.tsx) 中对应顺序。
- [Web/src/router/index.tsx](../Web/src/router/index.tsx)：新增 `/capture-settings` → `CaptureSettings` 页面组件。

### 5.2 新页面 `Web/src/pages/captureSettings/`

- `index.tsx`：Tab 容器（`config` / `records`），默认 `config`。
- `config.tsx`：抓拍设置表单（详见 §5.3）。
- `records.tsx`：抓拍记录列表（pending / sent / failed），调用 `/api/v1/capture/records` + `retry` + `DELETE`。
- 抓拍硬件参数（跳帧、分辨率、JPEG 质量、capture_storage_ai）整段从 `hardwareManagement/graphics.tsx` 迁过来作为「采集参数」子分组，沿用旧 API `/api/v1/device/image/config`。

### 5.3 抓拍设置表单（联动规则）

| 字段 | 控件 | 仅在以下条件可见 | 仅在以下条件可改 | 提示（i18n） |
|---|---|---|---|---|
| **抓拍模式 mode** | Radio/Select：即拍即传/批量/定时/只存不传 | 始终 | 始终 | 「即拍即传：唤醒后立刻上传，失败可走重传；批量：积累到 N 张统一传；定时：每天指定时间点一次性上传；只存不传：完全离线，仅保存本地」 |
| **保存位置 storage** | Select：自动/内部flash/SD卡/不保存 | 始终 | SD 选项需 `sd_card_connected == true`；「不保存」仅 `mode == INSTANT` 才可选 | 「自动选择会优先使用 SD 卡，未就绪时回退内部 flash」 |
| **存储策略 policy** | Select：循环覆盖 / 存满即停 | `storage != NONE` | 同上 | 「循环覆盖会优先删除已上传的最旧文件」 |
| **上传协议 upload_protocol** | Select：MQTT / WebHook | `mode != LOCAL_ONLY` | 同上 | 「MQTT 走默认数据上报主题；WebHook 走系统设置-WebHook 中配置的 URL」 |
| **批量阈值 batch_count** | Input 1..50 | `mode == BATCH` | 同上 | 「达到该数量后触发一次集中上传」 |
| **定时节点 schedule_minutes[]** | 多 TimePicker + 增删按钮 | `mode == SCHEDULED` | 同上 | 「每天到达任一节点时间，设备会单独唤醒一次（不抓拍），只把队列里 pending 的图片上传完再睡。每个节点会增加一次唤醒功耗，请按需配置。本设置与「功能调试 → 唤醒源 → 定时抓拍」是两套独立时间表，互不替代。」 |
| **重传开关 retry_enable** | Switch | `mode != LOCAL_ONLY && storage != NONE` | 同上 | 「失败后会在下次上传节点重新尝试，最多 N 次」 |
| **重传次数上限** | Input 1..20 | `retry_enable == true` | 同上 | 「每张图片的最大尝试次数，达到后标记为失败」 |
| **重传退避（秒）** | Input 5..3600 | `retry_enable == true` | 同上 | 「下次尝试的最小间隔，会随失败次数指数倍增」 |
| **保留已上传时长（小时）** | Input 0..720 | `storage != NONE` | 同上 | 「0 表示上传成功后立即删除文件」 |

DSL 校验：保存按钮 disabled until 校验通过；后端再次校验（信任边界）。

错误/异常态：保存位置切到 SD 时若 `sd_card_connected == false` 主动 toast 警告并禁用提交。

### 5.4 API 客户端

新增 [Web/src/services/api/captureSettings.ts](../Web/src/services/api/captureSettings.ts)：

```ts
export type CaptureMode = 'instant' | 'batch' | 'scheduled' | 'local_only';
export type CaptureStorage = 'auto' | 'flash' | 'sd' | 'none';
export type StoragePolicy = 'wrap' | 'stop';
export type UploadProto = 'mqtt' | 'webhook';

export interface CaptureUploadConfig {
  mode: CaptureMode;
  storage: CaptureStorage;
  policy: StoragePolicy;
  upload_protocol: UploadProto;
  retry_enable: boolean;
  retry_max_attempts: number;
  retry_backoff_sec: number;
  batch_count: number;
  schedule_minutes: number[];
  keep_sent_hours: number;
  max_pending_records: number;
}
// ... and queue/records types
```

### 5.5 i18n

`Web/src/locales/{zh,en}/sys.json` 新增 namespace `capture_settings`（含上述全部字段 + tooltip）。

`menu.capture_settings`: `"抓拍设置" / "Capture Settings"`。

旧 `hardware_management.capture_config*` / `fast_capture_*` / `capture_storage_ai` 等 key 大部分被搬到新页面，但为避免破坏其它引用，**保留 key 不删除**，仅更新文案。

### 5.6 旧页面 graphics.tsx 调整

把抓拍片段（[graphics.tsx#L618-L818](../Web/src/pages/hardwareManagement/graphics.tsx#L618-L818)）整段移除（迁移到 captureSettings）。`buildImageConfigRequest()` 继续传 `fast_capture_*`，因为这些是 fast-capture 路径专用参数（与硬件相关），保留在「采集参数」分组下。

---

## 6. 任务拆解与里程碑

| # | 任务 | 涉及文件 | 验证方式 |
|---|---|---|---|
| 1 | 配置 schema 扩展 + getter/setter | json_config_mgr.{h,c} | 启动加载/保存 round-trip，旧版本自动迁移 default |
| 2 | `upload_coordinator` 落盘逻辑（命名/状态/metadata） | upload_coordinator.{h,c} | 单测：模拟拍一张 → captures/ 下出现完整 4 文件，状态后缀正确 |
| 3 | `precheck_space` + cleanup（WRAP / STOP） | 同上 | 模拟存满，验证两种策略落点 |
| 4 | `do_upload` 走 MQTT 路径 | upload_coordinator + mqtt | 拔网线模拟失败 → `.pending` retry_count 自增；连网后 kick → `.sent` |
| 5 | Webhook sync API + coordinator 接入 | webhook_service.{h,c} | 服务器返回 5xx 时重试，2xx 时 `.sent` |
| 6 | scheduler（定时上传） | upload_coordinator | 设置 5 分钟后节点，等到点触发批量上传 |
| 7 | 批量上传阈值 | upload_coordinator | enqueue N-1 时静默，第 N 张时 kick |
| 8 | system_service 改造（删旧 SD 写入；INSTANT/non-INSTANT 分流；drain 接 sleep） | system_service.c | RTC 唤醒 → 抓拍 → drain → 入睡，下一次唤醒能继续上传上次失败的 |
| 8.5 | **wake_scheduler 模块**：合并 `timer_trigger` + `capture_upload.schedule_minutes[]`，提供 `next_event` / `due_events` / `mark_handled` / `reset_state` API；接入入睡前 sleep_second 计算 & 唤醒后查表分流；NVS `wake/state` 持久化；NTP/Web 校时回调里挂 `reset_state` | wake_scheduler.{h,c}, system_service.c, NVS, RTC 校时回调 | 配置 SCHEDULED + 节点 → 入睡 → 节点到点 → U0 RTC 唤醒 → N6 查表识别 UPLOAD_FLUSH duty → 不抓拍只 flush → 入睡。同时配 timer_trigger 撞同一时刻 → 查表两个 duty 都命中 → flush + 抓拍。重启/校时后行为正确，不重复触发也不漏触发 |
| 9 | Web API module + 注册 | api_capture_module.{h,c}, web_server.c | curl 验证 GET/POST/DELETE |
| 10 | Web 菜单 + 路由 + 新页面 + 配置表单联动 | layout, router, pages/captureSettings/* | UI 校验所有联动规则 |
| 11 | Web 抓拍记录页 | pages/captureSettings/records.tsx | 列出 / 手动重试 / 删除 |
| 12 | i18n 中英文文案 | locales/{zh,en}/sys.json | 所有 tooltip/label 显示正确 |
| 13 | 旧 graphics.tsx 抓拍段清理 | pages/hardwareManagement/graphics.tsx | 旧入口不再可见 |
| 14 | 文档：协议字段、状态机示意 | Docs/CaptureUpload.md | code review |

里程碑：
- **M1**（Backend MVP）：1–4，能即拍即传 + 重传 + .pending/.sent 状态正确。
- **M2**（Modes）：5–8，全部四种模式可用。
- **M3**（Web）：9–13，端到端可视化。
- **M4**：14 + 集成回归测试。

---

## 7. 边界情况与风险

1. **休眠唤醒丢上传**：RTC/PIR 唤醒拍完，coordinator 异步上传可能未完就睡。解决：sleep 流程前强制 `upload_coordinator_drain(TIMEOUT)`，timeout 内未传完则保持 `pending/` 等下次唤醒再传（这本就是重传机制要解决的情况）。已有 `webhook_service_wait_pending()` 思路可对齐。
2. **定时抓拍 vs 定时上传混淆**：见 §4.1.1 / §4.1.2。两者**两条独立的时间表，但共用同一类 RTC 唤醒**。N6 在入睡前用 `wake_scheduler` 把两个时间表合并，取最近事件算 `sleep_second`；唤醒后用当前时间 + tolerance 查表确定本次是 capture / flush / 两者都是。**不改 U0、不改 ms_bridging 协议**，靠 NVS 中的 `*_handled_at` 防重复触发。注意：RTC 校时后必须调 `wake_scheduler_reset_state()` 清状态。
2. **fast-capture 与新机制混跑**：fast 分支不进 coordinator，但下一次常规唤醒会扫到上次 fast-capture 残留？**不会**，fast-capture 不落地。✅
3. **存储介质中途插拔 SD**：`storage == AUTO` 在每次 enqueue 时重新选择目标，已有 record 留在原存储；coordinator 扫描时遍历两个 FS（先 SD 后 flash）即可。
4. **配置写入触发不一致**：用 `upload_coordinator_reload_config()` 在 setter 完成后 publish，避免锁。
5. **OTA 期间**：现有 `ota_is_upload_in_progress()` 已挡在 capture_request 入口；coordinator 自己的上传循环也要在主循环里 check。
6. **文件名 unix_ts 在没 RTC 校时前可能为 0/不准**：fallback 用 `rtc_get_uptime_ms()` + 单调序号；metadata 同步记录 `boot_offset_ms`，校时后可纠正排序。
7. **Web 上传记录列表的目录扫描成本**：单目录 `captures/` 上限 1000 条左右；超过用「分页 + 仅扫 metadata 文件」减负。
8. **fast_fail_mqtt_policy**：现有 INSTANT 走 fast-fail，对 coordinator 透明，仍可保留作为「INSTANT 模式遇 MQTT 不通则直接降级到走重传队列」。

---

## 8. 全局约束记忆（写入 memory）

> ✅ 已记录到全局 memory：`feedback_fast_capture_path` — fast-capture 路径只服务于 boot-time RTC 唤醒快拍 + AI 推理，**不重构其上传逻辑**，新机制不影响该路径。

---

## 9. 一句话总结

新增一个 `upload_coordinator` 服务承载「落盘 + 状态机 + 调度 + 重传 + 清理」，
[system_service_capture_and_upload_mqtt()](../Custom/Services/System/system_service.c#L3313) 改成「拍 + 委托」，
Web 端独立菜单「抓拍设置」给完整可视化，
fast-capture 路径原封不动。
