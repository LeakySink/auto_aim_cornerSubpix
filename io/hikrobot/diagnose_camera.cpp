#include <iostream>
#include <iomanip>
#include <libusb-1.0/libusb.h>
#include <MvCameraControl.h>
#include <unistd.h>

// 颜色输出
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_MAGENTA "\033[35m"

void print_header(const std::string & title)
{
  std::cout << "\n" << COLOR_BLUE << "========== " << title << " ==========" << COLOR_RESET << "\n";
}

void print_success(const std::string & msg)
{
  std::cout << COLOR_GREEN << "✓ " << msg << COLOR_RESET << "\n";
}

void print_error(const std::string & msg)
{
  std::cout << COLOR_RED << "✗ " << msg << COLOR_RESET << "\n";
}

void print_warning(const std::string & msg)
{
  std::cout << COLOR_YELLOW << "⚠ " << msg << COLOR_RESET << "\n";
}

void print_info(const std::string & msg)
{
  std::cout << "  " << msg << "\n";
}

// 检查USB设备列表
int check_usb_devices(int target_vid, int target_pid)
{
  print_header("USB设备扫描");

  libusb_device ** devs;
  ssize_t cnt;

  cnt = libusb_get_device_list(NULL, &devs);
  if (cnt < 0) {
    print_error("无法获取USB设备列表");
    return -1;
  }

  bool found = false;
  libusb_device * found_dev = NULL;

  for (ssize_t i = 0; i < cnt; i++) {
    libusb_device * dev = devs[i];
    struct libusb_device_descriptor desc;

    if (libusb_get_device_descriptor(dev, &desc) < 0) {
      continue;
    }

    // 打印所有海康机器人设备
    if (desc.idVendor == 0x2bdf) {
      found = true;
      found_dev = dev;

      std::cout << COLOR_GREEN;
      std::cout << "找到海康相机设备!\n";
      std::cout << COLOR_RESET;

      std::cout << "  VID:PID = " << std::hex << std::setw(4) << std::setfill('0')
                << desc.idVendor << ":" << std::setw(4) << desc.idProduct << std::dec << "\n";

      // 检查是否是目标设备
      if (desc.idVendor == target_vid && desc.idProduct == target_pid) {
        print_success("这是配置文件中指定的相机 (2bdf:0001)");
      } else {
        print_warning("PID不匹配! 配置文件期望: " + std::to_string(target_pid));
      }

      // 获取设备详细信息
      libusb_device_handle * handle;
      if (libusb_open(dev, &handle) == 0) {
        unsigned char bus = libusb_get_bus_number(dev);
        unsigned char address = libusb_get_device_address(dev);

        std::cout << "  总线号: " << (int)bus << "\n";
        std::cout << "  设备地址: " << (int)address << "\n";

        // 尝试获取速度
        int speed = libusb_get_device_speed(dev);
        const char * speed_str[] = {"未知", "低速", "全速", "高速", "超速", "超速+"};
        if (speed >= 0 && speed <= 5) {
          std::cout << "  USB速度: " << speed_str[speed];
          if (speed < LIBUSB_SPEED_HIGH) {
            std::cout << COLOR_RED << " (速度较慢，可能导致相机性能下降)" << COLOR_RESET;
          }
          std::cout << "\n";
        }

        libusb_close(handle);
      } else {
        print_error("无法打开设备 (可能需要权限或被其他程序占用)");
      }

      std::cout << "\n";
    }
  }

  libusb_free_device_list(devs, 1);

  if (!found) {
    print_error("未找到海康机器人相机 (VID: 2bdf)");
    std::cout << "\n排查建议:\n";
    std::cout << "  1. 检查USB线缆是否连接牢固\n";
    std::cout << "  2. 尝试更换USB线缆（建议使用高质量线缆）\n";
    std::cout << "  3. 尝试更换USB端口（USB 3.0端口为蓝色）\n";
    std::cout << "  4. 检查Type-C集线器是否供电充足\n";
    return -1;
  }

  return 0;
}

// 检查USB权限
int check_usb_permissions(int vid, int pid)
{
  print_header("USB权限检查");

  libusb_device_handle * handle = libusb_open_device_with_vid_pid(NULL, vid, pid);

  if (!handle) {
    print_error("无法打开相机设备");
    std::cout << "\n可能的原因:\n";
    std::cout << "  1. 缺少USB设备访问权限\n";
    std::cout << "  2. 设备被其他程序占用\n";
    std::cout << "\n解决方法:\n";
    std::cout << "  创建udev规则: /etc/udev/rules.d/50-hikrobot.rules\n";
    std::cout << "  内容: SUBSYSTEM==\"usb\", ATTR{idVendor}==\"2bdf\", MODE=\"0666\"\n";
    std::cout << "  然后运行: sudo udevadm control --reload-rules\n";
    std::cout << "  重新插拔相机\n";
    return -1;
  }

  print_success("有USB设备访问权限");

  // 尝试断开内核驱动
  if (libusb_kernel_driver_active(handle, 0)) {
    print_warning("内核驱动已占用设备，尝试断开...");
    if (libusb_detach_kernel_driver(handle, 0) == 0) {
      print_success("成功断开内核驱动");
    } else {
      print_error("无法断开内核驱动");
    }
  }

  libusb_close(handle);
  return 0;
}

// 测试海康SDK连接
int test_hik_sdk()
{
  print_header("海康SDK连接测试");

  unsigned int ret;
  MV_CC_DEVICE_INFO_LIST device_list{};

  ret = MV_CC_EnumDevices(MV_USB_DEVICE, &device_list);
  if (ret != MV_OK) {
    print_error("枚举设备失败, 错误码: " + std::to_string(ret));
    return -1;
  }

  if (device_list.nDeviceNum == 0) {
    print_error("SDK未发现任何相机");
    std::cout << "\n可能原因:\n";
    std::cout << "  1. USB驱动未正确安装\n";
    std::cout << "  2. 相机红灯表示硬件异常\n";
    std::cout << "  3. USB线缆或端口问题\n";
    return -1;
  }

  print_success("SDK发现 " + std::to_string(device_list.nDeviceNum) + " 个相机");

  // 尝试打开相机
  void * handle = NULL;
  ret = MV_CC_CreateHandle(&handle, device_list.pDeviceInfo[0]);
  if (ret != MV_OK) {
    print_error("创建句柄失败, 错误码: " + std::to_string(ret));
    return -1;
  }
  print_success("创建相机句柄成功");

  ret = MV_CC_OpenDevice(handle);
  if (ret != MV_OK) {
    print_error("打开相机失败, 错误码: " + std::to_string(ret));
    MV_CC_DestroyHandle(handle);

    std::cout << "\n常见错误码:\n";
    std::cout << "  0x80000003: 设备被占用或权限不足\n";
    std::cout << "  0x80000013: USB通信异常\n";
    std::cout << "  0x80000015: 设备未就绪（红灯状态）\n";
    return -1;
  }
  print_success("打开相机成功");

  // 获取相机信息
  MV_CC_DEVICE_INFO * devInfo = device_list.pDeviceInfo[0];
  if (devInfo) {
    std::string model = (char *)devInfo->SpecialInfo.stUsb3VInfo.chModelName;
    std::string serial = (char *)devInfo->SpecialInfo.stUsb3VInfo.chSerialNumber;
    print_info("相机型号: " + model);
    print_info("序列号: " + serial);
  }

  // 尝试获取一帧图像
  ret = MV_CC_StartGrabbing(handle);
  if (ret != MV_OK) {
    print_error("开始采集失败, 错误码: " + std::to_string(ret));
    MV_CC_CloseDevice(handle);
    MV_CC_DestroyHandle(handle);
    return -1;
  }
  print_success("开始采集成功");

  MV_FRAME_OUT raw;
  unsigned int nMsec = 2000;

  std::cout << "\n等待图像...\n";
  ret = MV_CC_GetImageBuffer(handle, &raw, nMsec);

  if (ret != MV_OK) {
    print_error("获取图像失败, 错误码: " + std::to_string(ret));

    if (ret == 0x80000013) {
      std::cout << "\nUSB通信异常 - 可能原因:\n";
      std::cout << "  1. USB带宽不足\n";
      std::cout << "  2. USB线缆质量差\n";
      std::cout << "  3. 端口不是USB 3.0\n";
    }
  } else {
    print_success("成功获取图像 (" + std::to_string(raw.stFrameInfo.nWidth) + "x" +
                 std::to_string(raw.stFrameInfo.nHeight) + ")");
    MV_CC_FreeImageBuffer(handle, &raw);
  }

  MV_CC_StopGrabbing(handle);
  MV_CC_CloseDevice(handle);
  MV_CC_DestroyHandle(handle);

  return 0;
}

// 重置USB设备
int reset_usb_device(int vid, int pid)
{
  print_header("USB设备重置");

  libusb_device_handle * handle = libusb_open_device_with_vid_pid(NULL, vid, pid);
  if (!handle) {
    print_error("无法打开设备进行重置");
    return -1;
  }

  std::cout << "重置USB设备...\n";
  if (libusb_reset_device(handle) == 0) {
    print_success("USB设备重置成功");
    std::cout << "\n请观察相机状态:\n";
    std::cout << "  - 蓝灯闪烁: 正常初始化\n";
    std::cout << "  - 绿灯常亮: 已连接就绪\n";
    std::cout << "  - 红灯: 仍有问题\n";
  } else {
    print_error("USB设备重置失败");
  }

  libusb_close(handle);
  return 0;
}

int main(int argc, char ** argv)
{
  std::cout << COLOR_MAGENTA;
  std::cout << "\n╔════════════════════════════════════════╗\n";
  std::cout << "║   海康CS106相机诊断工具               ║\n";
  std::cout << "╚════════════════════════════════════════╝\n";
  std::cout << COLOR_RESET;

  // 解析参数
  int vid = 0x2bdf;
  int pid = 0x0001;
  bool do_reset = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--reset") {
      do_reset = true;
    } else if (arg == "--help") {
      std::cout << "用法: " << argv[0] << " [--reset] [--help]\n";
      std::cout << "  --reset  : 重置USB设备\n";
      std::cout << "  --help   : 显示帮助\n";
      return 0;
    }
  }

  // 初始化libusb
  if (libusb_init(NULL) != 0) {
    print_error("无法初始化libusb");
    return -1;
  }

  // 1. 扫描USB设备
  if (check_usb_devices(vid, pid) != 0 && !do_reset) {
    libusb_exit(NULL);
    return -1;
  }

  // 2. 检查权限
  if (check_usb_permissions(vid, pid) != 0 && !do_reset) {
    libusb_exit(NULL);
    return -1;
  }

  // 3. 重置设备（如果请求）
  if (do_reset) {
    reset_usb_device(vid, pid);
    std::cout << "\n等待3秒后重新检测...\n";
    sleep(3);
    if (check_usb_devices(vid, pid) != 0) {
      libusb_exit(NULL);
      return -1;
    }
  }

  // 4. 测试SDK连接
  test_hik_sdk();

  libusb_exit(NULL);

  print_header("诊断完成");
  std::cout << "\n如果问题仍然存在:\n";
  std::cout << "  1. 运行: sudo dmesg | tail -20  查看内核日志\n";
  std::cout << "  2. 运行: lsusb -v -d 2bdf:0001  查看USB详细信息\n";
  std::cout << "  3. 尝试使用官方海康客户端工具测试相机\n";
  std::cout << "  4. 更换已知的USB 3.0线缆和端口\n";

  return 0;
}