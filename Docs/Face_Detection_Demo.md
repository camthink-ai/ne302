# NE302 人脸检测 Demo（Face Detection）

分支：`feature/face-detection-demo`

设备端实时人脸检测 + 跟踪，按 Web 配置对人脸做马赛克模糊；按外扩配置从 720p 原图（PIPE1）裁剪人脸小图，经 MQTT 或 Webhook 上报给后端人脸识别应用；后端返回的识别结果按 track ID 关联后，同步叠加显示在 Web 实时画面上（框、track ID、检测置信度、识别姓名与置信度）。

## 一、已确认的需求决策

| # | 决策点 | 结论 |
|---|--------|------|
| 1 | 上报通道 | MQTT 与 Webhook 都支持，**同时只有一种在工作**（Web 单选切换） |
| 2 | 后端协议 | 未定，由本仓库定义（见第五节），并提供模拟后端脚本 `Script/face_demo_backend.py` |
| 3 | 上报时机 | 新人脸（新 track）出现**必须先上报一次**；是否周期性重报及冷却时间由 Web 配置 |
| 4 | 外扩配置 | **统一比例**（四边同比例外扩）；超出画面默认 **clamp 截断**（不填充） |
| 5 | 模糊方式 | 马赛克（块大小可配），不需要高斯模糊 |
| 6 | 模型 | 单模型（人脸检测），识别在云端；**不动现有程序架构**（沿用单模型 AI 管线） |
| 7 | 上报图片 | 分辨率与质量 Web 可配；**默认为外扩裁剪后的原始尺寸**；可切换为固定分辨率（正方形边长，最大 512×512），**先等比缩放再填充（letterbox），不拉伸** |

其他要点：

- 裁剪必须从 **PIPE1（1280×720 原图）** 进行，保证上报清晰度（AI 检测在 PIPE2 小图上，坐标需映射回 PIPE1）。
- 需兼容**无关键点**的人脸模型（上报载荷 `landmarks` 为 `null`，前端不绘制关键点）。
- 默认模型：ST model zoo 的 **YuNet**（`face_detection/yunet`，320×320，ONNX QDQ int8，自带 5 关键点），已加入仓库并设为 `make flash-model` 默认（见第三节）。

## 二、功能总览

| 功能 | 说明 |
|------|------|
| 实时人脸检测 | PIPE2（≤480×480）推理，检测框/置信度/关键点映射回 PIPE1 720p 坐标 |
| 人脸跟踪 | IoU 跟踪器为每张人脸分配持久 track ID（阈值可配），兼容无关键点模型 |
| 人脸马赛克 | 可开关；块大小与模糊区域外扩比例可配；在 PIPE1 流上先模糊后画框 |
| 外扩裁剪上报 | 统一比例外扩 + clamp；从 PIPE1 裁剪，默认原始尺寸，可切换固定边长（≤512，缩放+填充） |
| 上报 | MQTT / Webhook 二选一（互斥）；新 track 必报，周期重报与冷却可配；置信度阈值独立于显示阈值 |
| 识别结果回传 | Webhook 同步 HTTP 响应 / MQTT 结果 topic 下发；按 request_id↔track_id 关联，带超时与过期 |
| 前端叠加 | 框 + `#ID 置信度` + 识别姓名与置信度（未出结果"识别中"、失败"未知"）；边缘标签自适应换位 |

## 三、默认模型（YuNet）准备与烧录

模型来自 [stm32ai-modelzoo face_detection/yunet](https://github.com/STMicroelectronics/stm32ai-modelzoo/tree/main/face_detection/yunet)（WIDER FACE 训练，int8，5 关键点，N6 上约 6.8ms/帧）。

仓库已包含：

- `Model/weights/yunetn_320_qdq_int8_rgb_fd_ui.onnx` — QDQ int8 权重，**输入已转为 RGB 通道序**（见下）
- `Model/weights/yunetn_320_qdq_int8_rgb_fd_ui.json` — 部署配置（`postprocess_type: pp_fd_yunet_ui`）
- 后处理 `pp_fd_yunet_ui` 已注册（`Custom/Common/Lib/pp/pp_fd_yunet_ui.c`），本次为其新增了 `nb_detections_8/16/32` 的 JSON 覆盖支持（320×320 输入对应 1600/400/100，默认值 2688/672/168 是其他输入尺寸的，必须覆盖）
- `Model/Makefile` 默认 `MODEL_NAME` 已切到 `yunetn_320_qdq_int8_rgb_fd_ui`，并支持 `.onnx` 权重自动探测

```bash
# 构建 + 打包 + 烧录到 AI_1 (0x70900000)，无需参数
make flash-model

# 或分步
make model        # ST Edge AI 编译 + 打包 -> build/ne302_Model.bin
make pkg-model    # 加 OTA 头 -> build/ne302_Model_v<版本>_pkg.bin

# 批量脚本方式（产出 Model/build/models/yunetn_320_qdq_int8_rgb_fd_ui_v<版本>_pkg.bin）
bash Script/build_all_models.sh yunetn_320_qdq_int8_rgb_fd_ui
```

> `_pkg.bin`（含 1024 字节 OTA 头）烧到分区起始地址 `0x70900000`；裸包（packager 直接产物）需 `+1024` 即 `0x70900400`。单模型 demo 使用当前激活 AI 槽位，烧到 AI_1 后请确认 `ai_1_active` 指向 AI_1（模型管理页切换或 `/api/v1/model/reload`）。

### 集成注意事项（YuNet 特有）

0. **两个已修复的 pp 缺陷**（`pp_fd_yunet_ui` 此前从未真正在设备上运行过）：
   - `Custom/Hal/nn.h` 的 `NN_MAX_OUTPUT_BUFFER` 原为 5，截断 yunet 的 12 输出枚举导致 pp 计数断言。已提升到 16，且该检查从硬断言改为错误返回。
   - 库参数 `pAnchors_8/16/32` 原为 NULL（pp 顶部 `#if 0` 引用的锚点头文件在仓库中并不存在），首次解码即 NULL 解引用 HardFault（BFAR=0xAFC）。现已在 pp init 时按 `in_size` **动态生成三层锚点表**（约定与 OpenCV FaceDetectorYN 一致：每网格单元 1 个锚点、行优先、`{col*stride, row*stride}` 像素值，解码侧用 `anchors[1].x` 兼任 stride），`nb_detections_8/16/32` 同步由 `in_size` 推导（JSON 中即使填写也会被覆盖）。支持任意 32 倍数的正方形输入。

1. **颜色顺序已在模型侧解决**：YuNet 原按 BGR 训练（ST 配置 `color_mode: bgr`），与 PIPE2 的 RGB888 输出不匹配。已用 `Script/yunet_bgr_to_rgb.py` 将**首卷积权重的输入通道维反转**（BGR 模型吃 RGB 输入 ≡ 反转首卷积 in-channel 切片，卷积对输入通道线性、通道交换为对合，且该权重量化仅按输出通道 per-channel，交换是精确等价无需重量化）。设备端**直接喂 RGB888，无需任何转换**。onnxruntime 自验证：5 张随机图、12 个输出与原模型 `original(swap(x))` **逐位一致（max diff = 0）**；重新编译后 12 个输出的 shape/scale/zero_point 与 BGR 版完全相同。
2. **构建已验证**（stedgeai 4.0.1，`yolov8_od` profile）：产物 `network_rel.bin` 200,176 B（权重 94,521 B），N6M1 封包校验通过；激活占用 1,136,020 B（npuRAM3/4/5 + hyperRAM ~94KB，由 `stm32n6_reloc_yolov8_od.mpool` 分配）。
3. **输出张量已核对**：12 个输出，顺序为 `cls_8, cls_16, cls_32, obj_8, obj_16, obj_32, bbx_8, bbx_16, bbx_32, kps_8, kps_16, kps_32`，与 `pp_fd_yunet_ui` 的索引映射**完全一致**；形状 `[1,1600,*]`/`[1,400,*]`/`[1,100,*]`（×1/×1/×4/×10 通道）。真实 scale/zero_point 已从编译产物回填到 `output_spec`（运行时 pp 仍从 NN 实例读取）。**结果映射为 PP_TYPE_MPE**（与 blazeface 同约定），5 个关键点（双眼/鼻/双嘴角，含名称与连接表）随之透出给绘制/上报/前端。
4. 剩余上机验证项：检测精度与阈值调优、推理耗时。

## 四、配置项（Web 可配，NVS 持久化）

沿用 `json_config_mgr` 三件套（struct / NVS / JSON 导入导出）。初版 schema：

| 分组 | 配置项 | 类型/范围 | 默认 | 说明 |
|------|--------|-----------|------|------|
| 总开关 | `enabled` | bool | false | demo 总开关 |
| 检测 | `confidence_threshold` | 0–1 | 0.5 | 显示/跟踪用置信度阈值 |
| 检测 | `detection_conf_threshold` | 0–1 | 0.5 | 检测置信度阈值，低于此值的人脸直接丢弃（过滤背景误检） |
| 检测 | `min_face_area` | 千分比² | 1500 | 小于此面积的人脸忽略（720p 下约 37px 见方，远距离小脸过滤） |
| 跟踪 | `track_iou_threshold` | 0–1 | 0.3 | IoU 匹配阈值 |
| 跟踪 | 统计口径 | — | — | track ID **在确认时分配**（连续命中 confirm_frames），未确认的误检不消耗编号；`累计人脸` = 已分配的 ID 数，两者一致 |
| 上报 | 消息队列约束 | — | — | CMSIS-ThreadX 的 osMessageQueue 单条消息上限 16 个 ULONG：worker 队列只传**堆分配的消息指针**（结构体约 180B 超限会导致队列创建失败、整个上报链路瘫痪） |
| 稳定性 | jpegc 驱动 | — | — | 移植 ne301 `cherry-usb-dev` 分支的修复：jpegcTask 优先级 High→**Realtime**（喂料线程被 web/USB Realtime 发送阻塞曾把编码拖到秒级；**编码服务块间 osDelay(1) 让渡**——否则 Realtime 转换突发会连续抢占视频流水线，人脸上报瞬间可见卡帧）、**静默卡死看门狗**（同模式无进展 → 全外设重初始化 + 唤醒等待者）、HW 错误恢复（避免句柄永久 BUSY）、**INPUT 前排干游离完成 token**（避免下一次 OUTPUT 秒败）、SET_ENC/DEC_PARAM 的 IDLE 保护、编码等待超时 10s→30s |
| 性能 | 线程分工 | — | — | 相机线程只做**逐行裁剪**（亚毫秒/脸）；CPU 重的 **JPEG 编码（RGB→YCbCr）在 worker 线程**执行（含一次重试，规避 jpegc 超时后信号量计数错位的连锁失败）；马赛克为 **4×4 采样平均**（视觉等效，算力降一个量级） |
| 状态机 | PENDING 老化 | — | — | PENDING 超时 `result_timeout_ms` 一律转 TIMEOUT（无论请求是否 in-flight）——否则手动触发后若裁剪/编码/发送失败，状态会永久卡在"识别中" |
| 裁剪 | 裁剪实现 | — | — | `image_crop_roi` 原实现有缺陷：`draw_copy_hw` 前景层 InputOffset 硬编码为 0（字体位图契约），把 ROI 当连续行读，任何子矩形裁剪都产生斜切坏图（连带 `image_extract_and_resize_roi`）。已改为**逐行 CPU 拷贝**（行距天然正确；该工具链此前无其他使用者，零回归） |
| 裁剪 | JPEG 尺寸约束 | — | — | 编码统一对齐抓拍路径：宽高补齐到 **16 的倍数**（原生裁剪向上补齐并夹回画面内、固定尺寸 N 向上取 16 倍数）并**固定 420 采样**——444 采样会让 N6 的 JPEG 硬件编码挂死（10s 超时返回 BUSY）；编码失败按冷却退避并打限频日志，不逐帧重试 |
| 跟踪 | `track_max_missed` | 帧 | 15 | 丢失超过该帧数回收 track ID（并清其识别结果） |
| 跟踪 | `track_confirm_frames` | 帧 | 3 | 连续命中 N 帧才确认新 track（防误检闪烁） |
| 跟踪 | `face_hold_ms` | ms | 400 | 检测批次为空时画面保持上次框的时长（防闪烁；与 `track_max_missed` 共同决定框消失前的总滞留） |
| 跟踪 | `extrapolation_enabled` | bool | false | 框/马赛克/裁剪按跟踪速度外推补偿流水线延迟；推理快（如 yunet ~十几 ms）时建议关闭 |
| 模糊 | `blur_enabled` | bool | false | 人脸马赛克开关 |
| 模糊 | `mosaic_block` | 4–64 | 16 | 马赛克块大小（PIPE1 像素） |
| 模糊 | `blur_expand_ratio` | 0–1 | 0.1 | 马赛克区域相对人脸框的外扩比例 |
| 裁剪 | `crop_expand_ratio` | 0–1 | 0.2 | 上报裁剪统一外扩比例（四边同比例，clamp 截断） |
| 裁剪 | `crop_min_size` | px | 32 | 裁剪区域最小边长（小于则不上报） |
| 裁剪 | `report_fixed_size` | 0 / 32–512 | 0 | 0=原始裁剪尺寸上报；>0=固定正方形边长（先等比缩放再填充，不拉伸） |
| 裁剪 | `jpeg_quality` | 10–95 | 80 | 上报 JPEG 质量 |
| 上报 | `report_enabled` | bool | false | 上报开关 |
| 上报 | `report_channel` | mqtt / webhook | webhook | 上报通道，**互斥单选** |
| 上报 | `report_confidence_threshold` | 0–1 | 0.6 | 上报置信度阈值（≥显示阈值） |
| 上报 | `report_periodic_enabled` | bool | false | 周期性重报开关（新 track 首报不受此开关控制，始终执行） |
| 上报 | `report_cooldown_ms` | ms | 10000 | 同一 track 两次上报最小间隔（含首报后的重报） |
| 上报 | `result_timeout_ms` | ms | 5000 | 等待识别结果的超时（超时前端显示"超时/未知"） |
| 上报 | `result_expire_ms` | ms | 30000 | 识别结果在 track 上的保留时长（人脸持续在画面时过期后重新变为"识别中"并可再触发上报） |
| 上报 | `face_report_topic` / `face_result_topic` | string | — | 已取消独立 topic：MQTT 模式复用现有 `data_report_topic`（上报）与 `data_receive_topic`（结果），见 5.1 |
| 绘制 | `show_box` / `show_id` / `show_confidence` / `show_landmarks` / `show_recognition` | bool | true | 叠加绘制元素开关（landmarks 仅模型支持时生效） |

> 两个"外扩"语义不同：`blur_expand_ratio` 作用于马赛克区域，`crop_expand_ratio` 作用于上报裁剪区域，相互独立。

## 五、上报与识别回传协议（v1）

后端未定，以本节为契约。字段设计对齐设备现有上报封装（`metadata` + `device_info` + `image_data`/multipart `image`），便于固件复用现有构造器。

### 5.1 通道与互斥

- `report_channel=webhook`：设备 HTTP POST，**识别结果从同步响应返回**（200 + JSON，固件读取响应体解析）。
- `report_channel=mqtt`：设备发布到现有 `data_report_topic`（`face_report` JSON 内嵌 base64 图）；识别结果由后端发布到设备**现有的下行接收 topic**（`data_receive_topic` / 控制通道），报文带 `type: face_recognition_result`，固件在控制命令解析前拦截消费。
- 两通道同一时刻只有一个工作；Web 切换即时生效（下一次上报走新通道）。
- 复用现有通道意味着**无需新增 MQTT 订阅**；模拟后端脚本的 `--topic-report` / `--topic-result` 对应这两个现有 topic。

### 5.2 上报载荷（设备 → 后端）

**MQTT**（单条 JSON，crop JPEG 以 base64 内嵌；裁剪图 ≤512×512，典型 <60KB，base64 后 <80KB，在 broker 常规限制内；如需更大走现有分块发布器）：

```json
{
  "type": "face_report",
  "request_id": "ne302-48E7AABB-000123",
  "timestamp_ms": 1759070000123,
  "frame_id": 4567,
  "device_info": { "device_name": "NE302", "serial_number": "48E7AABB..." },
  "face": {
    "track_id": 3,
    "reason": "new_track",
    "detection_confidence": 0.92,
    "bbox": [512, 208, 96, 128],
    "expanded_bbox": [493, 189, 134, 166],
    "landmarks": [[560, 250], [585, 252], [573, 272], [558, 286], [588, 286]]
  },
  "image": {
    "format": "jpeg", "width": 134, "height": 166, "size": 15384,
    "quality": 80, "resized": false
  },
  "image_data": "data:image/jpeg;base64,/9j/4AA...",
  "encoding": "base64"
}
```

**Webhook**（multipart/form-data，与现有 webhook 服务的 part 命名一致）：

- part `metadata`（`application/json`）：上述 JSON 去掉 `image_data`/`encoding` 两个字段；
- part `image`（`image/jpeg`）：裁剪 JPEG 原始字节。

字段说明：

| 字段 | 说明 |
|------|------|
| `request_id` | 设备生成的唯一请求 ID（`{设备前缀}-{单调序号}`），后端**必须原样回显**，是结果关联的第一键 |
| `face.track_id` | 设备跟踪 ID（当帧），用于展示关联；track 回收后设备不再接受该 ID 的迟到结果 |
| `face.reason` | `new_track`（新目标首报）/ `periodic`（周期重报）/ `manual`（手动触发） |
| `face.bbox` / `expanded_bbox` | 720p 原图坐标 `[x, y, w, h]`；前者为检测原始框，后者为外扩+clamp 后实际裁剪区域 |
| `face.landmarks` | 720p 坐标；**无关键点模型省略此字段或为 `null`** |
| `image.resized` | `false`=原始裁剪尺寸；`true`=已等比缩放+填充到 `report_fixed_size`（此时 width=height=固定边长） |

### 5.3 识别结果载荷（后端 → 设备）

Webhook 同步响应（HTTP 200，`application/json`）与 MQTT `face_result_topic` 消息使用**同一 schema**：

```json
{
  "type": "face_recognition_result",
  "request_id": "ne302-48E7AABB-000123",
  "track_id": 3,
  "timestamp_ms": 1759070000123,
  "status": "ok",
  "name": "张三",
  "confidence": 0.93,
  "message": ""
}
```

| 字段 | 说明 |
|------|------|
| `request_id` | **必须回显**上报值 |
| `track_id` / `timestamp_ms` | 回显上报值，便于日志与前端直接使用 |
| `status` | `ok`=识别出；`unknown`=完成识别但无匹配；`no_face`=图中未检出人脸；`error`=后端内部错误 |
| `name` | 识别姓名/人员 ID；`unknown`/`no_face`/`error` 时为 `"unknown"` 或空 |
| `confidence` | 识别置信度 0–1；非 `ok` 时可为 0 |
| `message` | 可选错误/附加信息 |

### 5.4 关联、超时与去重语义（设备端）

1. 设备维护 pending 表：`request_id → {track_id, 发送时刻}`；收到结果按 `request_id` 匹配，写入对应 track 的结果缓存（`name/confidence/收到时刻`），并从 pending 表移除。
2. `result_timeout_ms` 内未收到结果：该次请求标记超时，前端显示"识别中…"超时后转"未知"；不阻塞后续上报。
3. track 回收（`track_max_missed` 超限）时：清除其结果缓存，忽略后续迟到结果（静默丢弃）。
4. 结果在 track 上保留 `result_expire_ms`；人脸一直在画面但结果过期 → 标签回到"识别中"，且在周期重报开启时允许再次上报刷新。
5. 同一 `request_id` 的重复结果只接受第一次（幂等）。
6. MQTT 模式下设备订阅 `face_result_topic` 的 QoS 与现有 command topic 一致；Webhook 非 200 响应或连接失败按上报失败处理，走现有上传重试/放弃策略（upload_coordinator），**不**因识别失败影响检测与模糊。

## 六、展示与边界处理（设备端已实现的 OSD 方案）

设备侧采用**烧录进视频流**的 OSD：框、track ID、检测置信度、识别结果行、关键点全部绘制在 PIPE1 帧上再编码（DMA2D 画线/矩形 + Font16 文本 + 半透明底板），前端播放 H.264 流即所见即所得，天然与画面同步。识别姓名非 ASCII（如中文）时 OSD 显示为 `?`/`OK 93%` 形式（Font16 仅 ASCII），完整姓名通过 `/api/v1/face/results` 获取（供后续 Web 页面渲染）。

每张人脸绘制（**单行标签**）：

- 人脸框（颜色按识别状态：白=识别中、青=已上报等待、绿=识别成功、黄=未知、红=超时/错误）；
- 单行标签 `#3 92% Alice 93%`（track ID + 检测置信度 + 识别结果，三个部分各自开关控制，空格拼接不换行）；
- 关键点（模型支持且 `show_landmarks` 开启时，3×3 品红点；yunet 的 5 关键点经 MPE 映射透出）。

绘制层边界规则（已实现）：

1. **贴上边缘**：标签默认在框上方，空间不足时移入框内顶部；
2. **贴左/右边缘**：标签右溢出时向左收缩对齐，最小 x=2；
3. 框/标签始终 clamp 在画面内，底部空间不足时回退框内；
4. 马赛克与框默认使用最近一次匹配框（`extrapolation_enabled` 可开启按跟踪速度外推补偿 AI 流水线延迟，EMA 速度、300ms 内有效）；
5. 马赛克先画、框和标签后画，保证标签在最上层。

i18n：中英双语（CI 强制）。

## 七、模拟后端脚本 `Script/face_demo_backend.py`

用于联调与演示，不依赖后端应用：

```bash
# Webhook 模式（默认 http://0.0.0.0:8080/face）
python Script/face_demo_backend.py --mode webhook --port 8080

# MQTT 模式（需 pip install paho-mqtt）
python Script/face_demo_backend.py --mode mqtt --mqtt-host 127.0.0.1 --mqtt-port 1883

# 常用选项
#   --out ./face_captures        收到的人脸裁剪图保存目录
#   --gallery ./face_gallery     注册库目录（文件名即姓名，如 张三.jpg；装了 Pillow 时用均值哈希匹配）
#   --latency-ms 800             模拟识别时延
#   --fail-rate 0.1              模拟后端错误率（测超时/错误路径）
#   --unknown-rate 0.2           模拟识别不出的比例
#   --hash-threshold 12          均值哈希海明距离阈值（≤此值判为同人，默认 12/64）
```

识别逻辑（`--recognizer auto|face|ahash|md5`，默认 auto 自动选最优）：

- **face（真识别，推荐）**：[insightface](https://pypi.org/project/insightface/)（ArcFace，`pip install insightface onnxruntime`，免编译；首次启动自动下载 ~330MB 模型包到 `~/.insightface`）。对裁剪图做人脸检测 + 512 维特征提取，与库内特征做余弦相似度匹配，≥ `--match-threshold`（默认 0.40）判定为该文件名（姓名），置信度由相似度映射（0.60–0.99）；裁剪图中检测不到人脸返回 `no_face`。gallery 特征按文件 mtime 缓存到 `<gallery>/.fd_cache.pkl`，重复启动秒级就绪。已验证：同一人变尺寸/变压缩质量的裁剪稳定匹配（0.99/0.91），非人脸图返回 `no_face`，未录入人脸返回 `unknown`。
- **ahash**：装有 Pillow 时的降级方案，8×8 均值哈希 + 海明距离（粗粒度，姿态/光照敏感）。
- **md5**：无任何依赖时的链路演示兜底（同一图确定性同名）。

注册流程（两种 layout，均重启脚本生效）：
- 单张：`--out` 收到的裁剪图改名放 `--gallery`（文件名=姓名，如 `zhangsan.jpg`）；
- 多张（推荐，提升姿态/光照鲁棒性）：`--gallery/<姓名>/*.jpg` 子目录，目录内所有图作为该人的多 shot 特征，显示名统一为目录名。

## 八、实现状态

**设备端（已完成，编译通过零警告）**：

| 模块 | 内容 |
|------|------|
| `Custom/Core/Video/face_detection.c/h` + `face_detection_config.h` | IoU 跟踪（确认帧/丢失回收/速度外推）、CPU 马赛克、外扩裁剪（统一比例 + clamp，原生尺寸或固定边长 letterbox 不拉伸）、硬件 JPEG、上报决策（新 track 必报 + 周期重报 + 冷却 + 置信度门限）、request_id↔track 结果缓存（超时/过期/幂等/迟到丢弃）、OSD 绘制（边界标签规则）、上报 worker 线程（队列深度 4，溢水丢弃计数） |
| 配置三件套 | `json_config_mgr/nvs/json(_internal)` 全量 25 字段（NVS 键 `fd_*`，导入导出含 `face_detection` 节） |
| `api_face_detection_module.c` | `/api/v1/face/{config,status,results,trigger,reset,verify}`（部分合并更新 + 范围校验），已注册 `web_service`，web-api 文档已再生成；**verify** 接收 multipart 人脸图，经识别通道（webhook 同步 / MQTT 等待回包）返回 `{status,name,confidence}`，前端"识别验证"卡片调用 |
| `webhook_service_push_json_image` | 自定义 JSON 元数据 + 图像 multipart 的同步推送，捕获响应体回传识别结果 |
| 抓拍记录归档（方案 A） | 上报成功后把裁剪图经 `upload_coordinator_enqueue_capture_local`（新增的**仅存储**入队，记录状态固定 LOCAL）存入抓拍记录库：不进入抓拍上传流水线，避免同一张人脸被后端收到两次；新增 `AICAM_CAPTURE_TRIGGER_FACE`（=8，追加在枚举尾部不破坏既有 ID），记录页 trigger 显示 `face`，图片 ID 为 `face_<request_id>`；发送路径零阻塞（coordinator 复制字节，失败仅告警） |
| MQTT 通道 | 上报走 `data_report_topic`（JSON+base64）；结果在 `data_receive_topic` 上被 `face_detection_try_handle_mqtt` 拦截消费 |
| 挂接点 | `ai_telemetry_result_callback` 前置消费每帧结果；`ai_service_draw_callback` 中 demo 启用时接管叠加并替换通用 AI 绘制 |

**待做**：

| 项 | 说明 |
|----|------|
| 上机验证 | yunet 精度/阈值调优、推理耗时、与模拟后端 `Script/face_demo_backend.py` 联调 |

**Web 前端（已完成，构建通过）**：

- 页面 `Web/src/pages/faceDetection/index.tsx`，路由 `/face-detection`，菜单"人脸检测"；
- 实时画面（H.264 WS 播放器，OSD 已烧录在流内），右下角显示当前激活模型名；
- 状态卡片（1s 轮询）：当前人脸 / 累计人脸 / 上报成功失败 / 识别成功未知超时；
- **人脸跟踪与识别列表**（1s 轮询 `/face/results`）：每张人脸 `#ID 检测置信度` + 识别状态徽标（颜色区分）+ 完整姓名与置信度（浏览器字体可正常渲染中文姓名，弥补 OSD 的 ASCII 限制）；
- 配置面板：全部 demo 配置可调（**单列限宽**布局，开关即时生效，数值项统一保存；上报通道 Webhook/MQTT 单选下拉）；
- 操作：手动上报（全部确认 track；**重报语义**：已有识别结果的 track 会先清掉旧结果回到"识别中"，新结果到达后覆盖）、重置统计；
- i18n 中英词条已加（`sys.face_detection.*`），构建产物 331KB（1M Web 分区充裕）。

**边界情况对照**（实现验收）：贴边裁剪 clamp 与标签换位 ✓；ID 切换不串结果（结果按 request_id 关联、track 回收即清）✓；短暂丢失重现身保持 ID 与结果 ✓；迟到结果丢弃 ✓；显示阈值 < 上报阈值"显示不报"✓；裁剪小于 `crop_min_size` 不报 ✓；固定尺寸 letterbox 不拉伸 ✓；后端不可达不影响检测/模糊 ✓；无关键点模型（OD 输出）正常、`landmarks=null` ✓。

## 九、隐私与数据安全

人脸照片属于**生物特征敏感数据**，本 demo 的联调流程会把它落到多个本地位置，务必注意：

| 位置 | 内容 | 防护 |
|---|---|---|
| 仓库 `face_captures/` | 模拟后端收到的每张上报裁剪图 | **已加入 .gitignore，严禁提交/分享** |
| 仓库 `face_gallery/`（含 `.fd_cache.pkl` 特征缓存） | 注册的人脸照与 ArcFace 特征 | 同上 |
| 模拟后端 `--out` 目录 | 同 face_captures | 联调结束删除 |
| 设备抓拍记录库 | 归档的未打码人脸图（trigger=face） | 演示结束在记录页删除或清空记录 |
| 公共 MQTT broker（如 broker.emqx.io） | 上报图以 base64 明文经过，任何订阅者可见 | **仅限演示用假数据**；真实数据必须自建 broker + TLS（MQTTS）+ 账号鉴权 |

`.gitignore` 已包含 `face_captures/` 与 `face_gallery/`；提交前请用 `git status` 复核没有人脸文件被暂存。

## 十、风险与注意事项

- **Web 分区 1M**：新页面注意体积，避免超出 WEB 资产分区。
- **PIPE2 上限 480×480**：yunet 320×320 满足；若换 512 以上输入模型需改管线。
- **NPU 全局互斥**：单模型无并发问题；马赛克/裁剪在 CPU/DMA2D，注意与编码器抢 DMA2D 的调度。
- **web-api 文档 CI**：新增端点后必须再生成（`python Script/gen_web_api_docs.py --check`）。
- **yuNet 剩余上机验证项**：检测阈值调优、推理耗时（颜色序已模型侧解决、输出顺序/形状/量化参数已构建期核对，见第三节）。
