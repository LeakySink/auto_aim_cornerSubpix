// 多车协议自检（host/PROTOCOL.md）：
// 每辆车自己的 control 口 + beacon name；host 注册时分配不同的 data/peer 口。
// 数据只应出现在该车被分配的 data_port 上，且 JSON `_from` / 图像 0xFE 里的名字一致。
// 图像必须在 img_subscribe 之后才出现。

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace
{

constexpr uint16_t kDiscoverPort = 15999;
constexpr uint16_t kDataBase = 15001;
constexpr uint16_t kPeerBase = 15100;
constexpr uint16_t kCtrlBase = 16000;
constexpr int kImgFrag = 0xFE;

const char * kHostId = "multi-sender-test";
const char * kNames[] = {"robot_alpha", "robot_beta", "robot_gamma"};

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Robot
{
  std::string name;
  uint16_t control = 0;
  uint16_t data_port = 0;
  uint16_t peer_port = 0;
  int data_fd = -1;
  int peer_fd = -1;
  pid_t pid = -1;
  std::string ip;
  bool beacon = false;
  bool ack = false;
  bool plot = false;
  bool image = false;
  bool subscribed = false;
  int foreign = 0;
  int image_early = 0;
  std::chrono::steady_clock::time_point last_alive{};
  std::chrono::steady_clock::time_point last_register{};
};

int bind_udp(uint16_t port, bool broadcast)
{
  int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int yes = 1;
  if (broadcast)
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int take_port(uint16_t start, uint16_t * port_out)
{
  for (int i = 0; i < 200; i++) {
    uint16_t port = static_cast<uint16_t>(start + i);
    if (port == kDiscoverPort) continue;
    int fd = bind_udp(port, false);
    if (fd >= 0) {
      *port_out = port;
      return fd;
    }
  }
  return -1;
}

uint16_t reserve_ctrl_port(uint16_t start)
{
  for (int i = 0; i < 200; i++) {
    uint16_t port = static_cast<uint16_t>(start + i);
    int fd = bind_udp(port, false);
    if (fd >= 0) {
      ::close(fd);
      return port;
    }
  }
  return 0;
}

void send_json(const std::string & ip, uint16_t port, const nlohmann::json & j)
{
  if (ip.empty() || port == 0) return;
  sockaddr_in dest{};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(port);
  if (::inet_pton(AF_INET, ip.c_str(), &dest.sin_addr) != 1) return;
  int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return;
  auto payload = j.dump();
  ::sendto(fd, payload.data(), payload.size(), 0,
           reinterpret_cast<sockaddr *>(&dest), sizeof(dest));
  ::close(fd);
}

nlohmann::json ctrl_msg(const char * type, const Robot & r)
{
  nlohmann::json j;
  j["v"] = 1;
  j["type"] = type;
  j["host_id"] = kHostId;
  j["name"] = kHostId;
  j["data_port"] = r.data_port;
  j["peer_port"] = r.peer_port;
  return j;
}

std::string frag_from(const uint8_t * buf, size_t n)
{
  if (n < 16 || buf[0] != kImgFrag) return {};
  size_t from_len = buf[15];
  if (from_len == 0 || n < 16 + from_len) return {};
  return std::string(reinterpret_cast<const char *>(buf + 16), from_len);
}

void on_data(Robot & r, const uint8_t * buf, size_t n)
{
  if (n == 0) return;
  if (buf[0] == kImgFrag) {
    auto from = frag_from(buf, n);
    if (from != r.name) {
      r.foreign++;
      return;
    }
    if (!r.subscribed) r.image_early++;
    else r.image = true;
    return;
  }
  if (buf[0] != '{') return;
  try {
    auto j = nlohmann::json::parse(buf, buf + n);
    if (j.value("_from", "") != r.name) {
      r.foreign++;
      return;
    }
    if (j.contains("elapsed")) r.plot = true;
  } catch (...) {
  }
}

void on_peer(Robot & r, const char * buf, size_t n)
{
  try {
    auto j = nlohmann::json::parse(buf, buf + n);
    if (j.value("type", "") != "register_ack") return;
    if (j.value("status", "") != "ok") return;
    if (j.value("role", "") != "head") return;
    if (j.value("robot", "") != r.name) return;
    r.ack = true;
  } catch (...) {
  }
}

void stop_children(std::vector<Robot> & robots)
{
  for (auto & r : robots)
    if (r.pid > 0) ::kill(r.pid, SIGTERM);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    bool any = false;
    for (auto & r : robots) {
      if (r.pid <= 0) continue;
      int status = 0;
      pid_t w = ::waitpid(r.pid, &status, WNOHANG);
      if (w == r.pid) r.pid = -1;
      else any = true;
    }
    if (!any) return;
    ::usleep(50000);
  }
  for (auto & r : robots) {
    if (r.pid <= 0) continue;
    ::kill(r.pid, SIGKILL);
    for (int i = 0; i < 50; i++) {
      int status = 0;
      pid_t w = ::waitpid(r.pid, &status, WNOHANG);
      if (w == r.pid || w < 0) {
        r.pid = -1;
        break;
      }
      ::usleep(20000);
    }
  }
}

bool robot_ok(const Robot & r)
{
  return r.beacon && r.ack && r.plot && r.image && r.foreign == 0 && r.image_early == 0;
}

}  // namespace

int main(int argc, char * argv[])
{
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string rate = "20";
  int timeout_s = 20;
  std::string bin = argv[0];
  auto slash = bin.rfind('/');
  bin = (slash != std::string::npos ? bin.substr(0, slash + 1) : "./") + "remote_logger_test";

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::printf("Usage: %s [--bin=PATH] [--rate=HZ] [--timeout=SEC]\n", argv[0]);
      std::printf("  Spawns one remote_logger_test per robot, then acts as the host:\n");
      std::printf("  beacon :%u, per-robot control, data from %u, peer from %u.\n",
                  kDiscoverPort, kDataBase, kPeerBase);
      std::printf("  Exits 0 when each robot's plot and 0xFE image arrive on its own data port.\n");
      std::printf("  Needs UDP %u free. Do not run another watch on that port.\n", kDiscoverPort);
      return 0;
    }
    auto eq = arg.find('=');
    std::string k = (eq != std::string::npos) ? arg.substr(0, eq) : arg;
    std::string v = (eq != std::string::npos) ? arg.substr(eq + 1) : (i + 1 < argc ? argv[++i] : "");
    if (k == "--bin") bin = v;
    else if (k == "--rate") rate = v;
    else if (k == "--timeout") timeout_s = std::stoi(v);
  }

  int discover_fd = bind_udp(kDiscoverPort, true);
  if (discover_fd < 0) {
    std::perror("[multi] bind :15999");
    std::fprintf(stderr, "[multi] discovery port %u is busy\n", kDiscoverPort);
    return 1;
  }
  const int n = 3;
  std::vector<Robot> robots(n);
  uint16_t data_search = kDataBase;
  uint16_t peer_search = kPeerBase;
  uint16_t ctrl_search = kCtrlBase;
  for (int i = 0; i < n; i++) {
    robots[i].name = kNames[i];
    robots[i].data_fd = take_port(data_search, &robots[i].data_port);
    robots[i].peer_fd = take_port(peer_search, &robots[i].peer_port);
    robots[i].control = reserve_ctrl_port(ctrl_search);
    if (robots[i].data_fd < 0 || robots[i].peer_fd < 0 || robots[i].control == 0) {
      std::fprintf(stderr, "[multi] no free UDP port for %s\n", kNames[i]);
      stop_children(robots);
      return 1;
    }
    data_search = static_cast<uint16_t>(robots[i].data_port + 1);
    peer_search = static_cast<uint16_t>(robots[i].peer_port + 1);
    ctrl_search = static_cast<uint16_t>(robots[i].control + 1);
  }

  for (int i = 0; i < n; i++) {
    pid_t pid = ::fork();
    if (pid < 0) {
      std::perror("[multi] fork");
      stop_children(robots);
      return 1;
    }
    if (pid == 0) {
      auto ctrl = std::to_string(robots[i].control);
      ::execl(bin.c_str(), bin.c_str(),
              "--ctrl-port", ctrl.c_str(),
              "--name", robots[i].name.c_str(),
              "--rate", rate.c_str(),
              "--no-local",
              nullptr);
      std::perror("execl");
      _exit(1);
    }
    robots[i].pid = pid;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::printf("[multi] %d robots, rate %s Hz, discover :%u\n", n, rate.c_str(), kDiscoverPort);
  for (const auto & r : robots) {
    std::printf("[multi] %s control :%u data :%u peer :%u\n",
                r.name.c_str(), r.control, r.data_port, r.peer_port);
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  auto now = std::chrono::steady_clock::now();
  while (!g_stop && now < deadline) {
    int status = 0;
    pid_t dead = ::waitpid(-1, &status, WNOHANG);
    if (dead > 0) {
      std::fprintf(stderr, "[multi] sender %d exited early\n", dead);
      for (auto & r : robots)
        if (r.pid == dead) r.pid = -1;
      break;
    }

    pollfd pfds[7];
    nfds_t nfds = 0;
    pfds[nfds].fd = discover_fd;
    pfds[nfds].events = POLLIN;
    pfds[nfds].revents = 0;
    nfds++;
    for (auto & r : robots) {
      pfds[nfds].fd = r.data_fd;
      pfds[nfds].events = POLLIN;
      pfds[nfds].revents = 0;
      nfds++;
      pfds[nfds].fd = r.peer_fd;
      pfds[nfds].events = POLLIN;
      pfds[nfds].revents = 0;
      nfds++;
    }
    ::poll(pfds, nfds, 100);

    uint8_t buf[65536];
    while (true) {
      sockaddr_in from{};
      socklen_t flen = sizeof(from);
      ssize_t got = ::recvfrom(discover_fd, buf, sizeof(buf), MSG_DONTWAIT,
                               reinterpret_cast<sockaddr *>(&from), &flen);
      if (got <= 0) break;
      try {
        auto msg = nlohmann::json::parse(buf, buf + got);
        if (msg.value("v", 0) != 1 || msg.value("type", "") != "beacon") continue;
        auto name = msg.value("name", "");
        auto ip = msg.value("ip", "");
        int control = msg.value("control", 0);
        for (auto & r : robots) {
          if (r.name != name) continue;
          if (control != r.control || ip.empty()) {
            std::fprintf(stderr, "[multi] bad beacon for %s ip=%s control=%d\n",
                         name.c_str(), ip.c_str(), control);
            continue;
          }
          r.beacon = true;
          r.ip = ip;
        }
      } catch (...) {
      }
    }

    for (auto & r : robots) {
      while (true) {
        ssize_t got = ::recvfrom(r.data_fd, buf, sizeof(buf), MSG_DONTWAIT, nullptr, nullptr);
        if (got <= 0) break;
        on_data(r, buf, static_cast<size_t>(got));
      }
      while (true) {
        ssize_t got = ::recvfrom(r.peer_fd, buf, sizeof(buf), MSG_DONTWAIT, nullptr, nullptr);
        if (got <= 0) break;
        on_peer(r, reinterpret_cast<char *>(buf), static_cast<size_t>(got));
      }
    }

    now = std::chrono::steady_clock::now();
    bool all = true;
    for (auto & r : robots) {
      if (r.beacon && !r.ack) {
        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - r.last_register);
        if (r.last_register.time_since_epoch().count() == 0 || gap.count() >= 300) {
          send_json(r.ip, r.control, ctrl_msg("register", r));
          r.last_register = now;
        }
      }
      if (r.ack) {
        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - r.last_alive);
        if (r.last_alive.time_since_epoch().count() == 0 || gap.count() >= 400) {
          send_json(r.ip, r.control, ctrl_msg("head_alive", r));
          r.last_alive = now;
        }
      }
      if (r.ack && r.plot && !r.subscribed) {
        auto sub = ctrl_msg("img_subscribe", r);
        sub["streams"] = nlohmann::json::array({"simulation", "waveform"});
        send_json(r.ip, r.control, sub);
        r.subscribed = true;
        std::printf("[multi] img_subscribe %s\n", r.name.c_str());
      }
      if (!robot_ok(r)) all = false;
    }
    if (all) break;
    now = std::chrono::steady_clock::now();
  }

  for (auto & r : robots) {
    if (!r.ip.empty())
      send_json(r.ip, r.control, ctrl_msg("deregister", r));
  }
  stop_children(robots);
  ::close(discover_fd);
  for (auto & r : robots) {
    if (r.data_fd >= 0) ::close(r.data_fd);
    if (r.peer_fd >= 0) ::close(r.peer_fd);
  }

  int failed = 0;
  for (const auto & r : robots) {
    if (robot_ok(r)) {
      std::printf("[multi] ok %s control=%u data=%u peer=%u _from=%s\n",
                  r.name.c_str(), r.control, r.data_port, r.peer_port, r.name.c_str());
    } else {
      failed++;
      std::fprintf(stderr,
                   "[multi] fail %s beacon=%d ack=%d plot=%d image=%d early_img=%d foreign=%d\n",
                   r.name.c_str(), r.beacon, r.ack, r.plot, r.image, r.image_early, r.foreign);
    }
  }
  if (g_stop) {
    std::fprintf(stderr, "[multi] interrupted\n");
    return 1;
  }
  if (failed) {
    std::fprintf(stderr, "[multi] %d/%d robots failed\n", failed, n);
    return 1;
  }
  std::printf("[multi] %d robots passed\n", n);
  return 0;
}
