# 快速抓拍优化方案

## 背景

现有系统的抓拍流程存在延时以及功耗太高等问题，主要原因在于抓拍流程依赖核心层（core_system）和服务层（service），需要等待它们就绪后才开始抓拍，这两层初始化就需要快一秒的时间，然后抓拍流程只做了网络和抓拍并行，效率和速度较差，导致耗时长，功耗高。

## 约束

* 方案的所有流程在核心层、服务层都能找到对应的代码参考，尽量不要自己捏造；
* 方案内涉及到一个阶段性任务完成需要通知其它线程时，可以通过 Event 事件标志位实现；对**一次性**阶段完成标志，等待时应选用**不清除**（如 CMSIS-RTOS2 `osFlagsNoClear`），或由单一收口处在所有相关方读完后**再**显式清除。若首位等待者在返回时把标志**自动清掉**，后续线程或再次 `Wait` 同一标志时会永远等不到，表现为**一直卡在等待**；多消费者场景可改用信号量、条件变量，或「只许一方 wait+clear、其余读内存状态」等模式。
* 最终效果需要和之前流程对齐，比如上传的 JSON 格式、分包上传等待这些（应用侧可参考 `communication_service` / MQTT 上报路径，配置字段与 `json_config_mgr.h` 中 `mqtt_service_config_t` 等结构对齐）；
* JPEG 编码拿到结果后要用 `JPEGC_CMD_UNSHARE_ENC_BUFFER` 将结果从编码器侧「独立」出来，避免其它路径继续使用 JPEG 编码共用缓冲；使用完毕后再 `JPEGC_CMD_FREE_ENC_BUFFER` 释放。定义见 `Custom/Hal/jpegc.h`，实现见 `Custom/Hal/jpegc.c` 中对应 `case`；
* **AI 管道宽高**（NVS 键见 `json_config_internal.h` 中 `NVS_KEY_AI_PIPE_WIDTH` / `NVS_KEY_AI_PIPE_HEIGHT`）：若从 NVS 读不到有效值，须**先等 AI 模型信息就绪**（如 `quick_snapshot_wait_ai_info` / `nn_model_info_t`），再按模型输入尺寸配置 AI 管道，禁止在信息未就绪时用默认值硬配。
* 现有代码已经有部分模块头文件实现了，需要学习它们的风格，视需求完善补充它们。

## 当前仓库状态（实现进度）

| 组件 | 头文件 | 源文件 | 说明 |
|------|--------|--------|------|
| `quick_storage` | `quick_storage.h` | `quick_storage.c` 当前为空占位 | 已定义 NVS 读取 API、写任务队列参数、`qs_comm_pref_type_t` 等 |
| `quick_network` | `quick_network.h` | `quick_network.c` 当前为空占位 | 已定义 init / wait_config / MQTT 任务投递 / 远程唤醒切换 |
| `quick_snapshot` | `quick_snapshot.h` | `quick_snapshot.c` 当前为空占位 | 已定义 init、等待配置、主帧、JPEG、AI info/result |
| `quick_bootstrap` | `quick_bootstrap.h` 当前为空 | `quick_bootstrap.c` 当前为空占位 | 对外统一入口接口尚未在头文件中固化，需在实现前补全声明 |

上述 `.c` 尚未加入工程 `Makefile`（在仓库内检索 `quick_` 无编译条目），也未接入构建；落地时需要把源文件加入 Appli（或等价）构建。

**应用侧计划入口**：`Custom/Hal/driver_core.c` 中，在 `camera_register()` / `jpegc_register()` 之后、`ENABLE_U0_MODULE` 分支里读取 `u0_module_get_wakeup_flag()` 的位置即为快速抓拍流程入口（注释已标明 Quick_Bootstrap）。`driver_core_init` 里紧随其后的 `draw_register()`、`nn_register()` 等仍面向常规全功能路径；**画图与模型推理**在快速抓拍的具体实现中应按 `qs_snapshot_config_t`（AI 开关等）**按需注册并在阶段结束后销毁**，勿默认等同全量初始化链路。

## 头文件 API 与数据模型（与代码一致）

以下便于对照实现，避免与已写头文件脱节。

**`quick_storage.h`**

* 配置结构：`qs_snapshot_config_t`（AI 开关与阈值、灯控、镜像/翻转、快拍跳帧/分辨率/质量等）、`qs_work_mode_config_t`（图像/视频模式占位、PIR/定时/远程触发等）、`qs_mqtt_all_config_t`（topic、QoS、内嵌 `ms_mqtt_config_t`）。
* 网络：`qs_comm_pref_type_t`（`AUTO` / `WIFI` / `CELLULAR` / `POE` / `DISABLE`）、`quick_storage_read_netif_config(..., netif_config_t *)`（类型来自 `netif_manager.h`）。
* WiFi：`qs_wifi_network_info_t` 与 `quick_storage_read_known_wifi_networks`（最多 `MAX_KNOWN_WIFI_NETWORKS`）。
* 异步写盘：`quick_storage_init`、`quick_storage_add_write_task`（`qs_write_task_param_t`：`append/overwrite`、文件名、`data`/`data_len`、完成回调）。

**`quick_network.h`**

* `quick_network_init`、`quick_network_wait_config`（输出最终采用的 `qs_comm_pref_type_t`）。
* `quick_network_add_mqtt_task`（`qs_mqtt_task_param_t`：负载与发送完成回调）。
* `quick_network_switch_remote_wakeup_mode`（注释：当前仅 WiFi 通信场景）。

**`quick_snapshot.h`**

* 依赖 `quick_storage.h`、`nn.h`：`quick_snapshot_init`、`quick_snapshot_wait_config`、`quick_snapshot_wait_capture_frame`、`quick_snapshot_wait_capture_jpeg`、`quick_snapshot_wait_ai_info`、`quick_snapshot_wait_ai_result`。

**`quick_bootstrap`**

* 头文件待补充：建议在此声明「单次快拍流水线」入口（例如 `quick_bootstrap_run(void)` 或带唤醒原因参数），内部顺序与下文「方案」章节一致。

## 方案

`Custom/Hal/Quick_Bootstrap` 将在这个目录下实现一套尽量不依赖核心层与服务层完整启动链路的快速抓拍流程（底层仍复用已存在的 HAL：`camera`、`jpegc`、`nn`、`netif_manager`、MQTT 客户端等），具体分为以下几个模块：

1. **`quick_storage`**：提供一系列将 NVS 配置（实现时对照服务层/系统层的 `Custom/Core/System/json_config_mgr.h` 及 `json_config_get_*` 系列 API）读取到本目录头文件中已声明结构体的能力；`quick_storage_read_snapshot_config` 等以 **json_config / NVS** 为数据源即可。**按需读取**：若某模块在配置中为 disable，则跳过后续相关配置块与线程，减少 flash 访问与内存。`quick_storage_init` 后内部维护**存储线程**，通过 `quick_storage_add_write_task` 异步写入 JPEG、AI JSON 等，不阻塞抓拍路径。
2. **`quick_network`**：初始化后规划**网络线程**与 **MQTT 线程**（可与现有 `netif_manager` 的建链流程对齐，而非重复造轮子）。网络线程从 `quick_storage` 读取 `comm_pref_type`：`COMM_PREF_TYPE_DISABLE` 则直接结束；`COMM_PREF_TYPE_AUTO` 则按 **ETH → 4G → WiFi** 优先级探测；确定类型后再 `quick_storage_read_netif_config` 取详细参数并建链，成功后通过**信号量或事件**通知 MQTT 线程；MQTT 侧 `quick_storage_read_mqtt_all_config` 初始化客户端，等待网络就绪后处理 `quick_network_add_mqtt_task` 队列。
3. **`quick_snapshot`**：抓拍线程从 `quick_storage_read_snapshot_config` 取参；按配置决定是否拉起 AI 线程；**AI 管道宽高**：若 NVS 已给出有效宽高则直接用于管道配置；否则先走模型加载，待 **`nn_model_info_t` 就绪**后再配置 AI 管道，再进入取推理帧等步骤。灯、相机按配置开关；取帧阶段若开启 AI 则同步取推理帧并通知 AI 线程；**取帧结束后尽快停传感器/管道**以降低功耗；主流 YUV 再送 `jpegc` 编码。AI 线程：加载模型、对外通过 `quick_snapshot_wait_ai_info` 暴露模型信息、等待推理帧、推理结束后释放 NPU/模型相关资源。通过 `quick_snapshot_wait_*` 与 bootstrap 侧**同步汇合**（可用事件组表示「JPEG 已就绪」「AI 已就绪」等）。
4. **`quick_bootstrap`**：对外主入口模块。依次 `quick_storage_init` → `quick_network_init` → `quick_snapshot_init`（顺序可按依赖微调）；`quick_storage_read_work_mode_config`；等待 `quick_snapshot_wait_config`；再按配置等待 JPEG 与 AI（`quick_snapshot_wait_capture_jpeg` / `quick_snapshot_wait_ai_result`）。汇合逻辑与原文一致：AI 结果先到则先组结构化 JSON 并 `quick_storage_add_write_task`，并暂存上传用片段；JPEG 先到则先落盘再 base64（若需要）；齐套后组**与现网一致的上报 JSON**，`quick_network_add_mqtt_task` 发送；若 `quick_network_wait_config` 表明不进行网络传输则跳过 base64 与上报 JSON 构建。若 SD 可用且存在 AI 结果，可再次取帧、画框、再编码 JPEG 并异步写入。最后等待写盘与 MQTT 任务完成，按工作模式调用电源模块（`Custom/Hal/pwr.c` 等现有接口）配置睡眠。

## 建议对齐的现有代码索引（实现时查阅）

| 能力 | 建议入口/文件 |
|------|----------------|
| JSON / NVS 业务配置 | `Custom/Core/System/json_config_mgr.h`（如 `json_config_get_mqtt_service_config`、快拍相关字段） |
| 设备侧相机/灯/抓图编排 | `Custom/Services/Device/device_service.c`（与 `camera`、`jpegc`、`nn` 的交互方式） |
| 网络栈与 netif | `Custom/Hal/Network/netif_manager/`、`netif_config_t` |
| MQTT | `ms_mqtt_client`（`qs_mqtt_all_config_t` 已内嵌 `ms_mqtt_config_t`） |
| JPEG 缓冲生命周期 | `Custom/Hal/jpegc.h` / `jpegc.c`：`JPEGC_CMD_UNSHARE_ENC_BUFFER`、`JPEGC_CMD_FREE_ENC_BUFFER` |

## 集成清单（落地时）

* [ ] 将 `quick_*.c` 加入 Appli（或子项目）`Makefile` / `SOURCES`。
* [ ] 在合适的线程或冷启动路径调用 `quick_bootstrap` 入口（避免与完整 `driver_core_init` 中尚未就绪的模块循环依赖）。
* [ ] 补全 `quick_bootstrap.h` 中对外 API，与电源/唤醒（如 `u0_module` 唤醒标志）的衔接在入口参数或内部读取处写清。
* [ ] 单元/台架：验证禁用网络、仅 SD、仅 MQTT、AI 开/关等分支与旧上报 JSON 一致。
