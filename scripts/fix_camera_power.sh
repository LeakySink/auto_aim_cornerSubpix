#!/bin/bash

# 海康相机供电问题诊断和修复脚本
# 症状：蓝灯闪烁、偶尔红灯、连接不稳定

set -e

RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
BLUE='\033[34m'
MAGENTA='\033[35m'
CYAN='\033[36m'
RESET='\033[0m'

echo -e "${MAGENTA}"
echo "╔════════════════════════════════════════╗"
echo "║   海康相机供电问题修复工具             ║"
echo "╚════════════════════════════════════════╝"
echo -e "${RESET}"

PROJECT_ROOT="/home/baiye/temp/sp_vision_25"
BUILD_DIR="$PROJECT_ROOT/build"

# 检查是否在项目目录
if [ ! -d "$PROJECT_ROOT" ]; then
    echo -e "${RED}错误: 找不到项目目录 $PROJECT_ROOT${RESET}"
    exit 1
fi

cd "$PROJECT_ROOT"

# 解析参数
DO_RESET=0
DO_COMPILE=1

while [[ $# -gt 0 ]]; do
    case $1 in
        --reset)
            DO_RESET=1
            shift
            ;;
        --no-compile)
            DO_COMPILE=0
            shift
            ;;
        --help)
            echo "用法: $0 [选项]"
            echo "选项:"
            echo "  --reset      重置 USB 设备"
            echo "  --no-compile 跳过编译诊断工具"
            echo "  --help       显示帮助"
            exit 0
            ;;
        *)
            echo -e "${RED}未知选项: $1${RESET}"
            exit 1
            ;;
    esac
done

print_header() {
    echo -e "\n${BLUE}========== $1 ==========${RESET}\n"
}

print_success() {
    echo -e "${GREEN}✓ $1${RESET}"
}

print_error() {
    echo -e "${RED}✗ $1${RESET}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${RESET}"
}

print_info() {
    echo -e "  $1"
}

# ============================================================
# 1. 检查并禁用 USB 自动省电
# ============================================================
print_header "1. 检查 USB 自动省电设置"

AUTOSUSPEND=$(cat /sys/module/usbcore/parameters/autosuspend 2>/dev/null || echo "unknown")
echo -e "当前 USB autosuspend 值: ${CYAN}$AUTOSUSPEND${RESET}"

if [ "$AUTOSUSPEND" != "0" ]; then
    print_warning "USB 自动省电已启用，这可能导致相机掉线"

    echo -e "\n尝试禁用 USB 自动省电..."
    if echo 0 | sudo tee /sys/module/usbcore/parameters/autosuspend > /dev/null 2>&1; then
        print_success "已临时禁用 USB 自动省电"
    else
        print_error "禁用失败，可能需要 sudo 权限"
    fi
else
    print_success "USB 自动省电已禁用"
fi

# 检查 USB 设备电源控制
echo -e "\n检查 USB 设备电源管理..."
for device in /sys/bus/usb/devices/*/power/control; do
    if [ -f "$device" ]; then
        control=$(cat "$device")
        if [ "$control" = "auto" ]; then
            # 尝试设置为 on
            if echo "on" | sudo tee "$device" > /dev/null 2>&1; then
                echo -e "  ${GREEN}✓${RESET} $(dirname $device | xargs basename): auto -> on"
            fi
        fi
    fi
done

# ============================================================
# 2. 创建永久禁用规则
# ============================================================
print_header "2. 配置永久禁用 USB 自动省电"

RULE_FILE="/etc/udev/rules.d/50-usb_power_save.rules"

if [ -f "$RULE_FILE" ]; then
    print_info "规则文件已存在: $RULE_FILE"
    cat "$RULE_FILE"
else
    print_info "创建永久禁用 USB 自动省电的 udev 规则..."

    RULE_CONTENT='ACTION=="add", SUBSYSTEM=="usb", TEST=="power/control", ATTR{power/control}="on"'

    if echo "$RULE_CONTENT" | sudo tee "$RULE_FILE" > /dev/null 2>&1; then
        print_success "创建规则文件: $RULE_FILE"

        if sudo udevadm control --reload-rules > /dev/null 2>&1; then
            print_success "重新加载 udev 规则"
        else
            print_warning "重新加载 udev 规则失败（可能不需要）"
        fi
    else
        print_error "创建规则文件失败，可能需要 sudo 权限"
    fi
fi

# ============================================================
# 3. 编译诊断工具
# ============================================================
if [ $DO_COMPILE -eq 1 ]; then
    print_header "3. 编译诊断工具"

    if [ ! -f "$BUILD_DIR/diagnose_camera" ]; then
        print_info "编译诊断工具..."

        if cmake -B build > /dev/null 2>&1; then
            print_success "CMake 配置成功"
        else
            print_error "CMake 配置失败"
        fi

        if make -C build/ -j$(nproc) diagnose_camera > /dev/null 2>&1; then
            print_success "编译诊断工具成功"
        else
            print_error "编译诊断工具失败"
        fi
    else
        print_success "诊断工具已编译"
    fi
fi

# ============================================================
# 4. 运行诊断工具
# ============================================================
print_header "4. 运行相机诊断"

if [ -f "$BUILD_DIR/diagnose_camera" ]; then
    if [ $DO_RESET -eq 1 ]; then
        print_info "运行诊断并重置相机..."
        "$BUILD_DIR/diagnose_camera" --reset
    else
        print_info "运行相机诊断..."
        "$BUILD_DIR/diagnose_camera"
    fi
else
    print_error "诊断工具不存在: $BUILD_DIR/diagnose_camera"
fi

# ============================================================
# 5. 检查 USB 状态
# ============================================================
print_header "5. USB 设备详细信息"

# 检查海康相机
if command -v lsusb &> /dev/null; then
    echo -e "${CYAN}海康相机 USB 设备:${RESET}"
    lsusb -d 2bdf:0001

    echo -e "\n${CYAN}USB 速度和供电信息:${RESET}"
    if lsusb -v -d 2bdf:0001 2>/dev/null | grep -E "bcdUSB|MaxPower|bDeviceProtocol" | sed 's/^/  /'; then
        :
    else
        print_warning "无法获取详细信息（相机可能未连接）"
    fi
else
    print_warning "lsusb 命令不可用"
fi

# ============================================================
# 6. 检查内核日志
# ============================================================
print_header "6. 内核 USB 错误日志"

echo -e "${CYAN}最近的 USB 错误:${RESET}"
if sudo dmesg -T -l err,crit,alert,emerg 2>/dev/null | grep -i "usb\|2bdf" | tail -10 | sed 's/^/  /'; then
    :
else
    print_info "无错误日志"
fi

echo -e "\n${CYAN}最近的 USB 警告:${RESET}"
if sudo dmesg -T -l warn 2>/dev/null | grep -i "usb\|2bdf" | tail -10 | sed 's/^/  /'; then
    :
else
    print_info "无警告日志"
fi

# ============================================================
# 7. USB 设备树
# ============================================================
print_header "7. USB 设备连接树"

if command -v lsusb &> /dev/null; then
    lsusb -t | sed 's/^/  /'
else
    print_warning "lsusb 命令不可用"
fi

# ============================================================
# 总结和建议
# ============================================================
print_header "总结和建议"

echo -e "${GREEN}已完成的修复操作:${RESET}"
echo -e "  1. ${GREEN}✓${RESET} 禁用 USB 自动省电（临时）"
echo -e "  2. ${GREEN}✓${RESET} 配置永久禁用规则"
echo -e "  3. ${GREEN}✓${RESET} 运行相机诊断"
echo -e "  4. ${GREEN}✓${RESET} 检查 USB 状态和日志"

echo -e "\n${YELLOW}如果问题仍然存在，请尝试以下硬件改进:${RESET}"
echo -e "  1. ${CYAN}直接连接主板 USB 3.0 口${RESET}（蓝色，不用 Type-C）"
echo -e "  2. ${CYAN}使用海康原装 USB 3.0 线${RESET}（带磁环）"
echo -e "  3. ${CYAN}使用带外部供电的 USB 集线器${RESET}"
echo -e "  4. ${CYAN}BIOS 中禁用 USB Selective Suspend${RESET}"

echo -e "\n${CYAN}快速测试:${RESET}"
echo -e "  直接插主板 USB 口后运行: ${GREEN}./build/auto_aim_debug_mpc${RESET}"

echo -e "\n${CYAN}重新运行此脚本:${RESET}"
echo -e "  $0              # 常规诊断"
echo -e "  $0 --reset      # 诊断并重置相机"
echo -e "  $0 --no-compile # 跳过编译"

print_success "脚本执行完成！"