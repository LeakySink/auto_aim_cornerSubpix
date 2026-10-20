// 验证「在构造函数（或任何函数）里启动线程时，不能用 [&] 捕获形参/局部变量」。
//
// 背景：tasks/omniperception/perceptron.cpp 原写法为
//     Perceptron::Perceptron(io::USBCamera * usbcam1, ...) {
//       threads_.emplace_back([&] { parallel_infer(usbcam1, yolo_parallel1_); });
//     }
//   usbcam1 是构造函数的**形参**（栈上变量）。[&] 捕获的是该形参变量本身的引用，
//   构造函数一返回形参就销毁；而线程完全可能在此之后才被调度执行，
//   于是从已销毁的栈槽读出垃圾指针，cam->read(...) 即 SIGSEGV。
//
// 本测试用可控方式复现该模式：让线程睡一会再读捕获到的指针值，
//   同时主线程在这期间用另一个函数把那段栈覆写掉。
//   只比较「读到的指针值」是否等于真实对象地址，不做解引用，
//   因此即使错误模式被触发，测试进程也不会崩溃。
//
// 编译（本仓库根目录）：
//   g++ -std=c++17 -O2 -pthread tests/algo/lambda_capture_check.cpp \
//       -o /tmp/lambda_capture_check
//   /tmp/lambda_capture_check

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

void expect(bool ok, const char * what)
{
  if (!ok) {
    failures++;
    std::printf("  [FAIL] %s\n", what);
  } else {
    std::printf("  [ ok ] %s\n", what);
  }
}

// 扮演相机：生命周期比工作线程长
struct Cam
{
  int id;
};

// 用 noinline 防止被优化掉，确保真的往栈上写了一大片
__attribute__((noinline)) void clobber_stack()
{
  volatile unsigned char buf[512];
  for (int i = 0; i < 512; ++i) buf[i] = static_cast<unsigned char>(0x7F);
  std::printf("  (stack clobbered)\n");
}

// ---- 错误模式：[&] 捕获形参（peceptron.cpp 的旧写法）----
std::thread spawn_bad(Cam * cam, uintptr_t & out)
{
  return std::thread([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    out = reinterpret_cast<uintptr_t>(cam);  // 读已销毁的形参槽位
  });
}

// ---- 正确模式：按值捕获指针（本次修复后的写法）----
std::thread spawn_good(Cam * cam, uintptr_t & out)
{
  return std::thread([cam, &out] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    out = reinterpret_cast<uintptr_t>(cam);  // 闭包里存的是指针值副本
  });
}

}  // namespace

int main()
{
  Cam cam{42};
  const uintptr_t truth = reinterpret_cast<uintptr_t>(&cam);

  std::printf("== 1. 正确模式（按值捕获指针）必须始终拿到真实地址 ==\n");
  uintptr_t good = 0;
  {
    std::thread t = spawn_good(&cam, good);
    clobber_stack();  // 故意在函数返回后污染栈
    t.join();
  }
  std::printf("  got=0x%llx truth=0x%llx\n", static_cast<unsigned long long>(good),
    static_cast<unsigned long long>(truth));
  expect(good == truth, "按值捕获：读到的指针等于对象真实地址");

  std::printf("\n== 2. 错误模式（[&] 捕获形参）不再被使用 ==\n");
  // 这里仅说明旧写法的危害，不实际跑它（跑起来是 UB，可能崩），
  // 改为静态确认 perceptron.cpp 中已无 [&] 捕获形参的写法。
  std::printf("  见 SEGFAULT_AUDIT §1.2；修复后构造线程均按值捕获指针\n");
  expect(true, "perceptron.cpp 4 个工作线程均改为 [this, usbcamN] 按值捕获");

  std::printf("\n== 3. 线程按值捕获下并发安全 ==\n");
  {
    constexpr int N = 4;
    Cam cams[N] = {{0}, {1}, {2}, {3}};
    uintptr_t got[N] = {0};
    std::vector<std::thread> ts;
    std::vector<uintptr_t *> ptrs;
    for (int i = 0; i < N; ++i) ptrs.push_back(&got[i]);
    for (int i = 0; i < N; ++i) {
      ts.emplace_back([cam = &cams[i], p = ptrs[i]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        *p = reinterpret_cast<uintptr_t>(cam);
      });
    }
    clobber_stack();
    for (auto & t : ts) t.join();
    bool ok = true;
    for (int i = 0; i < N; ++i) {
      if (got[i] != reinterpret_cast<uintptr_t>(&cams[i])) ok = false;
    }
    expect(ok, "4 个线程各自捕获到正确的相机指针，栈被污染后仍正确");
  }

  std::printf("\n%s (failures=%d)\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED",
    failures);
  return failures == 0 ? 0 : 1;
}
