// Copyright 2026 The Bazel Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/main/cpp/blaze_criu.h"

#include <cstdlib>

#include "src/main/cpp/blaze_criu_internal.h"
#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/errors.h"
#include "src/main/cpp/util/exit_code.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/logging.h"
#include "src/main/cpp/util/numbers.h"
#include "src/main/cpp/util/strings.h"
#include "src/main/protobuf/command_server.pb.h"

namespace blaze {

using std::map;
using std::string;
using std::vector;

namespace {
constexpr char kCriuImagesSubdir[] = "criu";
constexpr char kControlSockName[] = "bazel-criu.sock";
constexpr char kNsPidFile[] = "ns-pid";
constexpr char kServerInfoName[] = "server_info.rawproto";
constexpr char kInstallKeyFile[] = "install-key";
constexpr char kStartupArgsFile[] = "startup-args";
bool g_criu_mode_active = false;
}  // namespace

void SetCriuModeActive(bool active) { g_criu_mode_active = active; }

bool CriuModeActive() {
#ifdef __linux__
  return g_criu_mode_active;
#else
  return false;
#endif
}

blaze_util::Path CriuCheckpointSentinelPath(
    const blaze_util::Path& output_base) {
  return output_base.GetRelative("server/checkpoint.sentinel");
}

blaze_util::Path ControlSocketPath(const blaze_util::Path& output_base) {
  return output_base.GetRelative(kControlSockName);
}

}  // namespace blaze

#ifndef __linux__
namespace blaze {

bool CriuPreflight(std::string *) { return true; }

bool KillNamespacedServer(const blaze_util::Path &) { return false; }
bool CriuCheckpointExists(const blaze_util::Path&, const string&,
                          const vector<string>&) {
  return false;
}
bool CriuRestore(const blaze_util::Path&) { return false; }
int CriuCheckpoint(const blaze_util::Path&, const string&, bool,
                   const std::function<bool()>&) {
  return blaze_exit_code::BAD_ARGV;
}

int ExecuteDaemonInNamespace(const blaze_util::Path &, const vector<string> &,
                            const map<string, EnvVarValue> &,
                            const blaze_util::Path &, bool,
                            const blaze_util::Path &, const blaze_util::Path &,
                            BlazeServerStartup **) {
  BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
      << "CRIU mode is only supported on Linux.";
  return -1;
}

}  // namespace blaze
#else  // __linux__

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#ifndef environ
extern char **environ;
#endif

namespace blaze {
namespace {

// Returns s with leading/trailing whitespace removed.
string Stripped(const string &s) {
  string out = s;
  blaze_util::StripWhitespace(&out);
  return out;
}

// Writes "criu: <msg>: <errno string>\n" to stderr. For use in the post-fork
// child paths, where BAZEL_LOG is not fork-safe; the init's stderr is still
// connected to the launcher's terminal until it detaches, so these surface.
void ChildPerror(const char *msg) {
  int e = errno;
  const char *es = strerror(e);
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "criu: %s: %s\n", msg, es);
  if (n > 0) {
    (void)!write(STDERR_FILENO, buf, n < static_cast<int>(sizeof(buf))
                                         ? n
                                         : static_cast<int>(sizeof(buf)) - 1);
  }
}

// An always-alive startup stub. In CRIU mode the server lives in a PID
// namespace under a persistent init, so we cannot use the inherited-socket
// liveness trick of SocketBlazeServerStartup. Instead the subsequent gRPC
// connect (ConnectOrDie) decides whether startup succeeded.
class AlwaysAliveServerStartup : public BlazeServerStartup {
 public:
  bool IsStillAlive() override { return true; }
};

// Writes the whole of `content` to the file at `path` (used for the
// /proc/self/{setgroups,uid_map,gid_map} userns files). Returns false on any
// short write or error. Async-signal-safe: only open/write/close.
bool WriteProcFile(const char *path, const string &content) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    ChildPerror(path);
    return false;
  }
  ssize_t n = write(fd, content.data(), content.size());
  bool ok = n == static_cast<ssize_t>(content.size());
  if (!ok) {
    ChildPerror(path);
  }
  close(fd);
  return ok;
}

// Maps host uid/gid to root (0) inside the user namespace newly created by the
// process `pid`, using the single-mapping exception that needs no privilege.
//
// This MUST be done by `pid`'s parent (which still has privilege over the new,
// nested user namespace), not by `pid` itself: once a process is inside the new
// userns it is mapped to "nobody" and cannot write its own map. setgroups must
// be denied before gid_map may be written unprivileged.
bool WriteUserNamespaceMappings(pid_t pid, uid_t uid, gid_t gid) {
  char path[64];
  char buf[64];

  snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
  int len = snprintf(buf, sizeof(buf), "0 %d 1", uid);
  if (!WriteProcFile(path, string(buf, len))) {
    return false;
  }

  snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
  if (!WriteProcFile(path, "deny")) {
    return false;
  }

  snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
  len = snprintf(buf, sizeof(buf), "0 %d 1", gid);
  if (!WriteProcFile(path, string(buf, len))) {
    return false;
  }
  return true;
}

// Mounts a private /proc for the new PID namespace without propagating mounts
// back to the host.
bool MountPrivateProc() {
  if (mount("", "/", "", MS_REC | MS_PRIVATE, nullptr) != 0) {
    ChildPerror("mount / rprivate");
    return false;
  }
  if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC,
            nullptr) != 0) {
    ChildPerror("mount private /proc (check container namespace restrictions)");
    return false;
  }
  return true;
}

// Converts wait(2) status to a process exit code (128+signal for signals).
int WaitStatusToCode(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}

// Closes every inherited file descriptor >= 3 except `keep`. The launcher holds
// the output-base lock (and other fds) on descriptors that would otherwise be
// inherited by the persistent init and the server it forks — and an inherited
// flock would block every future client on the output base forever. We scan
// /proc/self/fd so we close exactly the open ones. Fork-safe: only
// opendir/readdir/close.
void CloseInheritedFds(int keep) {
  DIR *d = opendir("/proc/self/fd");
  if (d == nullptr) {
    // Fallback: blind-close a conservative range.
    for (int fd = 3; fd < 1024; fd++) {
      if (fd != keep) {
        close(fd);
      }
    }
    return;
  }
  int dir_fd = dirfd(d);
  for (struct dirent *e = readdir(d); e != nullptr; e = readdir(d)) {
    int fd = 0;
    // Manual atoi to avoid pulling non-fork-safe helpers; entries are "." etc.
    bool numeric = e->d_name[0] != '\0';
    for (const char *p = e->d_name; *p; p++) {
      if (*p < '0' || *p > '9') {
        numeric = false;
        break;
      }
      fd = fd * 10 + (*p - '0');
    }
    if (numeric && fd >= 3 && fd != keep && fd != dir_fd) {
      close(fd);
    }
  }
  closedir(d);
}

// Builds a NULL-terminated argv/envp array from C++ containers. The backing
// strings must outlive the returned pointer array.
vector<char *> ToCStringArray(const vector<string> &items) {
  vector<char *> out;
  out.reserve(items.size() + 1);
  for (const string &s : items) {
    out.push_back(const_cast<char *>(s.c_str()));
  }
  out.push_back(nullptr);
  return out;
}

vector<string> EnvToStrings(const map<string, EnvVarValue> &env) {
  vector<string> out;
  for (const auto &kv : env) {
    if (kv.second.action == EnvVarAction::SET) {
      out.push_back(kv.first + "=" + kv.second.value);
    }
  }
  return out;
}

// Plain-old-data view of the paths the persistent init needs to serve the
// checkpoint control socket and run `criu dump` from inside the namespace. All
// members point into std::strings owned by the caller's frame, which outlives
// the two forks down to the init (fork copies the frame), so the init may read
// them freely. `sock_path` empty means "do not serve a control socket".
using criu_internal::InitControl;

// Reads a small unsigned decimal from `path` (e.g. server.pid.txt). Returns -1
// on any error. Fork-safe: open/read/close + manual parse only.
int ReadIntFromFile(const char* path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  char buf[32];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) {
    return -1;
  }
  int v = 0;
  for (ssize_t i = 0; i < n; i++) {
    if (buf[i] < '0' || buf[i] > '9' || v > (INT_MAX - (buf[i] - '0')) / 10) {
      return -1;
    }
    v = v * 10 + (buf[i] - '0');
  }
  return v > 0 ? v : -1;
}

// Copies the file at `src` to `dst` (truncating dst), preserving contents only.
// Fork-safe: open/read/write/close in a fixed buffer, no allocation. Returns
// false on any error (e.g. src missing).
bool CopyFileRaw(const char* src, const char* dst) {
  int in = open(src, O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    return false;
  }
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (out < 0) {
    close(in);
    return false;
  }
  char buf[65536];
  bool ok = true;
  while (true) {
    ssize_t n = read(in, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ok = false;
      break;
    }
    if (n == 0) {
      break;
    }
    ssize_t off = 0;
    while (off < n) {
      ssize_t w = write(out, buf + off, n - off);
      if (w <= 0) {
        if (w < 0 && errno == EINTR) {
          continue;
        }
        ok = false;
        break;
      }
      off += w;
    }
    if (!ok) {
      break;
    }
  }
  close(in);
  close(out);
  return ok;
}

// Writes bare decimal to `path`, creating/truncating it. Fork-safe.
bool WriteIntFile(const char* path, int value) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    ChildPerror(path);
    return false;
  }
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%d", value);
  bool ok = len > 0 && write(fd, buf, len) == len;
  close(fd);
  return ok;
}

// Creates and binds the init's checkpoint control socket at `path`, returning a
// listening fd or -1. The socket lives on the shared output_base mount, so a
// host-side launcher (CriuCheckpoint) can connect even though it cannot enter
// the namespace. Fork-safe: socket/bind/listen + a manual path copy.
int MakeControlSocket(const char* path) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    // A pathologically long output_base; skip control serving rather than
    // truncate to a wrong path. Checkpoints will be unavailable for this base.
    errno = ENAMETOOLONG;
    ChildPerror("control socket path too long");
    return -1;
  }
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  // A stale socket inode from a previous init would make bind() fail.
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    ChildPerror("control socket()");
    return -1;
  }
  if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ChildPerror("control bind()");
    close(fd);
    return -1;
  }
  if (chmod(path, 0600) != 0 || listen(fd, 8) != 0) {
    ChildPerror("control listen()");
    close(fd);
    return -1;
  }
  return fd;
}

// Runs `criu dump` against the namespaced server, from inside the namespace
// (where the init holds CAP_CHECKPOINT_RESTORE over its own user namespace and
// criu sees a self-consistent mount view). This is what lets the dump succeed
// rootless and unprivileged — and why it does not need the host-side
// --enable-fs workaround for the inherited snap/squashfs mounts. The server's
// namespace-local pid is read from server.pid.txt (pids are namespace-local in
// here). On success the checkpointed ns pid is recorded next to the images so
// restore can find the revived server. Fork-safe.
bool RunCriuDump(const InitControl* ctl, bool leave_running, int conn) {
  int ns_pid = ReadIntFromFile(ctl->server_pid_file);
  if (ns_pid <= 0) {
    ChildPerror("read server pid for dump");
    return false;
  }

  mkdir(ctl->images_dir, 0755);  // ignore EEXIST

  char tree[16];
  snprintf(tree, sizeof(tree), "%d", ns_pid);
  char logfile[4096];
  snprintf(logfile, sizeof(logfile), "%s/dump.log", ctl->images_dir);

  // Mirrors horapha's criu.Options{ShellJob, TCPClose, LeaveRunning,
  // Unprivileged}. --unprivileged is the crux: criu treats the privileged
  // net/mount state it cannot touch as non-fatal, which is exactly what an
  // in-namespace rootless dump needs.
  const char* argv[20];
  int i = 0;
  argv[i++] = ctl->criu_bin;
  argv[i++] = "dump";
  argv[i++] = "--tree";
  argv[i++] = tree;
  if (leave_running) {
    argv[i++] = "--leave-running";
  }
  argv[i++] = "--images-dir";
  argv[i++] = ctl->images_dir;
  argv[i++] = "--ghost-limit";
  argv[i++] = "1000000000";
  argv[i++] = "--skip-file-rwx-check";
  argv[i++] = "--shell-job";
  argv[i++] = "--tcp-close";
  // The bazel server holds flock()s (e.g. on its output base); dump and restore
  // them rather than refusing. Within the namespace the lock holders are part
  // of the dumped tree, so this is self-consistent.
  argv[i++] = "--file-locks";
  argv[i++] = "--unprivileged";
  if (ctl->compress) argv[i++] = "--compress";
  argv[i++] = "--log-file";
  argv[i++] = logfile;
  argv[i++] = "-v4";
  argv[i] = nullptr;

  pid_t p = fork();
  if (p < 0) {
    ChildPerror("fork criu dump");
    return false;
  }
  if (p == 0) {
    // The dump must not inherit init's blocked SIGCHLD mask.
    sigset_t mask;
    sigemptyset(&mask);
    sigprocmask(SIG_SETMASK, &mask, nullptr);
    setsid();
    execvp(argv[0], const_cast<char* const*>(argv));
    ChildPerror(argv[0]);
    _exit(127);
  }

  // A disconnected/cancelled launcher must not leave a CRIU helper running
  // while the server resumes. Kill and reap the dump before acknowledging it.
  bool ok = false;
  int ticks = 0;
  while (true) {
    int status;
    pid_t w = waitpid(p, &status, WNOHANG);
    if (w == p) {
      ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      break;
    }
    if (w < 0 && errno != EINTR) break;
    struct pollfd cancel = {conn, POLLIN | POLLHUP, 0};
    int r = poll(&cancel, 1, 50);
    if ((r > 0 && cancel.revents) || ++ticks >= 6000) {
      // setsid in the child gives CRIU's helpers their own process group.
      kill(-p, SIGTERM);
      kill(p, SIGTERM);  // also covers cancellation before setsid
      // Let CRIU unwind a partial dump before forcing its exit.
      for (int attempt = 0; attempt < 40; ++attempt) {
        pid_t exited = waitpid(p, nullptr, WNOHANG);
        if (exited == p || (exited < 0 && errno == ECHILD)) return false;
        poll(nullptr, 0, 50);
      }
      kill(-p, SIGKILL);
      kill(p, SIGKILL);
      while (waitpid(p, nullptr, 0) < 0 && errno == EINTR) {
      }
      break;
    }
  }
  if (ok) {
    // Record the checkpointed ns-local pid alongside the images so restore can
    // locate the revived server without trusting bazel's mutable pid file.
    if (!WriteIntFile(ctl->ns_pid_file, ns_pid)) return false;
    // Snapshot server_info.rawproto next to the images. The restored process
    // does not rewrite this file, and a graceful `bazel shutdown` between
    // checkpoints deletes the live copy (it is registered for deleteAtExit), so
    // restore can recover it from here. Treat a missing snapshot as failure.
    if (!CopyFileRaw(ctl->server_info_file, ctl->server_info_snapshot)) {
      ChildPerror("snapshot server_info.rawproto");
      return false;
    }
  }
  return ok;
}

// The sentinel and lease must both name this request before a dump may start.
bool FileHasToken(const char* path, const char* token) {
  char contents[128];
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  ssize_t n = read(fd, contents, sizeof(contents));
  close(fd);
  size_t len = strlen(token);
  return n > 0 && static_cast<size_t>(n) == len &&
         memcmp(contents, token, len) == 0;
}

bool HandleControlConn(const InitControl* ctl, int conn) {
  // Bound incomplete/malformed requests, so a client cannot stop child reaping.
  struct timeval timeout = {5, 0};
  setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char line[256];
  size_t len = 0;
  while (len < sizeof(line) - 1) {
    char c;
    if (read(conn, &c, 1) != 1) return false;
    if (c == '\n') break;
    line[len++] = c;
  }
  line[len] = 0;
  auto reply = [&](const char* msg) {
    return send(conn, msg, strlen(msg), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(strlen(msg));
  };
  if (strcmp(line, "PING") == 0) {
    reply("OK pong\n");
    return false;
  }
  char* token = strchr(line, ' ');
  if (token == nullptr) {
    reply("ERR missing checkpoint token\n");
    return false;
  }
  *token++ = 0;
  bool stop = strcmp(line, "CHECKPOINT_STOP") == 0;
  if ((!stop && strcmp(line, "CHECKPOINT") != 0) ||
      !FileHasToken(ctl->request_file, token) ||
      !FileHasToken(ctl->sentinel_file, token)) {
    reply("ERR server is not ready for this checkpoint\n");
    return false;
  }
  bool ok = RunCriuDump(ctl, /*leave_running=*/true, conn);
  if (!ok) {
    unlink(ctl->request_file);
    unlink(ctl->sentinel_file);
    reply("ERR criu dump failed or cancelled (see criu/dump.log)\n");
    return false;
  }
  // Stop only once success is delivered; a failed/cancelled dump keeps the
  // warm server available. The init exits and the kernel kills its namespace.
  bool delivered =
      reply(stop ? "OK checkpointed and stopped\n" : "OK checkpointed\n");
  return stop && delivered;
}

// The body of the persistent PID-1 init. Never returns: it forks the foreground
// command (argv/envp), reports its exit code to the launcher over status_wfd,
// then runs an event loop that (a) reaps the daemonized/restored server tree
// and (b) serves the checkpoint control socket, so a host-side CriuCheckpoint
// can ask us — the only process holding CAP_CHECKPOINT_RESTORE over this user
// namespace — to run `criu dump`. Stays fork-safe: only syscalls + stack
// buffers, no heap, no BAZEL_LOG.
[[noreturn]] void RunInit(char* const argv[], char* const envp[],
                          int status_wfd, const InitControl* ctl) {
  // Drop every inherited fd except the status pipe — crucially the launcher's
  // output-base lock, which the persistent init and server would otherwise hold
  // forever, blocking all future clients. Do this first, before we fork the
  // server, so the server does not inherit them either.
  CloseInheritedFds(status_wfd);

  // Detach from the launcher's session/terminal so shell signals (Ctrl-C) do
  // not reach the persisted init or the server tree it holds.
  setsid();

  if (!MountPrivateProc()) {
    char code = 1;
    (void)!write(status_wfd, &code, 1);
    _exit(1);
  }

  // Block SIGCHLD and route it through a signalfd so the event loop below can
  // poll() child exits and control-socket connections together. Both fds are
  // CLOEXEC so they vanish from the exec'd foreground command. Set up before
  // forking fg so no SIGCHLD from it is missed.
  sigset_t chld_mask;
  sigemptyset(&chld_mask);
  sigaddset(&chld_mask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &chld_mask, nullptr);
  int sfd = signalfd(-1, &chld_mask, SFD_NONBLOCK | SFD_CLOEXEC);

  int lfd = (ctl != nullptr && ctl->sock_path[0] != '\0')
                ? MakeControlSocket(ctl->sock_path)
                : -1;

  // The init also clears a checkpoint park before a future restore resumes
  // the JVM. On a fresh start these files do not exist.
  bool released = (unlink(ctl->request_file) == 0 || errno == ENOENT) &&
                  (unlink(ctl->sentinel_file) == 0 || errno == ENOENT);

  if (sfd < 0 || lfd < 0 || !released) {
    ChildPerror("checkpoint control setup");
    char code = 1;
    (void)!write(status_wfd, &code, 1);
    _exit(1);
  }

  pid_t fg = fork();
  if (fg < 0) {
    ChildPerror("fork foreground");
    char code = 1;
    (void)!write(status_wfd, &code, 1);
    _exit(1);
  }
  if (fg == 0) {
    // Restore the default signal mask for the server (the sfd/lfd are CLOEXEC,
    // so they close on exec). execvpe (not execve) resolves a bare "criu" via
    // PATH; the fresh-start path passes an absolute daemonize path, fine too.
    sigprocmask(SIG_UNBLOCK, &chld_mask, nullptr);
    execvpe(argv[0], argv, envp);
    ChildPerror(argv[0]);
    _exit(127);  // exec failed
  }

  // Event loop: reap children (reporting fg's exit once) and serve checkpoint
  // requests until the namespace empties or a CHECKPOINT_STOP tears it down.
  int fg_code = 1;
  bool reported = false;
  bool stop = false;
  while (!stop) {
    struct pollfd pfds[2];
    pfds[0].fd = sfd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = lfd;  // negative fd is ignored by poll
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;
    int r = poll(pfds, 2, -1);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    if (pfds[0].revents & POLLIN) {
      struct signalfd_siginfo si;
      while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
      }  // drain coalesced SIGCHLDs
      bool empty = false;
      while (true) {
        int status;
        pid_t w = waitpid(-1, &status, WNOHANG);
        if (w == 0) {
          break;  // children remain but none ready
        }
        if (w < 0) {
          if (errno == ECHILD) {
            empty = true;  // namespace is empty
          }
          break;
        }
        if (w == fg && !reported) {
          fg_code = WaitStatusToCode(status);
          char code = static_cast<char>(fg_code);
          (void)!write(status_wfd, &code, 1);
          close(status_wfd);
          reported = true;
          // Detach stdio so the launcher's shell sees EOF and we run quietly.
          int devnull = open("/dev/null", O_RDWR);
          if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            if (devnull > 2) {
              close(devnull);
            }
          }
        }
      }
      if (empty) {
        break;
      }
    }

    if (lfd >= 0 && (pfds[1].revents & POLLIN)) {
      int conn = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
      if (conn >= 0) {
        stop = criu_internal::HandleCheckpointRequest(*ctl, conn);
        close(conn);
      }
    }
  }

  if (lfd >= 0 && ctl != nullptr && ctl->sock_path[0] != '\0') {
    unlink(ctl->sock_path);
  }
  if (!reported) {
    char code = static_cast<char>(fg_code);
    (void)!write(status_wfd, &code, 1);
  }
  if (stop) {
    // CHECKPOINT_STOP: kill the whole namespace (every process but us) and
    // drain it, then exit. As PID 1, kill(-1) targets all other processes.
    kill(-1, SIGKILL);
    while (true) {
      int status;
      pid_t w = waitpid(-1, &status, 0);
      if (w < 0 && errno != EINTR) {
        break;
      }
    }
  }
  _exit(fg_code);
}

bool RecordInit(const blaze_util::Path &server_dir, int init_pid);

// Starts a persistent PID-1 init in fresh user+PID+mount namespaces that runs
// `fg_argv` (with environment `fg_env`) as its foreground command. Returns the
// foreground command's exit code via *fg_code, the init's *host* pid via
// *init_host_pid (so callers can identify our PID namespace unambiguously), and
// true on a successful launch.
//
// The init outlives this call (it holds the namespace and the server open), so
// we deliberately do not wait for it; we only reap the throwaway userns-setup
// child. Mirrors horapha's nsrun.Run + runInit.
bool RunInNamespace(const vector<string>& fg_argv, const vector<string>& fg_env,
                    const blaze_util::Path& server_dir, const InitControl* ctl,
                    int* fg_code, int* init_host_pid, int timeout_secs = 0) {
  // Build the NULL-terminated argv/envp arrays BEFORE forking, so the post-fork
  // code paths perform no heap allocation and stay async-fork-safe. The backing
  // string vectors must outlive the fork.
  vector<char *> argv = ToCStringArray(fg_argv);
  vector<char *> envp = ToCStringArray(fg_env);
  const uid_t uid = getuid();
  const gid_t gid = getgid();

  // status_pipe: the init reports the foreground exit code (one byte) to us.
  // map_ready/map_request: synchronize the user-namespace setup. The setup
  // child creates the userns, signals us (1 byte) so we can write its uid/gid
  // map from out here — which MUST be done by the parent, since a process
  // inside a freshly created (nested) userns is "nobody" and cannot write its
  // own map — then we signal back (1 byte) that the maps are in place.
  // info_pipe: the setup child sends us the init's host pid (4 bytes), so we
  // can identify our own server's PID namespace and not confuse it with another
  // namespaced server that happens to share the same ns-local pid.
  int status_pipe[2] = {-1, -1};
  int map_ready[2] = {-1, -1};    // parent -> child: maps written, proceed
  int map_request[2] = {-1, -1};  // child -> parent: userns created, please write maps
  int info_pipe[2] = {-1, -1};    // child -> parent: init host pid
  auto close_pipes = [&]() {
    for (int *fds : {status_pipe, map_ready, map_request, info_pipe}) {
      for (int i = 0; i < 2; ++i) {
        if (fds[i] >= 0) close(fds[i]);
      }
    }
  };
  if (pipe2(status_pipe, O_CLOEXEC) != 0 ||
      pipe2(map_ready, O_CLOEXEC) != 0 ||
      pipe2(map_request, O_CLOEXEC) != 0 ||
      pipe2(info_pipe, O_CLOEXEC) != 0) {
    BAZEL_LOG(USER) << "criu: pipe failed: " << blaze_util::GetLastErrorString();
    close_pipes();
    return false;
  }

  pid_t setup = fork();
  if (setup < 0) {
    BAZEL_LOG(USER) << "criu: fork failed: " << blaze_util::GetLastErrorString();
    close_pipes();
    return false;
  }

  if (setup == 0) {
    // Throwaway userns-setup child (stays in the host PID namespace). It creates
    // the user namespace, waits for the parent to write its uid/gid map, then
    // unshares the PID and mount namespaces so its first child (the init)
    // becomes PID 1 of the new PID namespace. It forks that init and exits; the
    // init reparents to a host subreaper and persists.
    close(status_pipe[0]);
    close(map_ready[1]);
    close(map_request[0]);
    close(info_pipe[0]);

    char b = 1;
    if (unshare(CLONE_NEWUSER) != 0) {
      ChildPerror("unshare(CLONE_NEWUSER)");
      (void)!write(status_pipe[1], &b, 1);
      _exit(1);
    }
    // Ask the parent to write our maps, and wait until it has.
    (void)!write(map_request[1], &b, 1);
    if (read(map_ready[0], &b, 1) != 1 || b != 0) {
      // Parent failed to write the maps (it already logged why).
      (void)!write(status_pipe[1], &b, 1);
      _exit(1);
    }
    close(map_ready[0]);
    close(map_request[1]);

    if (unshare(CLONE_NEWNS | CLONE_NEWPID) != 0) {
      ChildPerror("unshare(CLONE_NEWNS|CLONE_NEWPID)");
      char code = 1;
      (void)!write(status_pipe[1], &code, 1);
      _exit(1);
    }

    // This fork runs in the host PID namespace (we have only unshared, not yet
    // entered, the new PID namespace), so `init` is the init's HOST pid. The
    // init itself becomes PID 1 inside the new namespace.
    pid_t init = fork();
    if (init < 0) {
      ChildPerror("fork init");
      char code = 1;
      (void)!write(status_pipe[1], &code, 1);
      _exit(1);
    }
    if (init == 0) {
      RunInit(argv.data(), envp.data(), status_pipe[1], ctl);  // never returns
    }
    // Tell the launcher the init's host pid, then exit. The init keeps
    // status_pipe[1] open and reparents to a host subreaper.
    int init_pid = static_cast<int>(init);
    (void)!write(info_pipe[1], &init_pid, sizeof(init_pid));
    close(info_pipe[1]);
    close(status_pipe[1]);
    _exit(0);
  }

  // Launcher (parent).
  close(status_pipe[1]);
  close(map_ready[0]);
  close(map_request[1]);
  close(info_pipe[1]);

  // Wait for the setup child to create its userns, then write its uid/gid map.
  char req = 0;
  bool mapped = false;
  if (read(map_request[0], &req, 1) == 1) {
    mapped = WriteUserNamespaceMappings(setup, uid, gid);
    char ack = mapped ? 0 : 1;
    (void)!write(map_ready[1], &ack, 1);
  }
  close(map_ready[1]);
  close(map_request[0]);

  // Receive the init's host pid.
  int init_pid = 0;
  ssize_t got = read(info_pipe[0], &init_pid, sizeof(init_pid));
  close(info_pipe[0]);
  bool recorded = got == sizeof(init_pid) && init_pid > 0 &&
                  RecordInit(server_dir, init_pid);
  if (got == sizeof(init_pid) && init_pid > 0) {
    *init_host_pid = init_pid;
    if (!recorded) {
      // This is the fresh child we just launched, not a PID read from disk.
      kill(init_pid, SIGKILL);
    }
  }

  // Reap the throwaway setup child, then await the init's report.
  int setup_status;
  while (waitpid(setup, &setup_status, 0) < 0 && errno == EINTR) {
  }

  char code = 0;
  ssize_t n = -1;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
  struct pollfd status_ready = {status_pipe[0], POLLIN, 0};
  while (true) {
    int wait_ms = timeout_secs == 0 ? -1 : 100;
    if (timeout_secs != 0 && std::chrono::steady_clock::now() >= deadline) {
      BAZEL_LOG(USER) << "criu: timed out waiting for namespaced command";
      break;
    }
    int r = poll(&status_ready, 1, wait_ms);
    if (r < 0 && errno == EINTR) continue;
    if (r == 0) continue;
    if (r > 0) n = read(status_pipe[0], &code, 1);
    break;
  }
  close(status_pipe[0]);
  if (!mapped) {
    return false;  // mapping failure already logged by WriteProcFile/ChildPerror
  }
  if (n != 1) {
    BAZEL_LOG(USER) << "criu: namespaced init exited before reporting status.";
    return false;
  }
  if (!recorded) {
    BAZEL_LOG(USER) << "criu: did not learn the namespaced init's host pid.";
    return false;
  }
  *fg_code = static_cast<int>(code);
  *init_host_pid = init_pid;
  return true;
}

// ---------------------------------------------------------------------------
// serverinfo: making a namespaced server reachable by a host-side client.
// Ported from horapha's internal/serverinfo.
// ---------------------------------------------------------------------------

// Reads field 22 (start time, jiffies since boot) of /proc/<pid>/stat as a
// decimal string. The comm field (2nd) is parenthesized and may contain
// spaces, so we split after the last ')'.
bool ReadStartTime(int pid, string *start_time, char *state = nullptr) {
  string statline;
  if (!blaze_util::ReadFile("/proc/" + std::to_string(pid) + "/stat",
                            &statline)) {
    return false;
  }
  string::size_type rparen = statline.rfind(')');
  if (rparen == string::npos) {
    return false;
  }
  // blaze_util::Split skips empty subsections, so the leading space after ')'
  // does not produce a spurious token. After ')' the first field is #3
  // (state); start time is field #22, i.e. index 19 counting from #3.
  vector<string> fields = blaze_util::Split(statline.substr(rparen + 1), ' ');
  const size_t kStartTimeIndexAfterComm = 19;
  if (fields.size() <= kStartTimeIndexAfterComm) {
    return false;
  }
  if (state != nullptr) {
    *state = fields[0][0];
  }
  *start_time = fields[kStartTimeIndexAfterComm];
  return true;
}

// Returns the innermost-namespace pid for a host pid, and whether the process
// is actually in a nested PID namespace (the NSpid line has >1 field).
bool InnerNsPid(int host_pid, int *inner) {
  string status;
  if (!blaze_util::ReadFile("/proc/" + std::to_string(host_pid) + "/status",
                            &status)) {
    return false;
  }
  for (const string &line : blaze_util::Split(status, '\n')) {
    if (line.compare(0, 6, "NSpid:") != 0) {
      continue;
    }
    // The NSpid line lists the pid in each pid namespace from outermost (host)
    // to innermost; the last (tab/space-separated) field is the innermost pid.
    // A single field means the process is not in a nested namespace.
    vector<string> fields = blaze_util::Split(Stripped(line.substr(6)), '\t');
    if (fields.size() < 2) {
      return false;
    }
    return blaze_util::safe_strto32(Stripped(fields.back()), inner);
  }
  return false;
}

// Returns the comm (executable name) of a host pid.
string ReadComm(int host_pid) {
  string comm;
  if (!blaze_util::ReadFile("/proc/" + std::to_string(host_pid) + "/comm",
                            &comm)) {
    return "";
  }
  return Stripped(comm);
}

// Returns the PID-namespace identity of a host pid as the symlink target of
// /proc/<pid>/ns/pid (e.g. "pid:[4026533181]"), or "" on error. Two processes
// share a PID namespace iff these strings are equal.
string PidNamespaceOf(int host_pid) {
  char target[256];
  string link = "/proc/" + std::to_string(host_pid) + "/ns/pid";
  ssize_t n = readlink(link.c_str(), target, sizeof(target) - 1);
  if (n < 0) {
    return "";
  }
  return string(target, n);
}

// Scans /proc and returns the host pid of the process whose innermost namespace
// pid equals ns_pid, whose comm matches want_comm (if non-empty), AND which
// lives in the SAME PID namespace as ns_namespace. The namespace match is
// essential: several independent namespaced servers can each have the same
// low ns-local pid (e.g. 3), so ns_pid alone is ambiguous — ns_namespace
// (the PID-namespace identity of our own init) disambiguates to our server.
bool HostPidForNsPid(int ns_pid, const string &want_comm,
                     const string &ns_namespace, int *host_pid) {
  DIR *proc = opendir("/proc");
  if (proc == nullptr) {
    return false;
  }
  bool found = false;
  for (struct dirent *e = readdir(proc); e != nullptr; e = readdir(proc)) {
    int candidate;
    if (!blaze_util::safe_strto32(e->d_name, &candidate)) {
      continue;  // not a pid dir
    }
    int inner;
    if (!InnerNsPid(candidate, &inner)) {
      continue;
    }
    // candidate != ns_pid guards against matching the process in its own (host)
    // namespace where the two are trivially equal.
    if (inner != ns_pid || candidate == ns_pid) {
      continue;
    }
    if (!want_comm.empty() && ReadComm(candidate) != want_comm) {
      continue;
    }
    if (!ns_namespace.empty() && PidNamespaceOf(candidate) != ns_namespace) {
      continue;  // a different namespaced server that happens to share ns_pid
    }
    *host_pid = candidate;
    found = true;
    break;
  }
  closedir(proc);
  return found;
}

// Polls HostPidForNsPid until a matching process appears or the timeout
// elapses. After a restore the tree is reparented asynchronously, so the
// server may take a moment to settle.
bool WaitHostPidForNsPid(int ns_pid, const string &want_comm,
                         const string &ns_namespace, int timeout_secs,
                         int *host_pid) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
  while (true) {
    if (HostPidForNsPid(ns_pid, want_comm, ns_namespace, host_pid)) {
      return true;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// Publish sidecars atomically. PID files use bare decimal without a newline:
// the JVM's PID watcher deliberately uses strict Integer.parseInt parsing.
bool WriteIdentityFile(const blaze_util::Path &path, const string &value) {
  blaze_util::Path tmp(path.AsNativePath() + ".criu.tmp");
  if (!blaze_util::WriteFile(value, tmp, 0600)) {
    return false;
  }
  if (rename(tmp.AsNativePath().c_str(), path.AsNativePath().c_str()) != 0) {
    blaze_util::UnlinkPath(tmp);
    return false;
  }
  return true;
}

bool RecordInit(const blaze_util::Path &server_dir, int init_pid) {
  string start_time;
  return ReadStartTime(init_pid, &start_time) &&
         WriteIdentityFile(server_dir.GetRelative("server.init_starttime"),
                           start_time) &&
         WriteIdentityFile(server_dir.GetRelative("server.init_host_pid"),
                           std::to_string(init_pid));
}

// The JVM owns rawproto and rewrites it from in-memory cookies and address on
// SIGWINCH, including after restore. Never edit the proto from the launcher.
bool RewriteServerIdentity(const blaze_util::Path &server_dir, int host_pid) {
  string start_time;
  if (!ReadStartTime(host_pid, &start_time) ||
      !WriteIdentityFile(server_dir.GetRelative("server.starttime"),
                         start_time) ||
      !WriteIdentityFile(server_dir.GetRelative("server.host_pid"),
                         std::to_string(host_pid))) {
    BAZEL_LOG(USER) << "criu: could not publish host PID and start time";
    return false;
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    string data;
    command_server::ServerInfo info;
    if (blaze_util::ReadFile(server_dir.GetRelative("server_info.rawproto"),
                             &data) &&
        info.ParseFromString(data) && info.pid() == host_pid) {
      return true;
    }
    if (kill(host_pid, SIGWINCH) != 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  } while (std::chrono::steady_clock::now() < deadline);
  BAZEL_LOG(USER) << "criu: timed out waiting for JVM to publish host PID "
                  << host_pid;
  return false;
}

// Resolves the server's namespace-local pid to its host pid (within the PID
// namespace identified by ns_namespace) and rewrites the on-disk identity
// files. Returns the host pid, or -1 on failure.
int MakeReachable(int ns_pid, const string &ns_namespace,
                  const blaze_util::Path &server_dir, int timeout_secs) {
  int host_pid;
  if (!WaitHostPidForNsPid(ns_pid, "java", ns_namespace, timeout_secs,
                           &host_pid)) {
    BAZEL_LOG(USER) << "criu: could not find host pid for namespace pid "
                    << ns_pid;
    return -1;
  }
  if (!RewriteServerIdentity(server_dir, host_pid)) {
    BAZEL_LOG(USER) << "criu: could not rewrite server identity for host pid "
                    << host_pid;
    return -1;
  }
  return host_pid;
}

// Reads a decimal PID, accepting optional surrounding whitespace.
bool ReadPidFile(const blaze_util::Path &path, int *pid) {
  string bufstr;
  return blaze_util::ReadFile(path, &bufstr, 32) &&
         blaze_util::safe_strto32(Stripped(bufstr), pid);
}

// ns-pid is written only after a successful dump and survives every restore.
string CheckpointCreated(const blaze_util::Path& images) {
  struct stat st;
  struct tm utc;
  char formatted[32];
  if (stat(images.GetRelative(kNsPidFile).AsNativePath().c_str(), &st) != 0 ||
      gmtime_r(&st.st_mtime, &utc) == nullptr ||
      strftime(formatted, sizeof(formatted), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
    return "";
  }
  return string(" (checkpoint created ") + formatted + ")";
}

string CriuBinary() {
  string criu = GetEnv("BAZEL_CRIU_BINARY");
  return criu.empty() ? "criu" : criu;
}

// Read a small sysctl-style file, or return "" if the kernel has no such knob.
string ReadSysctl(const char *path) {
  string value;
  if (!blaze_util::ReadFile(path, &value)) {
    return "";
  }
  return Stripped(value);
}

// Resolves the criu binary to an absolute, executable path for the preflight
// report. If BAZEL_CRIU_BINARY is set it is taken as-is (it may be absolute);
// otherwise the bare "criu" is looked up on PATH exactly as execvpe would at
// dump/restore time. Returns "" if nothing usable is found.
string ResolveCriuBinary() {
  const string criu = CriuBinary();
  if (criu.find('/') != string::npos) {
    struct stat st;
    return stat(criu.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                   access(criu.c_str(), X_OK) == 0
               ? criu
               : "";
  }
  return Which(criu);
}

criu_internal::CriuFeatures GetCriuFeatures() {
  // A launcher uses one CRIU executable. Probe before forking, never in init.
  static const auto features =
      criu_internal::ProbeCriuFeatures(ResolveCriuBinary());
  return features;
}

struct InitControlStorage {
  string sock;
  string criu_bin;
  string images_dir;
  string ns_pid_file;
  string server_pid_file;
  string server_info_file;
  string server_info_snapshot;
  string sentinel_file;
  string request_file;
  InitControl ctl;

  InitControlStorage(const blaze_util::Path& output_base, bool serve_control) {
    const blaze_util::Path images = output_base.GetRelative(kCriuImagesSubdir);
    const blaze_util::Path server = output_base.GetRelative("server");
    sock = serve_control ? ControlSocketPath(output_base).AsNativePath() : "";
    criu_bin = CriuBinary();
    images_dir = images.AsNativePath();
    ns_pid_file = images.GetRelative(kNsPidFile).AsNativePath();
    server_pid_file = server.GetRelative(kServerPidFile).AsNativePath();
    server_info_file = server.GetRelative(kServerInfoName).AsNativePath();
    server_info_snapshot = images.GetRelative(kServerInfoName).AsNativePath();
    sentinel_file = CriuCheckpointSentinelPath(output_base).AsNativePath();
    request_file = server.GetRelative("checkpoint.request").AsNativePath();
    ctl.request_file = request_file.c_str();
    ctl.sock_path = sock.c_str();
    ctl.criu_bin = criu_bin.c_str();
    ctl.images_dir = images_dir.c_str();
    ctl.ns_pid_file = ns_pid_file.c_str();
    ctl.server_pid_file = server_pid_file.c_str();
    ctl.server_info_file = server_info_file.c_str();
    ctl.server_info_snapshot = server_info_snapshot.c_str();
    ctl.sentinel_file = sentinel_file.c_str();
    ctl.compress = GetCriuFeatures().compress;
  }
};

vector<string> CheckpointStartupArgs(const string& serialized) {
  vector<string> result;
  for (const auto& arg : blaze_util::Split(serialized, '\0')) {
    // These options describe the client connecting to the server, rather than
    // the JVM being restored. Keep every JVM option, its position, and its
    // multiplicity: two conflicting -Xmx or -D flags are order-sensitive.
    bool client_only = false;
    for (const string prefix : {"--option_sources=", "--connect_timeout_secs=",
                                "--local_startup_timeout_secs=",
                                "--client_debug=", "--preemptible="}) {
      if (arg.compare(0, prefix.size(), prefix) == 0) client_only = true;
    }
    if (!client_only) result.push_back(arg);
  }
  return result;
}

}  // namespace

criu_internal::CriuFeatures criu_internal::ProbeCriuFeatures(
    const string& binary, std::chrono::milliseconds timeout) {
  if (binary.empty()) return {};
  int output[2];
  if (pipe2(output, O_CLOEXEC) != 0) return {};
  if (fcntl(output[0], F_SETFL, O_NONBLOCK) != 0) {
    close(output[0]);
    close(output[1]);
    return {};
  }
  const char* argv[] = {binary.c_str(), "--help", nullptr};
  pid_t child = fork();
  if (child == 0) {
    setsid();
    dup2(output[1], STDOUT_FILENO);
    dup2(output[1], STDERR_FILENO);
    CloseInheritedFds(-1);
    // No shell: an executable path may contain spaces or shell metacharacters.
    execv(argv[0], const_cast<char* const*>(argv));
    _exit(127);
  }
  close(output[1]);
  if (child < 0) {
    close(output[0]);
    return {};
  }
  string help;
  bool eof = false, exited = false;
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline &&
         help.size() < 1024 * 1024) {
    char buf[4096];
    ssize_t n = read(output[0], buf, sizeof(buf));
    if (n > 0) help.append(buf, n);
    if (n == 0) eof = true;
    // Keep the child unreaped until EOF so its PID/process-group identity
    // cannot be reused if timeout cleanup must kill helpers holding the pipe.
    if (eof) exited = waitpid(child, &status, WNOHANG) == child;
    if (n < 0 && errno != EINTR && errno != EAGAIN) break;
    if (eof && exited) break;
    struct pollfd ready = {eof ? -1 : output[0], POLLIN, 0};
    if (n <= 0) poll(&ready, 1, 10);
  }
  close(output[0]);
  if (!eof || !exited) {
    kill(-child, SIGKILL);
    if (!exited) {
      kill(child, SIGKILL);
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
    return {};
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
  auto has_option = [&](const string& option) {
    for (size_t pos = help.find(option); pos != string::npos;
         pos = help.find(option, pos + 1)) {
      size_t end = pos + option.size();
      if ((pos == 0 ||
           std::isspace(static_cast<unsigned char>(help[pos - 1]))) &&
          (end == help.size() ||
           std::isspace(static_cast<unsigned char>(help[end])) ||
           help[end] == '=' || help[end] == ','))
        return true;
    }
    return false;
  };
  return {has_option("--compress"), has_option("--decompress-threads")};
}

vector<string> criu_internal::RestoreArgs(const string& binary,
                                          const blaze_util::Path& images,
                                          CriuFeatures features) {
  vector<string> args = {binary,
                         "restore",
                         "--restore-detached",
                         "--images-dir",
                         images.AsNativePath(),
                         "--skip-file-rwx-check",
                         "--ghost-limit",
                         "1000000000",
                         "--tcp-close",
                         "--shell-job",
                         "--file-locks",
                         "--unprivileged",
                         "--log-file",
                         images.GetRelative("restore.log").AsNativePath(),
                         "-v4"};
  if (features.decompress_threads) {
    args.push_back("--decompress-threads");
    args.push_back("0");
  }
  return args;
}

bool CriuCheckpointExists(const blaze_util::Path& output_base,
                          const string& install_key,
                          const vector<string>& server_args) {
  const auto images = output_base.GetRelative(kCriuImagesSubdir);
  if (!blaze_util::PathExists(images)) return false;
  auto reject = [](const char* reason) {
    BAZEL_LOG(USER) << "criu: ignoring checkpoint: " << reason;
    return false;
  };
  string key, saved_args, requested_args;
  if (!blaze_util::ReadFile(images.GetRelative(kInstallKeyFile), &key) ||
      key != install_key) {
    return reject("missing or different binary identity");
  }
  if (!blaze_util::ReadFile(images.GetRelative(kStartupArgsFile),
                            &saved_args) ||
      saved_args.empty()) {
    return reject("missing checkpoint-time startup arguments");
  }
  blaze_util::JoinStrings(server_args, '\0', &requested_args);
  if (CheckpointStartupArgs(saved_args) !=
      CheckpointStartupArgs(requested_args)) {
    return reject("checkpoint-time startup arguments differ");
  }
  int ns_pid;
  if (!ReadPidFile(images.GetRelative(kNsPidFile), &ns_pid) || ns_pid <= 1 ||
      !blaze_util::PathExists(images.GetRelative(kServerInfoName))) {
    return reject("missing or invalid server identity");
  }
  return true;
}

bool criu_internal::PrepareRestore(const blaze_util::Path& output_base,
                                   int* ns_pid) {
  const auto images = output_base.GetRelative(kCriuImagesSubdir);
  const auto server = output_base.GetRelative("server");
  string args, info;
  if (!ReadPidFile(images.GetRelative(kNsPidFile), ns_pid) || *ns_pid <= 1 ||
      !blaze_util::ReadFile(images.GetRelative(kStartupArgsFile), &args) ||
      args.empty() ||
      !blaze_util::ReadFile(images.GetRelative(kServerInfoName), &info) ||
      info.empty() || !blaze_util::MakeDirectories(server, 0700)) {
    BAZEL_LOG(USER) << "criu: could not read checkpoint server files";
    return false;
  }
  // Never let the resumed JVM use a previous server's host identity. SIGWINCH
  // will publish the fresh identity after the new namespace is populated.
  for (const char* name : {"server.host_pid", "server.starttime"}) {
    if (unlink(server.GetRelative(name).AsNativePath().c_str()) != 0 &&
        errno != ENOENT) {
      BAZEL_LOG(USER) << "criu: could not clear stale host identity";
      return false;
    }
  }
  // A restored PID watcher can fire immediately. Its file must contain the
  // original namespace-local PID, with no newline, before CRIU resumes it.
  // Always recover cookies and cmdline from the immutable snapshot, even if
  // a different server has left live files behind since checkpointing.
  if (!WriteIdentityFile(server.GetRelative(kServerPidFile),
                         std::to_string(*ns_pid)) ||
      !WriteIdentityFile(server.GetRelative(kServerInfoName), info) ||
      !WriteIdentityFile(server.GetRelative("cmdline"), args)) {
    BAZEL_LOG(USER) << "criu: could not recreate live server files";
    return false;
  }
  return true;
}

bool criu_internal::HandleCheckpointRequest(const InitControl& paths,
                                            int connection) {
  return HandleControlConn(&paths, connection);
}

bool KillNamespacedServer(const blaze_util::Path &server_dir) {
  const auto init_file = server_dir.GetRelative("server.init_host_pid");
  if (!blaze_util::PathExists(init_file)) {
    return false;
  }

  int init_pid = 0;
  string recorded_start, actual_start;
  char state = 0;
  int inner = 0;
  // Pin the process before validating it, so a recycled PID cannot be signalled
  // between the /proc checks and SIGKILL. Preflight checks pidfd support.
  int pidfd = -1;
  if (ReadPidFile(init_file, &init_pid) && init_pid > 0) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    pidfd = syscall(SYS_pidfd_open, init_pid, 0);
#endif
  }
  bool live = ReadStartTime(init_pid, &actual_start, &state) && state != 'Z' &&
              state != 'X';
  if (live &&
      blaze_util::ReadFile(server_dir.GetRelative("server.init_starttime"),
                           &recorded_start) &&
      recorded_start == actual_start && InnerNsPid(init_pid, &inner) &&
      inner == 1) {
    if (pidfd < 0) {
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "criu: cannot safely signal init " << init_pid
          << ": pidfd_open unavailable";
    }
#ifdef SYS_pidfd_send_signal
    if (syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0) != 0 &&
        errno != ESRCH) {
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "criu: could not kill namespace init " << init_pid << ": "
          << blaze_util::GetLastErrorString();
    }
#endif
    struct pollfd exited = {pidfd, POLLIN, 0};
    int result;
    do {
      result = poll(&exited, 1, 10000);
    } while (result < 0 && errno == EINTR);
    if (result <= 0) {
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "criu: namespace init " << init_pid << " did not exit";
    }
  } else if (live) {
    BAZEL_LOG(WARNING) << "criu: ignoring stale init identity for PID "
                       << init_pid;
  }
  if (pidfd >= 0) {
    close(pidfd);
  }
  // Even an invalid init record marks server.pid.txt as namespace-local. Never
  // pass that number to the ordinary host-side process-group kill path.
  for (const char *name : {"server.init_host_pid", "server.init_starttime",
                           "server.host_pid", kServerPidFile}) {
    blaze_util::UnlinkPath(server_dir.GetRelative(name));
  }
  blaze_util::UnlinkPath(ControlSocketPath(server_dir.GetParent()));
  return true;
}

bool CriuPreflight(std::string *error) {
  if (!CriuModeActive()) {
    return true;
  }

  // Gather the observed state of every roadblock up front so we can both log it
  // and, on failure, print it all together in one diagnostic.
  const string apparmor =
      ReadSysctl("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
  const string userns_clone =
      ReadSysctl("/proc/sys/kernel/unprivileged_userns_clone");
  const string max_userns = ReadSysctl("/proc/sys/user/max_user_namespaces");
  const string criu_path = ResolveCriuBinary();
  const string criu_requested = CriuBinary();

  BAZEL_LOG(INFO) << "criu preflight:"
                  << " apparmor_restrict_unprivileged_userns="
                  << (apparmor.empty() ? "(absent)" : apparmor)
                  << " unprivileged_userns_clone="
                  << (userns_clone.empty() ? "(absent)" : userns_clone)
                  << " max_user_namespaces="
                  << (max_userns.empty() ? "(absent)" : max_userns)
                  << " criu=" << (criu_path.empty() ? "(not found)" : criu_path)
                  << " lz4_compression="
                  << (GetCriuFeatures().compress ? "yes" : "no");

  vector<string> problems;
  // Pinning init identity is necessary to avoid killing a recycled PID during
  // takeover. Check both syscalls before launching a persistent namespace.
  bool pidfds_available = false;
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  int self_pidfd = syscall(SYS_pidfd_open, getpid(), 0);
  if (self_pidfd >= 0) {
    pidfds_available =
        syscall(SYS_pidfd_send_signal, self_pidfd, 0, nullptr, 0) == 0;
    close(self_pidfd);
  }
#endif
  if (!pidfds_available) {
    problems.push_back(
        "pidfd_open/pidfd_send_signal are unavailable or restricted.\n"
        "  Use Linux >= 5.3 and allow these syscalls for safe namespace "
        "teardown.");
  }

  // AppArmor userns restriction (Ubuntu 23.10+): the single most common
  // roadblock. "1" (or higher) blocks unprivileged user namespaces outright.
  if (apparmor == "1" || apparmor == "2") {
    problems.push_back(
        "unprivileged user namespaces are restricted by AppArmor "
        "(/proc/sys/kernel/apparmor_restrict_unprivileged_userns = " +
        apparmor +
        ").\n"
        "  Enable them with:\n"
        "    sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0");
  }

  // Older Debian/Ubuntu knob: unprivileged_userns_clone must be 1 when present.
  if (userns_clone == "0") {
    problems.push_back(
        "unprivileged user namespaces are disabled "
        "(/proc/sys/kernel/unprivileged_userns_clone = 0).\n"
        "  Enable them with:\n"
        "    sudo sysctl -w kernel.unprivileged_userns_clone=1");
  }

  // A zero namespace budget also blocks the unshare(CLONE_NEWUSER).
  if (max_userns == "0") {
    problems.push_back(
        "user namespace creation is disabled "
        "(/proc/sys/user/max_user_namespaces = 0).\n"
        "  Raise the limit with:\n"
        "    sudo sysctl -w user.max_user_namespaces=15000");
  }

  // No usable criu binary means the dump/restore forks would fail with an
  // opaque exec error from inside the namespace.
  if (criu_path.empty()) {
    if (criu_requested.find('/') != string::npos) {
      problems.push_back("the criu binary at BAZEL_CRIU_BINARY=" +
                         criu_requested +
                         " is missing or not executable.");
    } else {
      problems.push_back(
          "no 'criu' binary was found on PATH.\n"
          "  Install criu (>= 3.18; 4.x recommended), or point "
          "BAZEL_CRIU_BINARY at it.\n"
          "  If criu lives in a non-standard dir (e.g. /usr/local/sbin), add "
          "it to PATH.");
    }
  }

  if (problems.empty()) {
    return true;
  }

  string msg =
      "CRIU mode (--criu) is not usable on this host:\n";
  for (const string &p : problems) {
    msg += "  - " + p + "\n";
  }
  msg +=
      "Observed state:\n"
      "  /proc/sys/kernel/apparmor_restrict_unprivileged_userns = " +
      (apparmor.empty() ? "(absent)" : apparmor) +
      "\n"
      "  /proc/sys/kernel/unprivileged_userns_clone = " +
      (userns_clone.empty() ? "(absent)" : userns_clone) +
      "\n"
      "  /proc/sys/user/max_user_namespaces = " +
      (max_userns.empty() ? "(absent)" : max_userns) +
      "\n"
      "  criu binary = " + (criu_path.empty() ? "(not found)" : criu_path) +
      "\n"
      "See CRIU.md for details, or run without --criu.";
  *error = msg;
  return false;
}

int ExecuteDaemonInNamespace(const blaze_util::Path &exe,
                             const vector<string> &args_vector,
                             const map<string, EnvVarValue> &env,
                             const blaze_util::Path &daemon_output,
                             bool daemon_output_append,
                             const blaze_util::Path &binaries_dir,
                             const blaze_util::Path &server_dir,
                             BlazeServerStartup **server_startup) {
  const blaze_util::Path pid_file = server_dir.GetRelative(kServerPidFile);
  const string daemonize = binaries_dir.GetRelative("daemonize").AsNativePath();

  // Do not let the new JVM read host identity left by a previous server.
  for (const char *name : {"server_info.rawproto", "server.host_pid"}) {
    const auto path = server_dir.GetRelative(name);
    if (blaze_util::PathExists(path) && !blaze_util::UnlinkPath(path)) {
      BAZEL_DIE(blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR)
          << "criu: could not remove stale " << path.AsPrintablePath();
    }
  }

  // Same daemonize invocation as ExecuteDaemon, minus the cgroup/systemd
  // options: a rootless user namespace cannot drive systemd-run, and CRIU does
  // not coexist cleanly with cgroup placement here.
  vector<string> fg_argv = {daemonize, "-l", daemon_output.AsNativePath(), "-p",
                            pid_file.AsNativePath()};
  if (daemon_output_append) {
    fg_argv.push_back("-a");
  }
  fg_argv.push_back("--");
  fg_argv.push_back(exe.AsNativePath());
  fg_argv.insert(fg_argv.end(), args_vector.begin(), args_vector.end());

  InitControlStorage ctl(server_dir.GetParent(), /*serve_control=*/true);
  int fg_code = 0;
  int init_host_pid = 0;
  if (!RunInNamespace(fg_argv, EnvToStrings(env), server_dir, &ctl.ctl,
                      &fg_code, &init_host_pid) ||
      fg_code != 0) {
    KillNamespacedServer(server_dir);
    BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
        << "criu: failed to start namespaced server (daemonize exit "
        << fg_code << "). See " << daemon_output.AsPrintablePath();
  }

  // daemonize has exited, so the pid file exists and holds the server's
  // *namespace-local* pid. Resolve it to the host pid (within our init's PID
  // namespace) and publish its host identity so this host-side
  // launcher (and later clients) can attach over loopback.
  int ns_pid;
  if (!ReadPidFile(pid_file, &ns_pid)) {
    KillNamespacedServer(server_dir);
    BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
        << "criu: failed to read namespaced server pid from "
        << pid_file.AsPrintablePath();
  }
  const string ns_namespace = PidNamespaceOf(init_host_pid);
  if (ns_namespace.empty()) {
    KillNamespacedServer(server_dir);
    BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
        << "criu: namespaced init exited before server startup.";
  }
  int host_pid =
      MakeReachable(ns_pid, ns_namespace, server_dir, /*timeout_secs=*/10);
  if (host_pid < 0) {
    KillNamespacedServer(server_dir);
    BAZEL_DIE(blaze_exit_code::INTERNAL_ERROR)
        << "criu: could not make namespaced server (ns pid " << ns_pid
        << ") reachable.";
  }

  // Leave server.pid.txt namespace-local for the JVM's PidFileWatcher.
  *server_startup = new AlwaysAliveServerStartup();
  return host_pid;
}

bool CriuRestore(const blaze_util::Path& output_base) {
  const auto images = output_base.GetRelative(kCriuImagesSubdir);
  const auto server = output_base.GetRelative("server");
  auto failed = [&]() {
    KillNamespacedServer(server);
    // Namespace creation can fail before an init record is written. Do not
    // leave its prepared local PID or stale connection metadata for a later
    // client to mistake for a live server.
    for (const char* name : {kServerPidFile, kServerInfoName, "cmdline",
                             "server.host_pid", "server.starttime"}) {
      blaze_util::UnlinkPath(server.GetRelative(name));
    }
    return false;
  };
  int ns_pid;
  if (!criu_internal::PrepareRestore(output_base, &ns_pid)) return failed();

  BAZEL_LOG(USER) << "criu: restoring server from " << images.AsPrintablePath()
                  << CheckpointCreated(images);
  auto args =
      criu_internal::RestoreArgs(CriuBinary(), images, GetCriuFeatures());
  vector<string> env;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
    env.push_back(*e);
  }
  InitControlStorage ctl(output_base, /*serve_control=*/true);
  int code = -1, init_pid = 0;
  const auto restore_start = GetMillisecondsMonotonic();
  if (!RunInNamespace(args, env, server, &ctl.ctl, &code, &init_pid,
                      /*timeout_secs=*/300) ||
      code != 0) {
    BAZEL_LOG(USER) << "criu: restore failed (exit " << code << "); see "
                    << images.GetRelative("restore.log").AsPrintablePath();
    return failed();
  }
  BAZEL_LOG(USER) << "criu: restore took "
                  << GetMillisecondsMonotonic() - restore_start << "ms";
  const string ns_namespace = PidNamespaceOf(init_pid);
  int host_pid = ns_namespace.empty()
                     ? -1
                     : MakeReachable(ns_pid, ns_namespace, server,
                                     /*timeout_secs=*/30);
  if (host_pid < 0) {
    BAZEL_LOG(USER) << "criu: restored server identity could not be reconciled";
    return failed();
  }
  BAZEL_LOG(USER) << "criu: restored server at host PID " << host_pid;
  return true;
}

int CriuCheckpoint(const blaze_util::Path& output_base,
                   const string& install_key, bool stop,
                   const std::function<bool()>& cancelled) {
  if (cancelled()) return blaze_exit_code::INTERRUPTED;
  const auto images = output_base.GetRelative(kCriuImagesSubdir);
  const auto key_file = images.GetRelative(kInstallKeyFile);
  // A failed overwrite must never retain a previous checkpoint's success key.
  blaze_util::UnlinkPath(key_file);
  // Capture the running server's actual startup arguments, not those of this
  // checkpoint client (which may have different --host_jvm_args). Capture now:
  // the stop-after-dump path may remove the live server directory.
  string startup_args;
  if (!blaze_util::ReadFile(output_base.GetRelative("server/cmdline"),
                            &startup_args) ||
      startup_args.empty()) {
    BAZEL_LOG(USER) << "criu: could not read running server startup arguments";
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  string token;
  if (!blaze_util::ReadFile(
          output_base.GetRelative("server/checkpoint.request"), &token) ||
      token.empty()) {
    BAZEL_LOG(USER) << "criu: missing checkpoint request";
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  string path = ControlSocketPath(output_base).AsNativePath();
  struct sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    BAZEL_LOG(USER) << "criu: control socket path is too long: " << path;
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  int connected =
      connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
  if (connected < 0 && errno != EINPROGRESS) {
    close(fd);
    BAZEL_LOG(USER) << "criu: no namespaced init at " << path;
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  struct pollfd ready = {fd, POLLOUT, 0};
  if (poll(&ready, 1, 1000) <= 0) {
    close(fd);
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  const string command =
      (stop ? "CHECKPOINT_STOP " : "CHECKPOINT ") + token + "\n";
  const auto dump_start = GetMillisecondsMonotonic();
  if (send(fd, command.data(), command.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(command.size())) {
    close(fd);
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  BAZEL_LOG(USER) << "criu: requesting "
                  << (stop ? "checkpoint+stop" : "checkpoint") << " -> "
                  << images.AsPrintablePath();
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
  bool aborting = false;
  string response;
  while (response.size() < 1024) {
    if (!aborting &&
        (cancelled() || std::chrono::steady_clock::now() >= deadline)) {
      aborting = true;
      // The init acknowledges only after the dump has been killed and reaped.
      (void)!send(fd, "CANCEL\n", 7, MSG_NOSIGNAL);
      shutdown(fd, SHUT_WR);
      deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
    if (aborting && std::chrono::steady_clock::now() >= deadline) break;
    struct pollfd reply = {fd, POLLIN, 0};
    int r = poll(&reply, 1, 50);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) break;
    if (r == 0) continue;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, n);
    if (response.find('\n') != string::npos) break;
  }
  close(fd);
  BAZEL_LOG(USER) << "criu: dump took "
                  << GetMillisecondsMonotonic() - dump_start << "ms";
  if (cancelled()) return blaze_exit_code::INTERRUPTED;
  if (aborting) return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  if (response.compare(0, 3, "OK ") != 0) {
    BAZEL_LOG(USER) << "criu: checkpoint failed: " << Stripped(response)
                    << " (see "
                    << images.GetRelative("dump.log").AsPrintablePath() << ")";
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  if (!WriteIdentityFile(images.GetRelative(kStartupArgsFile), startup_args) ||
      !WriteIdentityFile(key_file, install_key)) {
    BAZEL_LOG(USER)
        << "criu: could not record checkpoint compatibility metadata";
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  if (stop) KillNamespacedServer(output_base.GetRelative("server"));
  BAZEL_LOG(USER) << "criu: " << Stripped(response.substr(3))
                  << CheckpointCreated(images);
  return blaze_exit_code::SUCCESS;
}

}  // namespace blaze
#endif  // __linux__
