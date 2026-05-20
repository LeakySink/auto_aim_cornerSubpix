#!/bin/bash

# CLion 社区版安装脚本（Ubuntu 22.04 x86_64）
# 通过 snap 安装 CLion Community Edition（免费，无需许可证）

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
echo "║   CLion 社区版安装工具                 ║"
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

if snap list clion &>/dev/null 2>&1; then
    VER=$(snap list clion | awk 'NR==2{print $2}')
    print_success "CLion 社区版已安装，版本 $VER"
    echo -e "\n${CYAN}如需更新: sudo snap refresh clion${RESET}"
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
    sudo systemctl enable --now snapd.socket 2>/dev/null || true
    sudo systemctl start snapd 2>/dev/null || true
    sleep 2
    print_success "snapd 安装完成"
else
    print_success "snapd 已可用"
fi

# ============================================================
# 3. 安装构建工具依赖（CLion 需要）
# ============================================================
print_header "3. 安装构建工具"

print_info "安装 cmake / ninja / gcc / gdb..."
sudo apt update -qq
sudo apt install -y cmake ninja-build gcc g++ gdb
print_success "构建工具安装完成"

# ============================================================
# 4. 安装 CLion 社区版
# ============================================================
print_header "4. 安装 CLion 社区版"

print_info "从 snap store 安装 clion（--classic 模式，允许访问系统文件）..."
sudo snap install clion --classic
print_success "CLion 社区版安装完成"

# ============================================================
# 5. 验证
# ============================================================
print_header "5. 验证安装"

if snap list clion &>/dev/null 2>&1; then
    VER=$(snap list clion | awk 'NR==2{print $2}')
    print_success "CLion $VER 安装成功"
else
    print_error "安装验证失败，请检查上方错误信息"
    exit 1
fi

# ============================================================
# 6. 使用说明
# ============================================================
print_header "使用说明"

echo -e "${CYAN}启动 CLion:${RESET}"
echo -e "  clion"
echo -e "  或在应用菜单搜索 CLion"
echo -e "\n${CYAN}打开本项目:${RESET}"
echo -e "  clion /home/baiye/auto_aim_"
echo -e "  CLion 会自动识别 CMakeLists.txt 并配置项目"
echo -e "\n${CYAN}推荐 CMake 配置（Settings → Build → CMake）:${RESET}"
echo -e "  Build type: ${GREEN}RelWithDebInfo${RESET}"
echo -e "  Generator:  ${GREEN}Ninja${RESET}"
echo -e "  Build dir:  ${GREEN}build${RESET}"
echo -e "\n${CYAN}更新:${RESET}"
echo -e "  sudo snap refresh clion"

print_success "脚本执行完成！"
