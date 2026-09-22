# NE302 人数统计 Demo(People Counting)

分支:`feature/people-counting-demo`

实时显示当前画面人数,对人越线进出做 +1/-1 统计,并对人脸做马赛克模糊。计数线可在 Web 上拖拽配置,同时在画面上可见;跟踪/统计参数可调,统计可清空。

## 功能总览

| 功能 | 说明 |
|------|------|
| 当前画面人数 | IoU 跟踪器的活跃 track 数,OSD 与 Web 同步显示 |
| 越线进出统计 | 按目标质心相对分界线的侧向变化判定,防抖确认后计数(进 +1 / 出 -1) |
| 分界线 | Web 可视化拖拽两个端点配置;画面上绘制黄线 + 绿色箭头指示"进"方向 |
| 人脸马赛克 | 第二模型(blazeface)检测人脸,CPU 块平均马赛克,不画框不打标签 |
| Web 配置 | 分界线、方向反转、跟踪参数、马赛克参数、显示开关、清空统计 |

## 一、双模型准备与烧录

本 demo 同时运行两个模型,NPU 访问由全局互斥自动串行:

| 模型 | 分区(默认) | 地址 | 说明 |
|------|------------|------|------|
| 人体检测(主模型) | AI_1 | `0x70900000` | 现有 OD 模型,如 `yolov8n_256_quant_pc_uf_od_coco-person-st` |
| 人脸检测(副模型) | AI_2 | `0x71100000` | `blazeface_128_fd_ui`(STM32 modelzoo face_detection,仓库已带权重与配置) |

> 地址由 NVS 标志 `ai_1_active` 决定:默认 `false` → 人在 AI_1、人脸在 AI_2。
> 若把 `ai_1_active` 设为 true,则人对 AI_2、人脸对 AI_1。固件自动取"非激活分区"作为人脸模型地址。

### 1. 构建人脸模型包

仓库已包含 `Model/weights/blazeface_128_fd_ui.{tflite,json}`,后处理 `pp_fd_blazeface_ui` 已注册(输出映射为 `PP_TYPE_MPE`,带 6 关键点)。

```bash
# 方式 A:批量脚本(推荐,产出带 OTA 头的 _pkg.bin)
bash Script/build_all_models.sh blazeface_128_fd_ui
# 产出: Model/build/models/blazeface_128_fd_ui_v<版本>_pkg.bin

# 方式 B:make 单独构建
make model MODEL_NAME=blazeface_128_fd_ui
make pkg-model MODEL_NAME=blazeface_128_fd_ui
# 产出: build/blazeface_128_fd_ui_v<版本>_pkg.bin
```

### 2. 烧录

`_pkg.bin` 已含 1024 字节 OTA 头,直接烧到**分区起始地址**:

```bash
# 人体模型 → AI_1 (0x70900000)
make flash-model MODEL_NAME=yolov8n_256_quant_pc_uf_od_coco-person-st

# 人脸模型 → AI_2 (0x71100000),make 未提供该目标,用 CubeProgrammer:
STM32_Programmer_CLI -c port=SWD mode=HOTPLUG \
  -el <你的外部加载器.stldr> \
  -w Model/build/models/blazeface_128_fd_ui_v<版本>_pkg.bin 0x71100000
```

> 若烧裸包(无 OTA 头,`model_packager.py` 直接产物),地址需 +1024:`0x71100400`。
> OTA 升级流程本 demo 未做适配,按上述手动烧录即可。

### 3. 烧录固件与 Web

```bash
make app        # 新固件(含人数统计)
make flash-app  # 烧录应用
make web        # 新 Web 前端
make flash-web  # 烧录 Web 分区 (0x71900000)
```

## 二、Web 使用

导航栏 → **人数统计**(`/people-counting`)。

1. **画面区**:实时视频(与预览相同的 H.264 WS 流),叠加:
   - 黄色分界线 + 绿色方向箭头(与固件 OSD 一致的"进"方向)
   - 右下角徽标显示人脸模型加载状态
2. **编辑分界线**:点击左下"编辑分界线",拖拽两端点(或直接改数值,0–1000 千分比坐标),"保存分界线"后生效并持久化。
3. **统计卡片**:当前画面人数 / 进入 / 离开 / 净人数,1s 轮询刷新。
4. **配置面板**:
   - 人数统计总开关、画面显示分界线 / 计数 OSD、反转进出方向(即时生效)
   - 人脸马赛克开关、块大小(4–64px)、人脸框扩展(0–100%)
   - 跟踪参数:IoU 阈值、丢失容忍帧数、越线确认帧数、最小目标面积、线宽(保存后生效)
5. **清空统计**:计数与 track 归零。

> 画面 OSD 固件端每帧绘制 `IN:x OUT:y NOW:z`(右上角),与 Web 数值一致。
> 画面叠加依赖 AI 绘制回调,页面打开时会自动确保 `ai_enabled=true`、`overlay_results=true`。

## 三、REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/v1/counting/config` | 读取全部配置 |
| POST | `/api/v1/counting/config` | 部分字段合并更新(带范围校验),即时生效并写 NVS |
| GET | `/api/v1/counting/stats` | `current_count / in_count / out_count / net_count / active_tracks / face_model_loaded / face_model_name ...` |
| POST | `/api/v1/counting/reset` | 清空进出计数与跟踪目标 |

配置字段(snake_case,与固件结构一致):`enabled, show_line, show_osd, face_mosaic, line_x1/y1/x2/y2 (0-1000), line_thickness (1-16), invert_direction, track_iou_threshold (1-99), track_max_missed (1-50), cross_debounce (1-10), min_box_area, mosaic_block (4-64), mosaic_expand (0-100)`。

示例:

```bash
# 移动分界线为水平线(全宽,y=50%)
curl -X POST http://<device>/api/v1/counting/config \
  -H 'Content-Type: application/json' \
  -d '{"line_x1":100,"line_y1":500,"line_x2":900,"line_y2":500}'

# 清空统计
curl -X POST http://<device>/api/v1/counting/reset
```

## 四、实现说明

### 数据流

```
Camera pipe2 (硬件缩放至主模型输入, 如 256x256 RGB888)
  └─ AI node 线程: 主模型推理 (NPU)
       ├─ 结果分发器 → people_counting_on_person_result()   跟踪+越线计数
       └─ RGB888 区域平均降采样 (如 256→128)
            └─ 副模型(blazeface)推理 (NPU, 全局互斥串行)
                 └─ people_counting_on_face_result()        缓存人脸框

Camera pipe1 (1280x720 RGB565, 编码前)
  └─ Camera 线程绘制回调:
       1) 人脸马赛克 (CPU 块平均, 直接写 RGB565)
       2) 人框/标签 (原有 ai_draw_results, DMA2D)
       3) 分界线+方向箭头 (DMA2D)
       4) 计数 OSD (DMA2D)
```

### 关键点

- **双模型**:`nn.c` 多实例 API(`nn_instance_*`),副模型独立 exec/ext RAM(PSRAM)。人脸模型加载失败时优雅降级(计数继续,马赛克禁用,Web 徽标提示)。
- **硬件能力**:输入缩放用 DCMIPP pipe2(硬件);线/箭头/OSD 用 DMA2D;两模型推理用 NPU。马赛克因 DMA2D 不支持缩放,采用 CPU 块平均(典型人脸区域开销 <1ms)。
- **计数方向**:分界线 P1→P2 的叉积符号定义侧别;负→正为"进"(`invert_direction` 可反转);`cross_debounce` 帧确认防抖动;出生在线上的目标首个侧别不计数。
- **持久化**:新配置节 `people_counting`(NVS 键前缀 `pc_`),导出/导入配置文件自动包含。
- **结果分发**:AI node 单回调槽由 `ai_service_result_dispatcher` 扇出到遥测与本 demo,互不挤占。

### 涉及文件

| 文件 | 变更 |
|------|------|
| `Custom/Core/Video/people_counting.{c,h}` | 新增:跟踪/计数/绘制/马赛克核心 |
| `Custom/Core/Video/people_counting_config.h` | 新增:配置结构(配置层共享) |
| `Custom/Core/Video/video_ai_node.{c,h}` | 副模型 API + 推理挂钩 + RGB888 降采样 |
| `Custom/Services/AI/ai_service.{c,h}` | 结果分发器、demo 装配、人脸模型加载、绘制链 |
| `Custom/Core/System/json_config_*` | 新配置节(NVS/JSON/默认值) |
| `Custom/Services/Web/api/api_people_counting_module.{c,h}` | 新增:REST API |
| `Web/src/pages/peopleCounting/`、`services/api/peopleCounting.ts` | 新增:前端页面与 API 客户端 |

### 调参建议

- 计数偏多(抖动往返):增大 `cross_debounce`(如 3-5)。
- 快速通过漏计:减小 `cross_debounce`、减小 `track_max_missed`。
- 远处小目标误检为人数:增大 `min_box_area`。
- 马赛克不完整:增大 `mosaic_expand`(如 30-50)或 `mosaic_block`。
