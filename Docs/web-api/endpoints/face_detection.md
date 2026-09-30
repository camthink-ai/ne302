---
title: Face Detection Demo Endpoints
---

<!-- GENERATED FILE - do not edit manually. Regenerate with Script/gen_web_api_docs.py -->

# Face Detection Demo Endpoints

Face tracking, mosaic, crop reporting and recognition results

Source: [`Custom/Services/Web/api/api_face_detection_module.c`](https://github.com/camthink-ai/ne302/blob/main/Custom/Services/Web/api/api_face_detection_module.c)

**7** endpoints. The ✅ marker in the Auth column means the request must carry [credentials](../authentication.md).

| Method | Path | Auth | Handler |
|--------|------|:----:|---------|
| `GET` | `/api/v1/face/config` | — | `face_config_get_handler` |
| `POST` | `/api/v1/face/config` | — | `face_config_set_handler` |
| `GET` | `/api/v1/face/status` | — | `face_status_handler` |
| `GET` | `/api/v1/face/results` | — | `face_results_handler` |
| `POST` | `/api/v1/face/trigger` | — | `face_trigger_handler` |
| `POST` | `/api/v1/face/verify` | — | `face_verify_handler` |
| `POST` | `/api/v1/face/reset` | — | `face_reset_handler` |
