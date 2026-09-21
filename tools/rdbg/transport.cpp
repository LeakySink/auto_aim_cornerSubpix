#include "transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

namespace tools
{
namespace rdbg
{

bool UdpSocket::bind(uint16_t port, bool broadcast, bool reuse_port)
{
  close();
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) return false;
  int yes = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (reuse_port) {
#ifdef SO_REUSEPORT
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
  }
  if (broadcast) {
    ::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(port);
  if (::bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
    close();
    return false;
  }
  return true;
}

void UdpSocket::close()
{
  if (fd_ < 0) return;
  ::close(fd_);
  fd_ = -1;
}

ssize_t UdpSocket::recvfrom_nb(void * buf, size_t len, sockaddr_in * from)
{
  if (fd_ < 0) return -1;
  sockaddr_in src{};
  socklen_t flen = sizeof(src);
  ssize_t n = ::recvfrom(fd_, buf, len, MSG_DONTWAIT,
                         reinterpret_cast<sockaddr *>(&src), &flen);
  if (n > 0 && from) *from = src;
  return n;
}

bool UdpSocket::sendto(const void * data, size_t len, const sockaddr_in & dest) const
{
  if (fd_ < 0) return false;
  return ::sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr *>(&dest),
                  sizeof(dest)) >= 0;
}

bool UdpSocket::sendmsg(const struct iovec * iov, int iovcnt,
                        const sockaddr_in & dest) const
{
  if (fd_ < 0 || !iov || iovcnt <= 0) return false;
  msghdr msg{};
  msg.msg_name = const_cast<sockaddr_in *>(&dest);
  msg.msg_namelen = sizeof(dest);
  msg.msg_iov = const_cast<struct iovec *>(iov);
  msg.msg_iovlen = static_cast<size_t>(iovcnt);
  return ::sendmsg(fd_, &msg, 0) >= 0;
}

bool UdpSocket::sendto(const void * data, size_t len, in_addr ip, uint16_t port) const
{
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_addr = ip;
  dest.sin_port = htons(port);
  return sendto(data, len, dest);
}

std::string ip_string(in_addr ip)
{
  char buf[INET_ADDRSTRLEN] = {};
  ::inet_ntop(AF_INET, &ip, buf, sizeof(buf));
  return buf;
}

sockaddr_in broadcast_addr(uint16_t port)
{
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  dest.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  return dest;
}

std::string local_ipv4()
{
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd >= 0) {
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(53);
    ::inet_aton("8.8.8.8", &dest.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&dest), sizeof(dest)) == 0) {
      sockaddr_in src{};
      socklen_t n = sizeof(src);
      if (::getsockname(fd, reinterpret_cast<sockaddr *>(&src), &n) == 0) {
        ::close(fd);
        auto s = ip_string(src.sin_addr);
        if (!s.empty() && s != "0.0.0.0") return s;
      } else {
        ::close(fd);
      }
    } else {
      ::close(fd);
    }
  }

  ifaddrs * ifa = nullptr;
  if (::getifaddrs(&ifa) != 0) return {};
  std::string out;
  for (ifaddrs * p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
    if (p->ifa_flags & IFF_LOOPBACK) continue;
    if (!(p->ifa_flags & IFF_UP)) continue;
    auto * in = reinterpret_cast<sockaddr_in *>(p->ifa_addr);
    out = ip_string(in->sin_addr);
    if (!out.empty()) break;
  }
  ::freeifaddrs(ifa);
  return out;
}

}  // namespace rdbg
}  // namespace tools
