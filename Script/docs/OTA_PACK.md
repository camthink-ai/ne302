# OTA Package Tool - Usage Guide

## Required Parameters

| Parameter | Description | Example |
|-----------|-------------|---------|
| `input` | Input bin file path | `firmware.bin` |
| `-o, --output` | Output file path | `firmware_v1.0.0.1_ota.bin` |
| `-t, --type` | Firmware type | `app`, `fsbl`, `web`, `ai_model` |
| `-n, --name` | Firmware name | `"NE302_APP"` |
| `-d, --desc` | Firmware description | `"Main Application"` |
| `-v, --version` | Version (format: major.minor.patch.build) | `1.0.0.1` |

## Firmware Types

| Type | Description | Type Code |
|------|-------------|-----------|
| `fsbl` | First Stage Boot Loader | 0x01 |
| `app` | Main Application | 0x02 |
| `web` | Web Assets | 0x03 |
| `ai_model` | AI Model | 0x04 |
| `config` | Configuration | 0x05 |
| `patch` | Patch | 0x06 |
| `full` | Full Package | 0x07 |

## Usage Examples

### Basic Usage

```bash
# FSBL
python ota_packer.py ne302_FSBL_signed.bin \
    -t fsbl \
    -n "NE302_FSBL" \
    -d "First Stage Boot Loader" \
    -v 1.0.0.1 \
    -o ne302_FSBL_signed_v1.0.0.1_ota.bin

# APP
python ota_packer.py ne302_Appli_signed.bin \
    -t app \
    -n "NE302_APP" \
    -d "Main Application" \
    -v 2.3.1.125 \
    -o ne302_Appli_signed_v2.3.1.125_ota.bin

# Web Assets
python ota_packer.py web-assets.bin \
    -t web \
    -n "NE302_WEB" \
    -d "Web User Interface" \
    -v 1.5.0.88 \
    -o web-assets_v1.5.0.88_ota.bin

# AI Model
python ota_packer.py model_yolov8_uf_od_coco.bin \
    -t ai_model \
    -n "YOLOv8_OD" \
    -d "YOLOv8 Object Detection COCO" \
    -v 3.0.2.45 \
    -o model_yolov8_uf_od_coco_v3.0.2.45_ota.bin
```

### With Output Directory

```bash
python ota_packer.py firmware.bin \
    -t app \
    -n "MyApp" \
    -d "My Application" \
    -v 1.0.0.1 \
    -o release/firmware_v1.0.0.1_ota.bin
```

### With Custom Address

```bash
# Flash to APP Slot B instead of Slot A
python ota_packer.py firmware.bin \
    -t app \
    -n "MyApp" \
    -d "My Application" \
    -v 1.0.0.1 \
    -a 0x70500000
```

## Clean Mode

```bash
# Remove all generated OTA packages (with confirmation)
python ota_packer.py --clean

# Force clean without confirmation
python ota_packer.py --clean --force
```

## Batch Processing

**Use the shell script for batch processing:**

```bash
# Edit version configuration
vim pack_all_ota.sh

# Set versions
FSBL_VERSION="1.0.0.1"
APP_VERSION="2.3.1.125"
WEB_VERSION="1.5.0.88"
AI_MODEL_VERSION="3.0.2.45"

# Run batch script
./pack_all_ota.sh
```

## Output Format
### Auto-generated Filename

```
{input_base}_v{major}.{minor}.{patch}.{build}_ota.bin
```

**Examples:**
```
ne302_Appli_signed.bin → ne302_Appli_signed_v2.3.1.125_ota.bin
web-assets.bin → web-assets_v1.5.0.88_ota.bin
model_yolov8.bin → model_yolov8_v3.0.2.45_ota.bin
```

### Package Structure

```
┌─────────────────────────────────┐
│  OTA Header (1024 bytes)        │  ← At beginning
│  ├─ Magic: 0x4F544155 (OTAU)   │
│  ├─ Firmware Type               │
│  ├─ Version: major.minor.patch.build
│  ├─ Firmware Size               │
│  ├─ CRC32, SHA256               │
│  ├─ Target Address              │
│  └─ Partition Info              │
├─────────────────────────────────┤
│  Original Firmware Data         │  ← Variable size
└─────────────────────────────────┘
```

## Verification

After creating OTA packages, verify them:  

```bash
python verify_ota_package.py firmware_v1.0.0.1_ota.bin
```

**Expected output:**
```
=== OTA Package Verification ===
File: firmware_v1.0.0.1_ota.bin
File size: 1436480 bytes

=== Header Information ===
Magic: 0x4F544155
Header Version: 0x0100
Firmware Type: 2
Version: 1.0.0.1
Total Package Size: 1436480 bytes

✓ PASSED: OTA package structure is valid!
```

## Error Messages

### Missing Required Parameter

```bash
$ python ota_packer.py firmware.bin
usage: ota_packer.py [-h] -n NAME -d DESC -t {fsbl,app,web,ai_model,config,patch,full} -v VERSION ...
ota_packer.py: error: the following arguments are required: -n/--name, -d/--desc, -t/--type, -v/--version
```

**Solution:** Provide all required parameters  

### Invalid Version Format

```bash
$ python ota_packer.py firmware.bin -t app -n "APP" -d "Desc" -v 1.0.0
Error: Version must have exactly 4 parts (major.minor.patch.build)
Example: 1.0.0.1
```

**Solution:** Use 4-part version format  

### File Not Found

```bash
$ python ota_packer.py nonexistent.bin -t app -n "APP" -d "Desc" -v 1.0.0.1
Error: Input file does not exist: nonexistent.bin
```

**Solution:** Check file path  

## Integration Examples

### In Makefile

```makefile
VERSION := 1.0.0.1
APP_NAME := "NE302_APP"
APP_DESC := "Main Application"

.PHONY: ota
ota: all
	cd Script && python ota_packer.py \
		../bin/ne302_Appli_signed.bin \
		-t app \
		-n $(APP_NAME) \
		-d $(APP_DESC) \
		-v $(VERSION)
```

### In Shell Script

```bash
#!/bin/bash
set -e

VERSION="2.3.1.125"

# Build
make clean all

# Create OTA package
python Script/ota_packer.py bin/firmware.bin \
    -t app \
    -n "MyApp" \
    -d "Production Release v${VERSION}" \
    -v "${VERSION}"

# Verify
python Script/verify_ota_package.py bin/firmware_v${VERSION}_ota.bin

# Flash
cd Script
./maker.sh flash ../bin/firmware_v${VERSION}_ota.bin 0x70100000
```

### CI/CD Pipeline

```yaml
# GitLab CI Example
build_ota:
  stage: build
  script:
    - make clean all
    - python Script/ota_packer.py bin/firmware.bin
        -t app
        -n "Product_FW"
        -d "Build ${CI_COMMIT_TAG}"
        -v "${CI_COMMIT_TAG}"
  artifacts:
    paths:
      - bin/*_ota.bin
```

## Migration Guide

### From Old Version

**Old command (no longer works):**
```bash
# ✗ This will fail now
python ota_packer.py firmware.bin -v 1.0.0.1
```

**New command (all parameters required):**
```bash
# ✓ All parameters required including output file
python ota_packer.py firmware.bin \
    -o firmware_v1.0.0.1_ota.bin \
    -t app \
    -n "AppName" \
    -d "Description" \
    -v 1.0.0.1
```

### Batch Processing

**Old command (no longer available):**
```bash
# ✗ Batch mode removed
python ota_packer.py --batch
```

**New approach (use shell script):**
```bash
# ✓ Use batch script
./pack_all_ota.sh
```

## Best Practices

### 1. Version Naming

```bash
# Production releases
-v "1.0.0.0"    # Major release
-v "1.1.0.0"    # Feature update
-v "1.0.1.0"    # Bug fix
-v "1.0.0.1"    # Build number

# Development builds
-v "2.0.0.100"  # Major version 2, build 100
-v "2.1.0.250"  # Feature branch, build 250
```

### 2. Naming Convention

```bash
# Use descriptive names
-n "NE302_APP_v2.3"     # Include version in name
-n "YOLOv8_COCO_OD"     # Include model details
-n "WebUI_Material"     # Include UI framework

# Use detailed descriptions
-d "Main Application with Bluetooth 5.0 support"
-d "YOLOv8 Nano Object Detection trained on COCO dataset"
-d "Web User Interface using Material Design"
```

### 3. Directory Organization

```bash
# Organize output files
bin/
├── releases/
│   ├── v1.0.0/
│   │   ├── ne302_FSBL_v1.0.0.1_ota.bin
│   │   ├── ne302_Appli_v1.0.0.1_ota.bin
│   │   └── web-assets_v1.0.0.1_ota.bin
│   └── v2.0.0/
│       └── ...
└── development/
    └── ...
```

## Quick Reference

```bash
# Complete command template
python ota_packer.py <input_file> \
    -o <output_file> \
    -t <fsbl|app|web|ai_model> \
    -n "<FirmwareName>" \
    -d "<Description>" \
    -v <major.minor.patch.build> 

# Real example
python ota_packer.py ne302_Appli_signed.bin \
    -o ne302_Appli_signed_v2.3.1.125_ota.bin \
    -t app \
    -n "NE302_APP" \
    -d "Main Application" \
    -v 2.3.1.125
```

## Troubleshooting

| Issue | Cause | Solution |
|-------|-------|----------|
| Missing parameter error | Required parameter not provided | Add all required parameters |
| Version format error | Wrong version format | Use format: `1.0.0.1` |
| File not found | Wrong file path | Check file path and extension |
| Permission denied | No write permission | Check directory permissions |
| Invalid firmware type | Unknown type specified | Use: fsbl, app, web, or ai_model |

## Support

For more information:
- [README.md](README.md) - Complete guide
- [pack_all_ota.sh](pack_all_ota.sh) - Batch processing script
- [verify_ota_package.py](verify_ota_package.py) - Verification tool

---

**Version**: 2.0  
**Last Updated**: October 2025  
**Breaking Changes**: All parameters now required, batch mode removed
