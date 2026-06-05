# ST Edge AI Model Generation and Packaging Toolkit

> **Chinese Version**: See [MODEL_PACK_CN.md](MODEL_PACK_CN.md)

This toolkit provides comprehensive model processing capabilities for STM32N6:

## Core Components

1. **`generate-reloc-model.sh`** - Model conversion script
   - Convert TensorFlow Lite model to C code
   - Convert C code to `network_rel.bin` binary file
   
2. **`model_packager.py`** - Model packaging tool
   - Create deployable `.bin` package files
   - Validate and extract model packages
   - Integrate model binaries with JSON configurations

## Complete Workflow

### Step 1: Generate Model Binary

```bash
# Show help (no arguments)
./generate-reloc-model.sh

# Use default model with default settings
./generate-reloc-model.sh -m st_yolo_x_nano_480_1.0_0.25_3_int8.tflite

# Specify custom model file
./generate-reloc-model.sh -m my_model.tflite

# Specify model and output
./generate-reloc-model.sh -m my_model.tflite -o my_network.bin

# Full configuration
./generate-reloc-model.sh -m my_model.tflite -o my_network.bin -f "default@./config.json"

# With additional stedgeai arguments
./generate-reloc-model.sh -m st_yolo_x_nano_480_1.0_0.25_3_int8.tflite -f reloc-ne302@neural_art_reloc.json -o network.bin -a "--abc xxx --xyz xxx"

# Clean generated files
./generate-reloc-model.sh -c
```

### Step 2: Create Model Package

```bash
# Create a complete model package
python model_packager.py create \
    --model network_rel.bin \
    --config yolov8n_256_quant_pc_uf_pose_coco-st.json \
    --output yolov8n_pose.bin

# Validate existing package
python model_packager.py validate --package yolov8n_pose.bin

# Extract package contents
python model_packager.py extract --package yolov8n_pose.bin --output extracted/
```

### Complete Example Workflow

```bash
# 1. Generate model binary from TensorFlow Lite
./generate-reloc-model.sh -m yolov8n_256_quant_pc_uf_pose_coco-st.tflite -o network_rel.bin

# 2. Create deployable package
python model_packager.py create \
    --model network_rel.bin \
    --config yolov8n_256_quant_pc_uf_pose_coco-st.json \
    --output yolov8n_pose.bin

# 3. Validate the package
python model_packager.py validate --package yolov8n_pose.bin

# 4. Deploy to target system (copy package file)
cp yolov8n_pose.bin ../bin/
```

### Positional Arguments (Backward Compatible)

```bash
./generate-reloc-model.sh [model_file] [output_name] [neural_art_config]
```

### Help

```bash
./generate-reloc-model.sh --help
```

## Prerequisites

1. **Environment Variables**
   ```bash
   export STEDGEAI_CORE_DIR="/path/to/stedgeai/installation"
   ```

2. **Required Tools**
   - `stedgeai` command line tool
   - `python` interpreter
   - ST Edge AI toolchain

3. **Required Files**
   - Model file (default: `st_yolo_x_nano_480_1.0_0.25_3_int8.tflite`)
   - Neural Art config: `$STEDGEAI_CORE_DIR/scripts/N6_reloc/test/neural_art_reloc.json`
   - NPU driver script: `$STEDGEAI_CORE_DIR/scripts/N6_reloc/npu_driver.py`

## Model Generation Options

| Short | Long | Argument | Description |
|-------|------|----------|-------------|
| `-m` | `--model` | FILE | TensorFlow Lite model file |
| `-o` | `--output` | FILE | Output binary file name |
| `-f` | `--config` | CONFIG | Neural art configuration |
| `-a` | `--append` | ARGS | Additional arguments for stedgeai generate |
| `-c` | `--clean` | - | Clean all generated files |
| `-h` | `--help` | - | Show help message |

## Model Packaging Options

### Create Package
```bash
python model_packager.py create --model MODEL --config CONFIG --output OUTPUT
```

| Option | Argument | Description |
|--------|----------|-------------|
| `--model` | FILE | Path to network_rel.bin file |
| `--config` | FILE | Path to model configuration JSON |
| `--output` | FILE | Output package path (.bin) |

### Validate Package
```bash
python model_packager.py validate --package PACKAGE
```

| Option | Argument | Description |
|--------|----------|-------------|
| `--package` | FILE | Path to model package |

### Extract Package
```bash
python model_packager.py extract --package PACKAGE --output OUTPUT
```

| Option | Argument | Description |
|--------|----------|-------------|
| `--package` | FILE | Path to model package |
| `--output` | DIR | Output directory |

## Output Files

### Model Generation Outputs
- **Main Output**: `network_rel.bin` - Final binary file
- **Intermediate Files**: 
  - `st_ai_c/` - C code and headers
  - `st_ai_bin/` - Intermediate binary files

### Model Package Outputs
- **Package File**: `*.bin` - Complete deployable package
- **Package Contents**:
  - `network_rel.bin` - Model binary
  - `model_config.json` - Model configuration
  - `metadata.json` - Package metadata
  - `package_info.json` - Package structure information

## Neural Art Configuration

### Priority Order

1. **Command line option** (highest priority)
   ```bash
   ./generate-reloc-model.sh -m model.tflite -f "default@./config.json"
   ```

2. **Environment variable**
   ```bash
   export NEURAL_ART_CONFIG_FILE="/path/to/config.json"
   ./generate-reloc-model.sh -m model.tflite
   ```

3. **Default** (lowest priority)
   ```
   default@$STEDGEAI_CORE_DIR/scripts/N6_reloc/test/neural_art_reloc.json
   ```

## Additional Arguments

The `-a|--append` option allows you to pass additional arguments directly to the `stedgeai generate` command.

### Usage Examples

```bash
# Add optimization flags
./generate-reloc-model.sh -m my_model.tflite -a "--optimization-level high --memory-layout custom"

# Add debugging options
./generate-reloc-model.sh -m my_model.tflite -a "--verbose --debug-info"

# Combine multiple options
./generate-reloc-model.sh -m st_yolo_x_nano_480_1.0_0.25_3_int8.tflite -f reloc-ne302@neural_art_reloc.json -o network.bin -a "--abc xxx --xyz xxx"
```

### Important Notes

- Additional arguments are passed directly to `stedgeai generate`
- Arguments should be quoted to preserve spaces and special characters
- Use `stedgeai generate --help` to see all available options

## Clean Function

### Usage
```bash
# Clean all generated files
./generate-reloc-model.sh -c
# or
./generate-reloc-model.sh --clean
```

### Cleaned Items
- **Directories**: `st_ai_c/`, `st_ai_bin/`
- **Binary files**: `network_rel.bin`, `*.bin`
- **Generated files**: `network.c`, `network.h`, `network_data.*`
- **Temporary files**: `*.log`, `*.tmp`

## Example Output

```
[INFO] ST Edge AI Model Generation Script
[INFO] Model: st_yolo_x_nano_480_1.0_0.25_3_int8.tflite
[INFO] Output: network_rel.bin

[INFO] Step 1: Converting model to C code...
[SUCCESS] Model successfully converted to C code

[INFO] Step 2: Converting C code to network_rel.bin...
[SUCCESS] C code successfully converted to binary

[SUCCESS] Ready to deploy: network_rel.bin
```

## Quick Start Guide

### For Object Detection (YOLO-X)
```bash
# 1. Generate model binary
./generate-reloc-model.sh -m st_yolo_x_nano_480_1.0_0.25_3_int8.tflite -f reloc-ne302@neural_art_reloc.json -o network_rel.bin

# 2. Create package
python model_packager.py create \
    --model network_rel.bin \
    --config st_yolo_x_nano_480_1.0_0.25_3_int8.json \
    --output yolox_detection.bin

# 3. Deploy
cp yolox_detection.bin ../bin/
```

### For Pose Estimation (YOLOv8)
```bash
# 1. Generate model binary
./generate-reloc-model.sh -m yolov8n_256_quant_pc_uf_pose_coco-st.tflite -f reloc-ne302@neural_art_reloc.json -o network_rel.bin

# 2. Create package
python model_packager.py create \
    --model network_rel.bin \
    --config yolov8n_256_quant_pc_uf_pose_coco-st.json \
    --output yolov8_pose.bin

# 3. Deploy
cp yolov8_pose.bin ../bin/
```

## Troubleshooting

### Common Issues

1. **STEDGEAI_CORE_DIR not set**
   ```bash
   export STEDGEAI_CORE_DIR="/path/to/stedgeai/installation"
   ```

2. **Model validation fails**
   - Check model file format (should be .tflite)
   - Verify model file is not corrupted
   - Ensure model is compatible with STM32N6

3. **Package creation fails**
   - Verify network_rel.bin exists and is valid
   - Check JSON configuration syntax
   - Ensure all required fields are present in config

4. **Clean workspace**
   ```bash
   ./generate-reloc-model.sh --clean
   ```

### Debug Information

Enable verbose output:
```bash
# For model generation
./generate-reloc-model.sh -m model.tflite -a "--verbose --debug-info"

# For packaging
python model_packager.py validate --package model.bin
```

## File Structure

```
Model/
├── generate-reloc-model.sh          # Main generation script
├── model_packager.py                # Packaging tool
├── README.md                        # This documentation
├── *.tflite                         # Input TensorFlow Lite models
├── *.json                           # Model configurations
├── st_ai_c/                         # Generated C code (temporary)
├── st_ai_bin/                       # Generated binaries (temporary)
├── network_rel.bin                  # Relocatable model binary
└── *.bin                            # Deployable packages
```