# 抓拍上传网络优化

> 配套改动:指定上传网卡 + 网卡全失败快速通知。降低休眠唤醒抓拍的网络初始化耗时与功耗。

## 背景

休眠唤醒抓拍时,原逻辑会同时初始化多个网卡(WiFi / HaLow / 4G / PoE),再按优先级决策选用哪个,平均多耗数秒、功耗高。且当网卡全部失败(如选 4G 但无 SIM)时,上传任务会死等 30 秒超时才走失败路径,浪费时间与电量。

本次优化:
1. 抓拍配置新增「上传网络」项,可选「默认」或指定一个网卡。指定后,唤醒抓拍路径**只初始化该网卡**。
2. 当(被尝试的)网卡全部失败时,**立即通知**等待中的上传任务,秒级走失败路径,不再死等超时。

## 配置项

`capture_upload_config_t.upload_comm_type`(uint32,值为 `communication_type_t`):

| 值 | 字符串 | 含义 |
|----|--------|------|
| `COMM_TYPE_NONE`(0) | `"default"` | 默认:沿用系统「通讯方式」逻辑,初始化多网卡按优先级 |
| `COMM_TYPE_WIFI`(1) | `"wifi"` | 唤醒抓拍只初始化 WiFi STA |
| `COMM_TYPE_HALOW`(2) | `"halow"` | 只初始化 Wi-Fi HaLow |
| `COMM_TYPE_CELLULAR`(3) | `"cellular"` | 只初始化 4G |
| `COMM_TYPE_POE`(4) | `"poe"` | 只初始化 PoE/以太网 |

- 持久化于 NVS key `cu_comm`(uint8)。
- Web API:`GET/POST /api/v1/capture/upload-config` 的 `upload_network` 字段。
- 前端:抓拍配置页「上传网络」下拉,首项「默认」,其余项**动态取自设备已接入网卡**(`GET /api/v1/system/network/status` 的 `available_comm_types`,与系统设置「通讯方式」同源,后端已按硬件存在性过滤)。

## 行为

### 默认(上传网络 = 默认)
保持原逻辑不变:唤醒抓拍时 `communication_service_start()` 按 `auto_start_*` 启动多网卡,`make_startup_connection_decision()` 按优先级决策。全速启动(AP/web)亦不受影响。

### 指定网卡
仅低功耗唤醒抓拍路径生效(条件:`POWER_MODE_LOW_POWER` + `essential_only` + 本次抓拍需要网络)。此时:
- `service_start()` 调用 `upload_coordinator_get_required_comm_type()` 取配置,非 NONE 则 `communication_service_set_required_type(type)`。
- `communication_service_start()` 检测到 `required_type != NONE`,只 `netif_init_manager_init_async(对应 if_name)`,跳过其余网卡。
- type→netif 名映射:`netif_name_for_comm_type()`(WIFI→"wl",HALOW→"hw",CELLULAR→"4g",POE→"wn")。

全速启动(长按进 AP、web/OTA/流)不调用 setter,所有网卡正常初始化。

## 失败快速通知机制

新增 ready 标志 `SERVICE_READY_NETIF_ALL_FAILED`(bit 14,定义于 `service_init.h`)。

**触发**:在唤醒单网卡路径下,被指定的网卡初始化回调(`on_wifi_sta_ready` / `on_halow_ready` / `on_cellular_ready` / `on_poe_ready`)收到 `result != AICAM_OK` 时,经 `comm_check_required_netif_failed()` 调 `service_set_netif_all_failed(TRUE)` 置位该标志。

**消费**:`upload_coordinator` 的网络等待(INSTANT+NONE 直传路径 与 `upload_one_record` 持久化上传路径)改为:
```
service_wait_for_ready(SERVICE_READY_STA | SERVICE_READY_NETIF_ALL_FAILED, wait_any, 30s)
if (flags & NETIF_ALL_FAILED) → 立即返回失败(落 pending 重试 或 丢弃)
否则链路已就绪 → 继续 publish + 等 PUBACK
```

标志位基于每次启动新建的 `g_service_ready_flags` 事件组,冷启动天然复位,无需手动清。

## 时序

```
唤醒(按键/RTC/PIR) → 冷启动 → service_init
  ├─ upload_coordinator_needs_network() = YES
  ├─ essential_only = true, LOW_POWER
  ├─ upload_coordinator_get_required_comm_type() = CELLULAR  (示例)
  ├─ communication_service_set_required_type(CELLULAR)
  └─ service_start() → communication_service_start()
       └─ 只 init "4g" 网卡 (跳过 wifi/halow/poe)
            ├─ 成功 → on_cellular_ready(OK) → 链路就绪设 STA
            └─ 失败 → on_cellular_ready(ERR) → 设 NETIF_ALL_FAILED

process_wakeup_event → capture → upload_coordinator_enqueue_capture
  └─ 等待 (STA | NETIF_ALL_FAILED)
       ├─ STA 置位 → 上传
       └─ ALL_FAILED 置位 → 秒级失败路径 → 睡眠
```

## 注意事项

- **全速启动不受影响**:AP/web/OTA 场景所有网卡照常初始化,`upload_comm_type` 仅在低功耗唤醒抓拍路径生效。
- **网卡硬件不存在时选了该网卡**:唤醒时该网卡 init 失败 → 走失败快速通知路径(行为合理,不做额外回退到其他网卡,以尊重用户显式选择)。
- **配置热重载**:web 修改 `upload_network` 后 `upload_coordinator_reload_config()` 刷新缓存,**下次唤醒**生效;当前唤醒周期已启动的网卡不受影响。
- **LOCAL_ONLY 模式**:`upload_coordinator_get_required_comm_type()` 返回 `COMM_TYPE_NONE`(不上传,不需要限制网卡)。
- 文档与代码同步:本文件描述的标志位、setter、回调点见改动清单各文件。
