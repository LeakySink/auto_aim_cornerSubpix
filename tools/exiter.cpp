#include "exiter.hpp"

#include <csignal>
#include <stdexcept>

namespace tools
{
// 信号处理函数里只能安全读写 volatile std::sig_atomic_t；普通 bool 允许编译器
// 把它缓存在寄存器里、也允许重排写入，SIGINT 到达时主循环可能永远看不到这次
// 写入 —— 表现就是按 Ctrl-C 没反应、进程只能被 SIGKILL 掉。
volatile std::sig_atomic_t exit_ = 0;
bool exiter_inited_ = false;

Exiter::Exiter()
{
  if (exiter_inited_) throw std::runtime_error("Multiple Exiter instances!");
  std::signal(SIGINT, [](int) { exit_ = 1; });
  std::signal(SIGTERM, [](int) { exit_ = 1; });
  exiter_inited_ = true;
}

bool Exiter::exit() const { return exit_ != 0; }

}  // namespace tools