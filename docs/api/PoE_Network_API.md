# PoE / Ethernet Network API

## Overview

This document describes the RESTful APIs for PoE/Ethernet network management, for frontend development reference.

**Base path:** `/api/v1/system/network/poe`

**Authentication:** all endpoints require authentication (`require_auth: true`)

**Content-Type:** `application/json`

---

## Unified Response Format

### Success response

```json
{
  "success": true,
  "message": "Operation success message",
  "data": {
    // actual business data
  }
}
```

### Failure response

```json
{
  "success": false,
  "error_code": "ERROR_CODE_STRING",
  "message": "Error description"
}
```

### Response fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `success` | boolean | yes | whether the operation succeeded |
| `error_code` | string | no | error code string (present only on failure) |
| `message` | string | no | result message (optional) |
| `data` | object | no | business data (present only on success) |

### Error codes (error_code)

| Error code | Value | Description |
|------------|-------|-------------|
| `INVALID_REQUEST` | 400 | invalid request |
| `UNAUTHORIZED` | 401 | unauthorized |
| `FORBIDDEN` | 403 | forbidden |
| `NOT_FOUND` | 404 | not found |
| `METHOD_NOT_ALLOWED` | 405 | method not allowed |
| `TIMEOUT` | 408 | timeout |
| `TOO_MANY_REQUESTS` | 429 | too many requests |
| `INTERNAL_ERROR` | 500 | internal error |
| `BAD_GATEWAY` | 502 | bad gateway |
| `SERVICE_UNAVAILABLE` | 503 | service unavailable |
| `GATEWAY_TIMEOUT` | 504 | gateway timeout |

---

## PoE Status Codes

| Code | Name | Description |
|------|------|-------------|
| 0 | `POE_STATUS_OFFLINE` | PoE offline / not powered |
| 1 | `POE_STATUS_LINK_DOWN` | Ethernet cable not connected |
| 2 | `POE_STATUS_CONNECTING` | connecting (DHCP in progress) |
| 3 | `POE_STATUS_CONNECTED` | connected, valid IP |
| 4 | `POE_STATUS_DHCP_FAILED` | failed to obtain IP via DHCP |
| 5 | `POE_STATUS_STATIC_CONFIG_ERROR` | static IP configuration error |
| 6 | `POE_STATUS_IP_CONFLICT` | IP address conflict |
| 7 | `POE_STATUS_GATEWAY_UNREACHABLE` | gateway unreachable |
| 8 | `POE_STATUS_DNS_ERROR` | DNS resolution error |
| 9 | `POE_STATUS_ERROR` | generic error |

---

## 1. Get PoE Status

### Request

```http
GET /api/v1/system/network/poe/status
```

### Success response

```json
{
  "success": true,
  "message": "PoE status retrieved successfully",
  "data": {
    "available": true,
    "status": "Connected",
    "ip_address": "192.168.60.100",
    "connected": true
  }
}
```

### data fields

| Field | Type | Description |
|-------|------|-------------|
| `available` | boolean | whether the PoE/Ethernet module is available |
| `status` | string | connection status: `Unavailable`, `Disconnected`, `Connecting`, `Connected`, `Failed`, `Switching` |
| `ip_address` | string | current IP address (valid only when connected) |
| `connected` | boolean | whether connected |

### Response when the module is unavailable

```json
{
  "success": true,
  "message": "PoE status retrieved successfully",
  "data": {
    "available": false,
    "status": "unavailable",
    "message": "PoE/Ethernet module not available"
  }
}
```

---

## 2. Get PoE Detailed Information

### Request

```http
GET /api/v1/system/network/poe/info
```

### Success response

```json
{
  "success": true,
  "message": "PoE info retrieved successfully",
  "data": {
    "available": true,
    "network_status": "Connected",
    "status_code": 3,
    "status_message": "Connected",
    "ip_mode": "dhcp",
    "ip_address": "192.168.60.100",
    "netmask": "255.255.255.0",
    "gateway": "192.168.60.1",
    "dns_primary": "8.8.8.8",
    "dns_secondary": "223.5.5.5",
    "hostname": "aicam-poe",
    "mac_address": "00:11:22:33:44:55",
    "interface_name": "wn",
    "link_up": true,
    "poe_powered": true,
    "connection_duration_sec": 3600,
    "connection_start_time": 1703318400,
    "dhcp_lease_time": 86400,
    "dhcp_lease_remaining": 43200,
    "connect_count": 5,
    "disconnect_count": 2,
    "dhcp_fail_count": 0,
    "last_error_code": 0
  }
}
```

### data fields

| Field | Type | Description |
|-------|------|-------------|
| `available` | boolean | whether the PoE module is available |
| `network_status` | string | network status text |
| `status_code` | number | status code (see the PoE status code table) |
| `status_message` | string | message for the status code |
| `ip_mode` | string | IP mode: `dhcp` or `static` |
| `ip_address` | string | IPv4 address |
| `netmask` | string | subnet mask |
| `gateway` | string | default gateway |
| `dns_primary` | string | primary DNS server |
| `dns_secondary` | string | secondary DNS server |
| `hostname` | string | hostname |
| `mac_address` | string | MAC address |
| `interface_name` | string | network interface name |
| `link_up` | boolean | physical link state |
| `poe_powered` | boolean | PoE power state |
| `connection_duration_sec` | number | connection duration (seconds) |
| `connection_start_time` | number | connection start timestamp |
| `dhcp_lease_time` | number | DHCP lease time (seconds) |
| `dhcp_lease_remaining` | number | remaining DHCP lease time (seconds) |
| `connect_count` | number | connection attempts |
| `disconnect_count` | number | disconnections |
| `dhcp_fail_count` | number | DHCP failures |
| `last_error_code` | number | last error code |

---

## 3. Get PoE Configuration

### Request

```http
GET /api/v1/system/network/poe/config
```

### Success response

```json
{
  "success": true,
  "message": "PoE config operation completed",
  "data": {
    "ip_mode": "dhcp",
    "ip_address": "192.168.60.232",
    "netmask": "255.255.255.0",
    "gateway": "192.168.60.1",
    "dns_primary": "8.8.8.8",
    "dns_secondary": "223.5.5.5",
    "hostname": "aicam-poe",
    "dhcp_timeout_ms": 30000,
    "dhcp_retry_count": 3,
    "dhcp_retry_interval_ms": 5000,
    "power_recovery_delay_ms": 5000,
    "auto_reconnect": true,
    "persist_last_ip": true,
    "validate_gateway": true,
    "detect_ip_conflict": true
  }
}
```

### data fields

| Field | Type | Description |
|-------|------|-------------|
| `ip_mode` | string | IP mode: `dhcp` or `static` |
| `ip_address` | string | static IPv4 address |
| `netmask` | string | subnet mask |
| `gateway` | string | default gateway |
| `dns_primary` | string | primary DNS server |
| `dns_secondary` | string | secondary DNS server |
| `hostname` | string | hostname |
| `dhcp_timeout_ms` | number | DHCP timeout (ms) |
| `dhcp_retry_count` | number | DHCP retry count |
| `dhcp_retry_interval_ms` | number | DHCP retry interval (ms) |
| `power_recovery_delay_ms` | number | power-on recovery delay (ms) |
| `auto_reconnect` | boolean | auto-reconnect toggle |
| `persist_last_ip` | boolean | persist the last IP |
| `validate_gateway` | boolean | validate gateway reachability |
| `detect_ip_conflict` | boolean | detect IP conflicts |

---

## 4. Set PoE Configuration

### Request

```http
POST /api/v1/system/network/poe/config
Content-Type: application/json
```

### Request body

```json
{
  "ip_mode": "static",
  "ip_address": "192.168.60.200",
  "netmask": "255.255.255.0",
  "gateway": "192.168.60.1",
  "dns_primary": "8.8.8.8",
  "dns_secondary": "223.5.5.5",
  "hostname": "my-device",
  "dhcp_timeout_ms": 30000,
  "dhcp_retry_count": 3,
  "auto_reconnect": true,
  "validate_gateway": true,
  "detect_ip_conflict": true
}
```

> **Note**: all fields are optional - send only the fields to change. Omitted fields keep their current values.

### Request fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `ip_mode` | string | no | `dhcp` or `static` |
| `ip_address` | string | no | static IPv4 address |
| `netmask` | string | no | subnet mask |
| `gateway` | string | no | default gateway |
| `dns_primary` | string | no | primary DNS server |
| `dns_secondary` | string | no | secondary DNS server |
| `hostname` | string | no | hostname (max 31 chars) |
| `dhcp_timeout_ms` | number | no | DHCP timeout (ms) |
| `dhcp_retry_count` | number | no | DHCP retry count |
| `auto_reconnect` | boolean | no | auto-reconnect toggle |
| `validate_gateway` | boolean | no | validate gateway reachability |
| `detect_ip_conflict` | boolean | no | detect IP conflicts |

### Success response

```json
{
  "success": true,
  "message": "PoE config operation completed",
  "data": {
    "message": "PoE configuration updated successfully",
    "ip_mode": "static"
  }
}
```

---

## 5. Validate a Static IP Configuration

### Request

```http
POST /api/v1/system/network/poe/validate
Content-Type: application/json
```

### Request body

```json
{
  "ip_address": "192.168.60.200",
  "netmask": "255.255.255.0",
  "gateway": "192.168.60.1",
  "dns_primary": "8.8.8.8",
  "dns_secondary": "223.5.5.5",
  "hostname": "my-device",
  "check_gateway": true,
  "check_conflict": true
}
```

### Request fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `ip_address` | string | yes | IPv4 address to validate |
| `netmask` | string | yes | subnet mask |
| `gateway` | string | yes | default gateway |
| `dns_primary` | string | no | primary DNS server |
| `dns_secondary` | string | no | secondary DNS server |
| `hostname` | string | no | hostname |
| `check_gateway` | boolean | no | whether to check gateway reachability |
| `check_conflict` | boolean | no | whether to check for IP conflicts |

### Success response (validation passed)

```json
{
  "success": true,
  "message": "PoE configuration validation completed",
  "data": {
    "valid": true,
    "errors": [],
    "warnings": [],
    "gateway_reachable": true,
    "ip_conflict": false
  }
}
```

### Response when validation fails

```json
{
  "success": true,
  "message": "PoE configuration validation completed",
  "data": {
    "valid": false,
    "errors": [
      "Invalid IP address format",
      "IP address conflict detected"
    ],
    "warnings": [
      "Gateway may not be reachable"
    ],
    "gateway_reachable": false,
    "ip_conflict": true
  }
}
```

### data fields

| Field | Type | Description |
|-------|------|-------------|
| `valid` | boolean | whether the configuration is valid |
| `errors` | array | list of error messages |
| `warnings` | array | list of warning messages |
| `gateway_reachable` | boolean | whether the gateway is reachable |
| `ip_conflict` | boolean | whether an IP conflict exists |

---

## 6. Apply PoE Configuration

Applies the currently saved configuration and connects to the network.

### Request

```http
POST /api/v1/system/network/poe/apply
```

### Success response

```json
{
  "success": true,
  "message": "PoE configuration applied successfully",
  "data": {
    "success": true,
    "status": "Connected",
    "ip_mode": "dhcp"
  }
}
```

### Failure response

```json
{
  "success": false,
  "error_code": "INTERNAL_ERROR",
  "message": "Failed to apply PoE configuration"
}
```

### data fields

| Field | Type | Description |
|-------|------|-------------|
| `success` | boolean | whether the operation succeeded |
| `status` | string | current network status |
| `ip_mode` | string | current IP mode |

---

## 7. Save PoE Configuration

Persists the current configuration to NVS; it is restored automatically after reboot.

### Request

```http
POST /api/v1/system/network/poe/save
```

### Success response

```json
{
  "success": true,
  "message": "PoE configuration saved successfully",
  "data": {
    "success": true
  }
}
```

### Failure response

```json
{
  "success": false,
  "error_code": "INTERNAL_ERROR",
  "message": "Failed to save PoE configuration"
}
```

---

## 8. Connect to the PoE Network

### Request

```http
POST /api/v1/system/network/poe/connect
```

### Success response

```json
{
  "success": true,
  "message": "PoE connection initiated",
  "data": {
    "success": true,
    "status": "Connecting"
  }
}
```

### Failure response

```json
{
  "success": false,
  "error_code": "INTERNAL_ERROR",
  "message": "Failed to connect PoE network"
}
```

---

## 9. Disconnect from the PoE Network

### Request

```http
POST /api/v1/system/network/poe/disconnect
```

### Success response

```json
{
  "success": true,
  "message": "PoE disconnected successfully",
  "data": {
    "success": true,
    "status": "Disconnected"
  }
}
```

### Failure response

```json
{
  "success": false,
  "error_code": "INTERNAL_ERROR",
  "message": "Failed to disconnect PoE network"
}
```

---

## Common Error Responses

### Service not running

```json
{
  "success": false,
  "error_code": "SERVICE_UNAVAILABLE",
  "message": "Communication service is not running"
}
```

### PoE module unavailable

```json
{
  "success": false,
  "error_code": "SERVICE_UNAVAILABLE",
  "message": "PoE/Ethernet module not available"
}
```

### Wrong request method

```json
{
  "success": false,
  "error_code": "METHOD_NOT_ALLOWED",
  "message": "Only GET method is allowed"
}
```

### Request body parse error

```json
{
  "success": false,
  "error_code": "INVALID_REQUEST",
  "message": "Invalid JSON request body"
}
```

---

## Frontend Integration Examples

### TypeScript type definitions

```typescript
// unified response type
interface ApiResponse<T = any> {
  success: boolean;
  error_code?: string;
  message?: string;
  data?: T;
}

// PoE status
interface PoeStatus {
  available: boolean;
  status: string;
  ip_address?: string;
  connected?: boolean;
  message?: string;
}

// PoE detailed information
interface PoeDetailInfo {
  available: boolean;
  network_status: string;
  status_code: number;
  status_message: string;
  ip_mode: 'dhcp' | 'static';
  ip_address: string;
  netmask: string;
  gateway: string;
  dns_primary: string;
  dns_secondary: string;
  hostname: string;
  mac_address: string;
  interface_name: string;
  link_up: boolean;
  poe_powered: boolean;
  connection_duration_sec: number;
  connection_start_time: number;
  dhcp_lease_time: number;
  dhcp_lease_remaining: number;
  connect_count: number;
  disconnect_count: number;
  dhcp_fail_count: number;
  last_error_code: number;
}

// PoE configuration
interface PoeConfig {
  ip_mode: 'dhcp' | 'static';
  ip_address: string;
  netmask: string;
  gateway: string;
  dns_primary: string;
  dns_secondary: string;
  hostname: string;
  dhcp_timeout_ms: number;
  dhcp_retry_count: number;
  dhcp_retry_interval_ms: number;
  power_recovery_delay_ms: number;
  auto_reconnect: boolean;
  persist_last_ip: boolean;
  validate_gateway: boolean;
  detect_ip_conflict: boolean;
}

// validation result
interface ValidationResult {
  valid: boolean;
  errors: string[];
  warnings: string[];
  gateway_reachable: boolean;
  ip_conflict: boolean;
}
```

### Request wrapper

```typescript
const API_BASE = '/api/v1/system/network/poe';

async function request<T>(
  endpoint: string,
  options: RequestInit = {}
): Promise<ApiResponse<T>> {
  const response = await fetch(`${API_BASE}${endpoint}`, {
    headers: {
      'Content-Type': 'application/json',
      'Authorization': `Bearer ${getToken()}`
    },
    ...options
  });
  return response.json();
}

// API functions
export const poeApi = {
  // get status
  getStatus: () => request<PoeStatus>('/status'),

  // get detailed info
  getInfo: () => request<PoeDetailInfo>('/info'),

  // get configuration
  getConfig: () => request<PoeConfig>('/config'),

  // set configuration
  setConfig: (config: Partial<PoeConfig>) =>
    request<{ message: string; ip_mode: string }>('/config', {
      method: 'POST',
      body: JSON.stringify(config)
    }),

  // validate configuration
  validate: (config: {
    ip_address: string;
    netmask: string;
    gateway: string;
    dns_primary?: string;
    dns_secondary?: string;
    hostname?: string;
    check_gateway?: boolean;
    check_conflict?: boolean;
  }) => request<ValidationResult>('/validate', {
    method: 'POST',
    body: JSON.stringify(config)
  }),

  // apply configuration
  apply: () => request<{ success: boolean; status: string; ip_mode: string }>('/apply', {
    method: 'POST'
  }),

  // save configuration
  save: () => request<{ success: boolean }>('/save', {
    method: 'POST'
  }),

  // connect
  connect: () => request<{ success: boolean; status: string }>('/connect', {
    method: 'POST'
  }),

  // disconnect
  disconnect: () => request<{ success: boolean; status: string }>('/disconnect', {
    method: 'POST'
  })
};
```

### Usage

```typescript
// load PoE detailed information
async function loadPoeInfo() {
  const res = await poeApi.getInfo();
  if (res.success && res.data) {
    console.log('IP address:', res.data.ip_address);
    console.log('Connection status:', res.data.network_status);
    console.log('PoE powered:', res.data.poe_powered);
  } else {
    console.error('Failed to load:', res.message);
  }
}

// set a static IP and save it
async function setStaticIP(config: {
  ip: string;
  netmask: string;
  gateway: string;
  dns1: string;
  dns2: string;
}) {
  // 1. validate the configuration
  const validateRes = await poeApi.validate({
    ip_address: config.ip,
    netmask: config.netmask,
    gateway: config.gateway,
    dns_primary: config.dns1,
    dns_secondary: config.dns2,
    check_gateway: true,
    check_conflict: true
  });

  if (!validateRes.success) {
    throw new Error(validateRes.message || 'Validation request failed');
  }

  if (!validateRes.data?.valid) {
    throw new Error(validateRes.data?.errors.join(', ') || 'Invalid configuration');
  }

  // 2. set the configuration
  const setRes = await poeApi.setConfig({
    ip_mode: 'static',
    ip_address: config.ip,
    netmask: config.netmask,
    gateway: config.gateway,
    dns_primary: config.dns1,
    dns_secondary: config.dns2
  });

  if (!setRes.success) {
    throw new Error(setRes.message || 'Failed to set configuration');
  }

  // 3. apply the configuration
  const applyRes = await poeApi.apply();
  if (!applyRes.success || !applyRes.data?.success) {
    throw new Error(applyRes.message || 'Failed to apply configuration');
  }

  // 4. persist to storage
  const saveRes = await poeApi.save();
  if (!saveRes.success || !saveRes.data?.success) {
    console.warn('Failed to save to NVS; the configuration may be lost after reboot');
  }

  return applyRes.data;
}

// switch to DHCP mode
async function switchToDHCP() {
  const setRes = await poeApi.setConfig({ ip_mode: 'dhcp' });
  if (!setRes.success) throw new Error(setRes.message);

  const applyRes = await poeApi.apply();
  if (!applyRes.success) throw new Error(applyRes.message);

  const saveRes = await poeApi.save();
  if (!saveRes.success) console.warn('Failed to save');
}
```

---

## Typical Workflows

### 1. On page load

```
GET /poe/info     → current status and configuration
GET /poe/config   → full configuration (to populate the form)
```

### 2. User changes the static IP configuration

```
POST /poe/validate  → validate the configuration (optional)
POST /poe/config    → save the configuration to memory
POST /poe/apply     → apply the configuration
POST /poe/save      → persist to NVS
```

### 3. User switches DHCP/static mode

```
POST /poe/config { "ip_mode": "dhcp" }
POST /poe/apply
POST /poe/save
```

### 4. Polling for status updates

```
GET /poe/status   → lightweight status query (recommended every 5 s)
GET /poe/info     → full information (only when details are needed)
```

---

## Version Information

- API version: v1
- Document updated: 2024-12-23
