#ifndef TOOLS_RDBG_CLOCK_HPP
#define TOOLS_RDBG_CLOCK_HPP

#include <chrono>
#include <cstdint>

namespace tools
{
namespace rdbg
{

inline uint64_t now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

}  // namespace rdbg
}  // namespace tools

#endif
