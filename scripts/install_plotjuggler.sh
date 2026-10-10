#!/bin/bash

# PlotJuggler 独立版安装脚本（Ubuntu 22.04 x86_64，无需 ROS）
# 通过 snap 安装，用于实时可视化云台轨迹（UDP 端口 9870）

set -e

RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
BLUE='\033[34m'
CYAN='\033[36m'
MAGENTA='\033[35m'
RESET='\033[0m'

echo -e "${MAGENTA}"
echo "╔════════════════════════════════════════╗"
echo "║   PlotJuggler 安装工具                 ║"
echo "╚════════════════════════════════════════╝"
echo -e "${RESET}"

print_header() { echo -e "\n${BLUE}========== $1 ==========${RESET}\n"; }
print_success() { echo -e "${GREEN}✓ $1${RESET}"; }
print_error()   { echo -e "${RED}✗ $1${RESET}"; }
print_warning() { echo -e "${YELLOW}⚠ $1${RESET}"; }
print_info()    { echo -e "  $1"; }

# ============================================================
# 1. 检查是否已安装
# ============================================================
print_header "1. 检查现有安装"

if snap list plotjuggler &>/dev/null 2>&1; then
    VER=$(snap list plotjuggler | awk 'NR==2{print $2}')
    print_success "PlotJuggler 已安装，版本 $VER"
    echo -e "\n${CYAN}如需更新: sudo snap refresh plotjuggler${RESET}"
    exit 0
fi

# ============================================================
# 2. 确保 snapd 可用
# ============================================================
print_header "2. 检查 snapd"

if ! command -v snap &>/dev/null; then
    print_info "安装 snapd..."
    sudo apt update -qq
    sudo apt install -y snapd
    print_success "snapd 安装完成"

    # snapd socket 需要重启后才完全可用，尝试手动启动
    sudo systemctl enable --now snapd.socket 2>/dev/null || true
    sudo systemctl start snapd 2>/dev/null || true
    sleep 2
else
    print_success "snapd 已可用"
fi

# ============================================================
# 3. 安装 PlotJuggler
# ============================================================
print_header "3. 安装 PlotJuggler"

print_info "从 snap store 安装 PlotJuggler..."
sudo snap install plotjuggler
print_success "PlotJuggler 安装完成"

# ============================================================
# 4. 验证
# ============================================================
print_header "4. 验证安装"

if snap list plotjuggler &>/dev/null 2>&1; then
    VER=$(snap list plotjuggler | awk 'NR==2{print $2}')
    print_success "PlotJuggler $VER 安装成功"
else
    print_error "安装验证失败，请检查上方错误信息"
    exit 1
fi

# ============================================================
# 5. 使用说明
# ============================================================
print_header "使用说明"

echo -e "${CYAN}启动 PlotJuggler:${RESET}"
echo -e "  plotjuggler"
echo -e "\n${CYAN}加载项目布局文件:${RESET}"
echo -e "  plotjuggler --layout /home/baiye/auto_aim_/mpc_layout.xml"
echo -e "  plotjuggler --layout /home/baiye/auto_aim_/buff_layout.xml"
echo -e "\n${CYAN}接收实时数据（Plotter 工具通过 UDP 发送）:${RESET}"
echo -e "  启动后菜单 Streaming → Start UDP Server，端口填 ${GREEN}9870${RESET}"

print_success "脚本执行完成！"
