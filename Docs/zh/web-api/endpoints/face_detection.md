---
title: 人脸检测 Demo 端点参考
---

<!-- GENERATED FILE - 由 Script/gen_web_api_docs.py 自动生成，请勿手工编辑 -->

# 人脸检测 Demo 端点参考

人脸跟踪、马赛克、裁剪上报与识别结果管理

源文件: [`Custom/Services/Web/api/api_face_detection_module.c`](https://github.com/camthink-ai/ne302/blob/main/Custom/Services/Web/api/api_face_detection_module.c)

共 **7** 个端点。鉴权列 ✅ 表示需要携带[认证凭据](../authentication.md)。

| 方法 | 路径 | 鉴权 | 处理函数 |
|------|------|:----:|----------|
| `GET` | `/api/v1/face/config` | — | `face_config_get_handler` |
| `POST` | `/api/v1/face/config` | — | `face_config_set_handler` |
| `GET` | `/api/v1/face/status` | — | `face_status_handler` |
| `GET` | `/api/v1/face/results` | — | `face_results_handler` |
| `POST` | `/api/v1/face/trigger` | — | `face_trigger_handler` |
| `POST` | `/api/v1/face/verify` | — | `face_verify_handler` |
| `POST` | `/api/v1/face/reset` | — | `face_reset_handler` |
