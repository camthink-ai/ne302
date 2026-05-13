# Fast capture optimization

## Background

The existing capture path suffers from high latency and power draw mainly because it waits for the core layer (`core_system`) and service layer to finish initializing before capture starts—often nearly one second—while only overlapping network and capture in parallel, which yields poor throughput and higher energy use.

## Constraints

* Follow patterns already present in the core and service layers; avoid inventing divergent flows.
* When a phased task must notify other threads, use RTOS event flags. For **one-shot** completion flags, wait with **no auto-clear** (e.g. CMSIS-RTOS2 `osFlagsNoClear`), or have a single owner clear explicitly after every consumer has observed the flag. If the first waiter **auto-clears** on return, later threads or a second wait on the same flag may block forever (“stuck waiting”). Multi-consumer cases may use semaphores, condition variables, or “only one waiter does wait+clear; others read shared state”.
* Behavior must align with the legacy path: upload JSON shape, chunked upload waits, etc. (see `communication_service` / MQTT reporting; fields align with `mqtt_service_config_t` and related structs in `json_config_mgr.h`).
* After JPEG encode completes, call `JPEGC_CMD_UNSHARE_ENC_BUFFER` to detach the buffer from the encoder so no other path keeps using the shared JPEG buffer; when done, `JPEGC_CMD_FREE_ENC_BUFFER`. Definitions: `Custom/Hal/jpegc.h`; implementation: matching `case` in `Custom/Hal/jpegc.c`.
* **AI pipe width/height** (NVS keys `NVS_KEY_AI_PIPE_WIDTH` / `NVS_KEY_AI_PIPE_HEIGHT` in `json_config_internal.h`): if NVS has no valid values, **wait for AI model info** (e.g. `quick_snapshot_wait_ai_info` / `nn_model_info_t`), then size the AI pipe from model input—do not force defaults before info is ready.
* Several module headers already exist; mirror their style and extend only as needed.

## Repository status (implementation progress)

| Component | Header | Source | Notes |
|-----------|--------|--------|-------|
| `quick_storage` | `quick_storage.h` | `quick_storage.c` (placeholder when written) | NVS read APIs, write-task queue params, `qs_comm_pref_type_t`, etc. |
| `quick_network` | `quick_network.h` | `quick_network.c` (placeholder when written) | init / wait_config / MQTT task enqueue / remote wakeup switch |
| `quick_snapshot` | `quick_snapshot.h` | `quick_snapshot.c` (placeholder when written) | init, wait config, main frame, JPEG, AI info/result |
| `quick_bootstrap` | `quick_bootstrap.h` (empty when written) | `quick_bootstrap.c` (placeholder when written) | Public entry not yet fixed in the header; add declarations before shipping |

The `.c` files were not yet in the project `Makefile` (no `quick_` build entries); add them to Appli (or equivalent) when integrating.

**Planned app entry**: In `Custom/Hal/driver_core.c`, after `camera_register()` / `jpegc_register()`, inside `ENABLE_U0_MODULE` where `u0_module_get_wakeup_flag()` is read—that is the fast-capture entry (comments reference Quick_Bootstrap). Following `draw_register()`, `nn_register()`, etc. in `driver_core_init` remain the full-featured path; **draw and NN** in fast capture should register per `qs_snapshot_config_t` (AI on/off, etc.) and **tear down after the phase**, not assume the full init chain.

## Header APIs and data model (match code)

Use this to stay consistent with the headers.

**`quick_storage.h`**

* Config structs: `qs_snapshot_config_t` (AI toggles/thresholds, light, mirror/flip, fast-capture skip/resolution/quality), `qs_work_mode_config_t` (image/video placeholders, PIR/schedule/remote triggers), `qs_mqtt_all_config_t` (topic, QoS, embedded `ms_mqtt_config_t`).
* Network: `qs_comm_pref_type_t` (`AUTO` / `WIFI` / `CELLULAR` / `POE` / `DISABLE`), `quick_storage_read_netif_config(..., netif_config_t *)` (types from `netif_manager.h`).
* WiFi: `qs_wifi_network_info_t` and `quick_storage_read_known_wifi_networks` (up to `MAX_KNOWN_WIFI_NETWORKS`).
* Async disk writes: `quick_storage_init`, `quick_storage_add_write_task` (`qs_write_task_param_t`: append/overwrite, file name, `data`/`data_len`, completion callback).

**`quick_network.h`**

* `quick_network_init`, `quick_network_wait_config` (outputs the chosen `qs_comm_pref_type_t`).
* `quick_network_add_mqtt_task` (`qs_mqtt_task_param_t`: payload and send-complete callback).
* `quick_network_switch_remote_wakeup_mode` (per comments: WiFi communication scenario for now).

**`quick_snapshot.h`**

* Depends on `quick_storage.h`, `nn.h`: `quick_snapshot_init`, `quick_snapshot_wait_config`, `quick_snapshot_wait_capture_frame`, `quick_snapshot_wait_capture_jpeg`, `quick_snapshot_wait_ai_info`, `quick_snapshot_wait_ai_result`.

**`quick_bootstrap`**

* Header TBD: declare a single fast-capture pipeline entry (e.g. `quick_bootstrap_run(void)` or with wakeup reason), ordering as in **Design** below.

## Design

`Custom/Hal/Quick_Bootstrap` implements a fast capture path that avoids waiting for the full core/service boot chain while reusing HAL: `camera`, `jpegc`, `nn`, `netif_manager`, MQTT client, etc.

1. **`quick_storage`**: Read NVS into the structs declared here (mirror `Custom/Core/System/json_config_mgr.h` and `json_config_get_*`). `quick_storage_read_snapshot_config` and peers use **json_config / NVS**. **Read on demand**: if a module is disabled in config, skip its block and threads to cut flash traffic and RAM. After `quick_storage_init`, a **storage thread** handles `quick_storage_add_write_task` for JPEG, AI JSON, etc., without blocking capture.
2. **`quick_network`**: After init, run **network** and **MQTT** threads (align with `netif_manager` link-up instead of duplicating logic). Network reads `comm_pref_type` from `quick_storage`: `COMM_PREF_TYPE_DISABLE` exits early; `COMM_PREF_TYPE_AUTO` probes **ETH → 4G → WiFi**; then `quick_storage_read_netif_config` for details, bring link up, signal MQTT; MQTT uses `quick_storage_read_mqtt_all_config`, waits for network, drains `quick_network_add_mqtt_task`.
3. **`quick_snapshot`**: Capture thread uses `quick_storage_read_snapshot_config`; optionally starts AI thread. **AI pipe size**: if NVS has valid width/height, configure the pipe; else load model first and configure after **`nn_model_info_t`** is ready. Light/camera per config; if AI is on, grab inference frames and notify the AI thread; **stop sensor/pipe soon after frames** to save power; main YUV goes to `jpegc`. AI thread loads model, exposes info via `quick_snapshot_wait_ai_info`, waits for inference frames, releases NPU/model resources after inference. `quick_snapshot_wait_*` synchronizes with bootstrap (event bits e.g. “JPEG ready”, “AI ready”).
4. **`quick_bootstrap`**: Public orchestrator. Order: `quick_storage_init` → `quick_network_init` → `quick_snapshot_init` (tweak if deps require); `quick_storage_read_work_mode_config`; wait `quick_snapshot_wait_config`; then JPEG and AI per config (`quick_snapshot_wait_capture_jpeg` / `quick_snapshot_wait_ai_result`). Join logic: if AI finishes first, build structured JSON, `quick_storage_add_write_task`, stash upload fragments; if JPEG first, write disk then base64 if needed; when both ready, build **production-equivalent** uplink JSON and `quick_network_add_mqtt_task`; if `quick_network_wait_config` says no network, skip base64 and uplink JSON. If SD is present and AI ran, optional second grab, boxes, re-encode JPEG, async write. Finally wait for disk and MQTT tasks, then sleep via power APIs (`Custom/Hal/pwr.c`, etc.) per work mode.

## Reference code (when implementing)

| Area | Starting point |
|------|----------------|
| JSON / NVS app config | `Custom/Core/System/json_config_mgr.h` (e.g. `json_config_get_mqtt_service_config`, fast-capture fields) |
| Device camera/light/capture flow | `Custom/Services/Device/device_service.c` (interaction with `camera`, `jpegc`, `nn`) |
| Network / netif | `Custom/Hal/Network/netif_manager/`, `netif_config_t` |
| MQTT | `ms_mqtt_client` (`qs_mqtt_all_config_t` embeds `ms_mqtt_config_t`) |
| JPEG buffer lifetime | `Custom/Hal/jpegc.h` / `jpegc.c`: `JPEGC_CMD_UNSHARE_ENC_BUFFER`, `JPEGC_CMD_FREE_ENC_BUFFER` |

## Integration checklist

* [ ] Add `quick_*.c` to Appli (or subproject) `Makefile` / `SOURCES`.
* [ ] Call `quick_bootstrap` from the right thread or cold-boot path (avoid circular deps with modules not ready in full `driver_core_init`).
* [ ] Fill `quick_bootstrap.h` public API; document tie-in with power/wakeup (e.g. `u0_module` wakeup flag) via parameters or internal reads.
* [ ] Bench/unit: network off, SD-only, MQTT-only, AI on/off branches match legacy uplink JSON.
