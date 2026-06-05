# NE302 Development Environment Setup Guide

## Quick Start

### 1. Check Environment

```bash
./check_env.sh        # Check what tools are missing
```

### 2. Auto Install (Recommended)

**Linux/macOS/Git Bash:**
```bash
./setup.sh
```

**Windows:**
```cmd
setup.bat
```

## Manual Installation

### 1. ARM GCC Toolchain

#### Windows

**Option 1: STM32CubeCLT (Recommended)**
1. Download: https://www.st.com/stm32cubeclt
2. Run the installer
3. Default path: `C:\ST\STM32CubeCLT\GNU-tools-for-STM32\bin`

**Option 2: ARM Official**
1. Download: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
2. Select: `arm-gnu-toolchain-*-mingw-w64-i686-arm-none-eabi.exe`
3. Install and note the path

#### Linux (Ubuntu/Debian)

```bash
# Method 1: Package manager (simple)
sudo apt update
sudo apt install gcc-arm-none-eabi make

# Method 2: Latest version
wget https://developer.arm.com/-/media/Files/downloads/gnu/13.3.rel1/binrel/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz
tar xf arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi.tar.xz
sudo mv arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi /opt/
sudo ln -s /opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/* /usr/local/bin/
```

#### macOS

```bash
# Using Homebrew
brew install --cask gcc-arm-embedded
```

### 2. Other Dependencies

#### Make

**Windows:**
- Git for Windows (includes make): https://git-scm.com/

**Linux:**
```bash
sudo apt install build-essential
```

**macOS:**
```bash
xcode-select --install
```

#### Python 3 (for model packaging)

**All platforms:**
- Download: https://www.python.org/
- Or use system package manager

#### Node.js & pnpm (for Web building)

**All platforms:**
```bash
# Install Node.js
# Download: https://nodejs.org/ (LTS version)

# Install pnpm
npm install -g pnpm
```

#### STM32_Programmer_CLI (for flashing)

**All platforms:**

1. Download **STM32CubeProgrammer**
   - Official: https://www.st.com/stm32cubeprog
   - Version: 2.19.0 or newer

2. Installation locations:
   - **Windows:** `C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin`
   - **Linux:** `/usr/local/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin`
   - **macOS:** `/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin`

3. Add to PATH:

**Windows (PowerShell):**
```powershell
$env:PATH += ";C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin"

# Or permanently:
[System.Environment]::SetEnvironmentVariable("Path", $env:Path + ";C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin", [System.EnvironmentVariableTarget]::User)
```

**Linux/macOS:**
```bash
# Add to ~/.bashrc or ~/.zshrc
export PATH="/usr/local/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin:$PATH"
```

4. Verify installation:
```bash
STM32_Programmer_CLI --version
```

#### STM32_SigningTool_CLI (for firmware signing)

**All platforms:**

1. Usually included in **STM32CubeCLT** or **STM32CubeProgrammer**

2. Locations:
   - **Windows:** `C:\ST\STM32CubeCLT\STM32_SigningTool_CLI\bin`
   - **Linux:** `/opt/st/stm32cubeclt_*/STM32_SigningTool_CLI/bin`

3. Add to PATH (similar to STM32_Programmer_CLI)

4. Verify installation:
```bash
STM32_SigningTool_CLI --version
```

**Note:** If SigningTool is not found in STM32CubeProgrammer, you may need to install STM32CubeCLT separately.

#### stedgeai (for AI model generation)

**All platforms:**

1. Download **ST Edge AI Core**
   - Official: https://www.st.com/en/development-tools/stedgeai-core.html#get-software
   - Or via **STM32Cube.AI**: https://www.st.com/en/embedded-software/x-cube-ai.html#get-software

2. Installation locations:
   - **Windows:** `C:\Users\<username>\STM32Cube\Repository\Packs\STMicroelectronics\X-CUBE-AI\<version>\Utilities\windows`
   - **Linux:** `~/STM32Cube/Repository/Packs/STMicroelectronics/X-CUBE-AI/<version>/Utilities/linux`
   - **macOS:** `~/STM32Cube/Repository/Packs/STMicroelectronics/X-CUBE-AI/<version>/Utilities/mac`

3. Add to PATH:
   - **Windows:** Add to system environment variables
   - **Linux/macOS:** Add to `~/.bashrc` or `~/.zshrc`:
     ```bash
     export PATH="$HOME/STM32Cube/Repository/Packs/STMicroelectronics/X-CUBE-AI/<version>/Utilities/linux:$PATH"
     export STEDGEAI_CORE_DIR="$HOME/STM32Cube/Repository/Packs/STMicroelectronics/X-CUBE-AI/<version>"
     ```

4. Configure environment variable (required):
   ```bash
   # Windows (PowerShell)
   $env:STEDGEAI_CORE_DIR = "C:\Users\<username>\STM32Cube\Repository\Packs\STMicroelectronics\X-CUBE-AI\<version>"
   
   # Linux/macOS (Bash)
   export STEDGEAI_CORE_DIR="$HOME/STM32Cube/Repository/Packs/STMicroelectronics/X-CUBE-AI/<version>"
   ```

5. Verify installation:
   ```bash
   stedgeai --version
   echo $STEDGEAI_CORE_DIR  # Ensure environment variable is set
   ```

**Note:** 
- `stedgeai` is **optional**, only needed for AI model regeneration
- If you only build firmware and Web, you don't need this tool
- Precompiled model files are included in the `bin/` directory

### 3. Verify Installation

**Use check script (recommended):**
```bash
./check_env.sh
```

**Manual check:**
```bash
# Essential tools
arm-none-eabi-gcc --version
arm-none-eabi-objcopy --version
make --version
python --version
node --version
pnpm --version

# Optional tools (flashing and signing)
STM32_Programmer_CLI --version
STM32_SigningTool_CLI --version

# Optional tools (AI model)
stedgeai --version
echo $STEDGEAI_CORE_DIR

# Or use project command
make info
```

**Expected output:**
```
arm-none-eabi-gcc (GNU Tools for STM32) 13.3.1
GNU Make 4.x
Python 3.x.x
v20.x.x (Node.js)
9.x.x (pnpm)
STM32_Programmer_CLI v2.19.0     # optional
STM32_SigningTool_CLI v2.19.0    # optional
stedgeai v2.2.0-20266 2adc00962  # optional (AI model tool)
```

## Configuration

### Method 1: Environment Variables

**Windows (PowerShell):**
```powershell
$env:PATH += ";C:\ST\STM32CubeCLT\GNU-tools-for-STM32\bin"
```

**Linux/macOS (Bash):**
```bash
export PATH="/opt/arm-gnu-toolchain/bin:$PATH"
```

### Method 2: .make.env File (Recommended)

Create `.make.env` in project root:

```makefile
# NE302 Makefile Configuration

# ARM GCC Toolchain path
GCC_PATH = C:/ST/STM32CubeCLT/GNU-tools-for-STM32/bin

# Parallel build jobs
MAKEFLAGS += -j20

# Optimization level
OPT = -Os -g3

# Flash addresses
FLASH_ADDR_FSBL = 0x34000000
FLASH_ADDR_APP = 0x70100000
FLASH_ADDR_WEB = 0x90400000
FLASH_ADDR_MODEL = 0x90700000

# ST Edge AI Core (for AI model generation)
export STEDGEAI_CORE_DIR=/path/to/STEdgeAI
```

**Note:** The setup script will auto-generate this file.

### Method 3: Makefile Command Line

```bash
make GCC_PATH=/path/to/toolchain/bin
make -j20  # Use 20 parallel jobs
```

## Troubleshooting

### Issue 1: arm-none-eabi-gcc not found

**Solution:**

```bash
# Check if installed
which arm-none-eabi-gcc

# If not found, install STM32CubeCLT or ARM GCC
# Then add to PATH or use GCC_PATH in .make.env
```

### Issue 2: Make not found (Windows)

**Solution:**

Install **Git for Windows** which includes Make:
- Download: https://git-scm.com/
- Use Git Bash terminal

Or install Make separately:
```bash
# Via Chocolatey
choco install make

# Via Scoop
scoop install make
```

### Issue 3: STM32_Programmer_CLI not found

**Solution:**

```bash
# 1. Download STM32CubeProgrammer
#    https://www.st.com/stm32cubeprog

# 2. Install and add to PATH
#    Windows: C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin
#    Linux: /usr/local/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin

# 3. Verify
STM32_Programmer_CLI --version
```

### Issue 4: STM32_SigningTool_CLI not found

**Solution:**

STM32_SigningTool_CLI is included in **STM32CubeCLT**:

```bash
# 1. Download STM32CubeCLT
#    https://www.st.com/stm32cubeclt

# 2. Installation location:
#    Windows: C:\ST\STM32CubeCLT\STM32_SigningTool_CLI\bin
#    Linux:   /opt/st/stm32cubeclt*/STM32_SigningTool_CLI/bin

# 3. Add to PATH (Windows example)
set PATH=%PATH%;C:\ST\STM32CubeCLT\STM32_SigningTool_CLI\bin

# 4. Verify
STM32_SigningTool_CLI --version
```

### Issue 5: USB driver issue (Windows)

**Solution:**
```bash
# ST-Link driver needs separate installation
# Download: https://www.st.com/en/development-tools/stsw-link009.html

# Or install automatically via STM32CubeProgrammer
```

### Issue 6: stedgeai not found

**Solution:**

```bash
# 1. Download ST Edge AI Core
#    https://www.st.com/en/development-tools/stedgeai-core.html#get-software

# 2. Install to default location

# 3. Set environment variable
export STEDGEAI_CORE_DIR="$HOME/STM32Cube/Repository/Packs/STMicroelectronics/X-CUBE-AI/<version>"

# 4. Add to PATH
export PATH="$STEDGEAI_CORE_DIR/Utilities/linux:$PATH"

# 5. Verify
stedgeai --version
```

**Note:** stedgeai is optional for basic firmware/web builds

### Issue 7: STEDGEAI_CORE_DIR not set

**Solution:**

```bash
# Find your ST Edge AI installation
# Usually in:
# - Windows: C:\Users\<username>\STM32Cube\Repository\Packs\STMicroelectronics\X-CUBE-AI\
# - Linux/macOS: ~/STM32Cube/Repository/Packs/STMicroelectronics/X-CUBE-AI/

# Set permanently:
# Windows (PowerShell):
[System.Environment]::SetEnvironmentVariable("STEDGEAI_CORE_DIR", "C:\path\to\X-CUBE-AI\version", [System.EnvironmentVariableTarget]::User)

# Linux/macOS (add to ~/.bashrc or ~/.zshrc):
export STEDGEAI_CORE_DIR="$HOME/STM32Cube/Repository/Packs/STMicroelectronics/X-CUBE-AI/<version>"
```

## Recommended Development Environment

- **IDE:** VS Code, STM32CubeIDE, or any text editor
- **Terminal:** 
  - Windows: PowerShell, Git Bash, or Windows Terminal
  - Linux/macOS: Default terminal
- **Debugger:** STM32CubeIDE with ST-Link

## Quick Test

After installation, verify the build system:

```bash
# Check environment
./check_env.sh

# Show build configuration
make info

# Test build (don't flash)
make -n

# Full build
make

# Build specific target
make app
make web
make model
```

**If all tools show [OK], you're ready to develop!** 🎉

For build commands, see: `README.md`
