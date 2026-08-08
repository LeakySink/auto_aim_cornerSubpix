#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static volatile sig_atomic_t g_running = 1;
static void on_signal(int) { g_running = 0; }

int main(int argc, char * argv[])
{
  std::string host = "127.0.0.1";
  std::string ctrl_port = "15000";
  std::string rate = "50";
  std::string bin = argv[0];
  auto slash = bin.rfind('/');
  bin = (slash != std::string::npos ? bin.substr(0, slash + 1) : "./") + "remote_logger_test";

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    auto eq = arg.find('=');
    std::string k = (eq != std::string::npos) ? arg.substr(0, eq) : arg;
    std::string v = (eq != std::string::npos) ? arg.substr(eq + 1) : (i + 1 < argc ? argv[++i] : "");
    if (k == "--host") host = v;
    else if (k == "--ctrl-port") ctrl_port = v;
    else if (k == "--rate") rate = v;
    else if (k == "--bin") bin = v;
  }

  const char * names[] = {"robot_alpha", "robot_beta", "robot_gamma"};
  int n = 3;
  std::vector<pid_t> pids;

  for (int i = 0; i < n; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      execl(bin.c_str(), bin.c_str(),
            "--host", host.c_str(),
            "--ctrl-port", ctrl_port.c_str(),
            "--name", names[i],
            "--rate", rate.c_str(),
            nullptr);
      std::perror("execl");
      _exit(1);
    }
    pids.push_back(pid);
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  std::printf("[multi] %d senders @ %s Hz -> %s\n", n, rate.c_str(), host.c_str());

  while (g_running) {
    int status;
    pid_t w = ::waitpid(-1, &status, WNOHANG);
    if (w > 0) {
      std::printf("[multi] sender %d exited\n", w);
      for (auto & p : pids) if (p == w) p = -1;
    }
    bool all_dead = true;
    for (auto p : pids) if (p > 0) all_dead = false;
    if (all_dead) break;
    ::usleep(200000);
  }

  for (auto p : pids)
    if (p > 0) { ::kill(p, SIGTERM); ::waitpid(p, nullptr, 0); }

  std::printf("[multi] stopped\n");
  return 0;
}
