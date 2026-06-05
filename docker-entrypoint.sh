#!/bin/bash
######################################
# Docker Entrypoint Script
# NE302 Development Environment
######################################

set -e

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}NE302 Docker Build Environment${NC}"
echo -e "${GREEN}=========================================${NC}"
echo ""

# Source profile scripts to load PATH
if [ -f /etc/profile.d/stedgeai.sh ]; then
    source /etc/profile.d/stedgeai.sh
fi

# Verify toolchain
echo "Verifying toolchain..."
if command -v arm-none-eabi-gcc &> /dev/null; then
    echo -e "${GREEN}✓ ARM GCC toolchain found${NC}"
    arm-none-eabi-gcc --version | head -n 1
else
    echo -e "${YELLOW}✗ ARM GCC toolchain not found${NC}"
    exit 1
fi

# Verify other tools
echo ""
echo "Checking dependencies..."
for tool in make python3 node pnpm; do
    if command -v $tool &> /dev/null; then
        echo -e "${GREEN}✓ $tool found${NC}"
        if [ "$tool" = "python3" ]; then
            python3 --version
        elif [ "$tool" = "node" ]; then
            node --version
        elif [ "$tool" = "pnpm" ]; then
            pnpm --version
        fi
    else
        echo -e "${YELLOW}✗ $tool not found${NC}"
    fi
done

# Check STM32 tools
echo ""
echo "Checking STM32 tools..."
if command -v STM32_Programmer_CLI &> /dev/null; then
    echo -e "${GREEN}✓ STM32_Programmer_CLI found${NC}"
    STM32_Programmer_CLI --version 2>/dev/null | head -n 1 || echo "  (version check failed)"
else
    echo -e "${YELLOW}✗ STM32_Programmer_CLI not found (optional)${NC}"
    echo "  Place SetupSTM32CubeProgrammer-*.linux in tools/ directory"
fi

if command -v STM32_SigningTool_CLI &> /dev/null; then
    echo -e "${GREEN}✓ STM32_SigningTool_CLI found${NC}"
    STM32_SigningTool_CLI --version 2>/dev/null | head -n 1 || echo "  (version check failed)"
else
    echo -e "${YELLOW}✗ STM32_SigningTool_CLI not found (optional)${NC}"
    echo "  Usually included in STM32CubeProgrammer installation"
fi

# Check stedgeai
echo ""
echo "Checking AI model tools..."
if command -v stedgeai &> /dev/null; then
    echo -e "${GREEN}✓ stedgeai found${NC}"
    stedgeai --version 2>/dev/null | head -n 1 || echo "  (version check failed)"
else
    echo -e "${YELLOW}✗ stedgeai not found (optional)${NC}"
    echo "  Place stedgeai-linux-onlineinstaller in tools/ directory"
fi

if [ -n "$STEDGEAI_CORE_DIR" ] && [ -d "$STEDGEAI_CORE_DIR" ]; then
    echo -e "${GREEN}✓ STEDGEAI_CORE_DIR = $STEDGEAI_CORE_DIR${NC}"
else
    echo -e "${YELLOW}✗ STEDGEAI_CORE_DIR not set or directory not found (optional)${NC}"
fi

# Display usage info
echo ""
echo -e "${GREEN}=========================================${NC}"
echo -e "${GREEN}Available Commands:${NC}"
echo -e "${GREEN}=========================================${NC}"
echo "  make              # Build all (FSBL + App + Web + Model)"
echo "  make fsbl         # Build FSBL only"
echo "  make app          # Build App only"
echo "  make web          # Build Web frontend"
echo "  make model        # Build AI model (requires stedgeai)"
echo "  make wakecore     # Build WakeCore"
echo "  make sign         # Sign firmware (requires STM32_SigningTool_CLI)"
echo "  make flash        # Flash to device (requires STM32_Programmer_CLI)"
echo "  make clean        # Clean all build files"
echo "  make help         # Show help"
echo ""
if command -v STM32_Programmer_CLI &> /dev/null && command -v STM32_SigningTool_CLI &> /dev/null; then
    echo -e "${GREEN}✓ Flashing and signing tools available${NC}"
else
    echo -e "${YELLOW}Note:${NC} Flashing requires STM32_Programmer_CLI"
    echo "  Signing requires STM32_SigningTool_CLI"
    echo "  Place SetupSTM32CubeProgrammer-*.linux in tools/ directory and rebuild image"
fi
if ! command -v stedgeai &> /dev/null; then
    echo -e "${YELLOW}Note:${NC} Model compilation requires stedgeai"
    echo "  Place stedgeai-linux-onlineinstaller in tools/ directory and rebuild image"
fi
echo ""
echo -e "${GREEN}=========================================${NC}"
echo ""

# Execute command
exec "$@"

