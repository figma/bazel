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

#include <string>

#ifdef __linux__
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#endif

#include "googletest/include/gtest/gtest.h"
#include "src/main/cpp/blaze_criu_internal.h"
#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/strings.h"

namespace blaze {
namespace {

class CriuModeTest : public ::testing::Test {
 protected:
  void TearDown() override { SetCriuModeActive(false); }
};

TEST_F(CriuModeTest, InactiveDoesNotRequireCriuBinary) {
  SetCriuModeActive(false);
  WithEnvVars env({{"BAZEL_CRIU_BINARY",
                    EnvVarValue(EnvVarAction::SET, "/nonexistent/criu")}});
  std::string error;
  EXPECT_FALSE(CriuModeActive());
  EXPECT_TRUE(CriuPreflight(&error));
  EXPECT_TRUE(error.empty());
}

#ifdef __linux__
TEST_F(CriuModeTest, ReportsMissingExecutableAndObservedHostSettings) {
  SetCriuModeActive(true);
  WithEnvVars env({{"BAZEL_CRIU_BINARY",
                    EnvVarValue(EnvVarAction::SET, "/nonexistent/criu")}});
  std::string error;
  EXPECT_TRUE(CriuModeActive());
  EXPECT_FALSE(CriuPreflight(&error));
  EXPECT_NE(error.find("BAZEL_CRIU_BINARY=/nonexistent/criu"), std::string::npos);
  EXPECT_NE(error.find("missing or not executable"), std::string::npos);
  EXPECT_NE(error.find("/proc/sys/user/max_user_namespaces"), std::string::npos);
}

TEST_F(CriuModeTest, ExecutableDirectoryIsNotACriuBinary) {
  SetCriuModeActive(true);
  WithEnvVars env({{"BAZEL_CRIU_BINARY",
                    EnvVarValue(EnvVarAction::SET, "/tmp")}});
  std::string error;
  EXPECT_FALSE(CriuPreflight(&error));
  EXPECT_NE(error.find("BAZEL_CRIU_BINARY=/tmp"), std::string::npos);
}

// Exercise PID-1 teardown without a private proc mount, which container hosts
// can prohibit independently of user/PID namespace creation.
class NamespaceTeardownTest : public ::testing::Test {
protected:
  void SetUp() override {
    std::string pattern = "/tmp/criu-identity-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    ASSERT_NE(mkdtemp(name.data()), nullptr);
    output_base_ = blaze_util::Path(name.data());
    server_dir_ = output_base_.GetRelative("server");
    ASSERT_EQ(mkdir(server_dir_.AsNativePath().c_str(), 0700), 0);
  }

  void TearDown() override {
    if (init_ > 0) {
      kill(init_, SIGKILL);
      while (waitpid(init_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
    for (const char *name : {"server.init_host_pid", "server.init_starttime",
                             "server.host_pid", "server.pid.txt"}) {
      blaze_util::UnlinkPath(server_dir_.GetRelative(name));
    }
    rmdir(server_dir_.AsNativePath().c_str());
    rmdir(output_base_.AsNativePath().c_str());
  }

  static int Init(void *arg) {
    int *ready = static_cast<int *>(arg);
    close(ready[0]);
    pid_t server = fork();
    if (server < 0)
      return 1;
    if (server == 0) {
      close(ready[1]);
      for (;;)
        pause();
    }
    char value = 1;
    (void)!write(ready[1], &value, 1);
    close(ready[1]);
    while (waitpid(-1, nullptr, 0) >= 0 || errno == EINTR) {
    }
    return 0;
  }

  bool StartNamespace() {
    int ready[2];
    EXPECT_EQ(pipe(ready), 0);
    std::vector<char> stack(1024 * 1024);
    init_ = clone(Init, stack.data() + stack.size(),
                  CLONE_NEWUSER | CLONE_NEWPID | SIGCHLD, ready);
    int clone_errno = errno;
    close(ready[1]);
    if (init_ < 0) {
      close(ready[0]);
      EXPECT_TRUE(clone_errno == EPERM || clone_errno == EINVAL ||
                  clone_errno == ENOSYS);
      return false;
    }
    char value;
    EXPECT_EQ(read(ready[0], &value, 1), 1);
    close(ready[0]);
    std::ifstream children("/proc/" + std::to_string(init_) + "/task/" +
                           std::to_string(init_) + "/children");
    children >> server_;
    EXPECT_GT(server_, 0);
    Write("server.init_host_pid", std::to_string(init_));
    Write("server.init_starttime", StartTime(init_));
    Write("server.host_pid", std::to_string(server_));
    Write("server.pid.txt", "2");
    return true;
  }

  static std::string StartTime(int pid) {
    std::string stat;
    if (!blaze_util::ReadFile("/proc/" + std::to_string(pid) + "/stat",
                              &stat)) {
      return "";
    }
    std::istringstream fields(stat.substr(stat.rfind(')') + 1));
    std::string value;
    for (int i = 0; i <= 19; ++i)
      fields >> value;
    return value;
  }

  void Write(const char *name, const std::string &value) {
    ASSERT_TRUE(blaze_util::WriteFile(value, server_dir_.GetRelative(name)));
  }

  void ExpectNamespaceGone() {
    // waitpid also verifies init exit; killed namespace children are reaped by
    // the kernel before PID 1 exits.
    ASSERT_EQ(waitpid(init_, nullptr, 0), init_);
    init_ = -1;
    EXPECT_EQ(kill(server_, 0), -1);
    EXPECT_EQ(errno, ESRCH);
    EXPECT_FALSE(
        blaze_util::PathExists(server_dir_.GetRelative("server.pid.txt")));
    EXPECT_FALSE(
        blaze_util::PathExists(server_dir_.GetRelative("server.host_pid")));
    EXPECT_FALSE(blaze_util::PathExists(
        server_dir_.GetRelative("server.init_host_pid")));
  }

  blaze_util::Path output_base_;
  blaze_util::Path server_dir_;
  pid_t init_ = -1;
  int server_ = -1;
};

TEST_F(NamespaceTeardownTest, OrdinaryServerHasNoInitRecord) {
  EXPECT_FALSE(KillNamespacedServer(server_dir_));
}

TEST_F(NamespaceTeardownTest, TakeoverKillsInitAndServerWithoutCriuFlag) {
  if (!StartNamespace())
    GTEST_SKIP() << "User/PID namespaces unavailable";
  SetCriuModeActive(false);
  EXPECT_TRUE(KillNamespacedServer(server_dir_));
  ExpectNamespaceGone();
}

TEST_F(NamespaceTeardownTest, ForcedCancellationKillsNamespaceNotProcessGroup) {
  if (!StartNamespace())
    GTEST_SKIP() << "User/PID namespaces unavailable";
  EXPECT_NE(getpgid(server_), server_);
  EXPECT_TRUE(KillServerProcess(server_, output_base_));
  ExpectNamespaceGone();
}

TEST_F(NamespaceTeardownTest, GracefulExitCleansDeadInitRecord) {
  if (!StartNamespace())
    GTEST_SKIP() << "User/PID namespaces unavailable";
  EXPECT_EQ(kill(server_, SIGTERM), 0);
  siginfo_t info = {};
  ASSERT_EQ(waitid(P_PID, init_, &info, WEXITED | WNOWAIT), 0);
  EXPECT_TRUE(KillNamespacedServer(server_dir_));
  ExpectNamespaceGone();
}

TEST_F(NamespaceTeardownTest, RecycledNamespaceInitPidIsNotKilled) {
  if (!StartNamespace())
    GTEST_SKIP() << "User/PID namespaces unavailable";
  Write("server.init_starttime", "0");
  EXPECT_TRUE(KillNamespacedServer(server_dir_));
  EXPECT_EQ(kill(init_, 0), 0);
  EXPECT_EQ(kill(server_, 0), 0);
}

TEST_F(NamespaceTeardownTest, UnrelatedHostProcessIsNotKilled) {
  Write("server.init_host_pid", std::to_string(getpid()));
  Write("server.init_starttime", StartTime(getpid()));
  Write("server.pid.txt", "2");
  EXPECT_TRUE(KillNamespacedServer(server_dir_));
  EXPECT_EQ(kill(getpid(), 0), 0);
  EXPECT_FALSE(
      blaze_util::PathExists(server_dir_.GetRelative("server.pid.txt")));
}

TEST_F(NamespaceTeardownTest, MalformedRecordDoesNotFallBackToLocalPid) {
  Write("server.init_host_pid", "invalid");
  Write("server.pid.txt", "2");
  EXPECT_TRUE(KillNamespacedServer(server_dir_));
  EXPECT_FALSE(
      blaze_util::PathExists(server_dir_.GetRelative("server.pid.txt")));
}

class CheckpointControlTest : public NamespaceTeardownTest {
 protected:
  void SetUp() override {
    NamespaceTeardownTest::SetUp();
    images_ = output_base_.GetRelative("criu");
    ASSERT_TRUE(blaze_util::MakeDirectories(images_, 0700));
    binary_ = output_base_.GetRelative("fake-criu");
    Write("server.pid.txt", "3");
    Write("checkpoint.request", "request-token");
    Write("checkpoint.sentinel", "request-token");
    blaze_util::JoinStrings(args_, '\0', &serialized_args_);
    Write("cmdline", serialized_args_);
    ASSERT_TRUE(
        blaze_util::WriteFile("connection-metadata",
                              server_dir_.GetRelative("server_info.rawproto")));
    ASSERT_TRUE(blaze_util::WriteFile(
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > '" +
            output_base_.GetRelative("dump-args").AsNativePath() +
            "'\nexit 0\n",
        binary_, 0700));
  }

  void TearDown() override {
    if (control_ > 0) {
      kill(control_, SIGKILL);
      waitpid(control_, nullptr, 0);
    }
    NamespaceTeardownTest::TearDown();
    blaze_util::RemoveRecursively(output_base_);
  }

  void StartControl(bool compress = false) {
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(listener, 0);
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    auto socket_path = ControlSocketPath(output_base_).AsNativePath();
    ASSERT_LT(socket_path.size(), sizeof(addr.sun_path));
    memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);
    ASSERT_EQ(
        bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)),
        0);
    ASSERT_EQ(listen(listener, 1), 0);
    std::string pid_file =
        server_dir_.GetRelative("server.pid.txt").AsNativePath();
    std::string ns_pid = images_.GetRelative("ns-pid").AsNativePath();
    std::string info =
        server_dir_.GetRelative("server_info.rawproto").AsNativePath();
    std::string snapshot =
        images_.GetRelative("server_info.rawproto").AsNativePath();
    std::string sentinel =
        server_dir_.GetRelative("checkpoint.sentinel").AsNativePath();
    std::string request =
        server_dir_.GetRelative("checkpoint.request").AsNativePath();
    std::string images = images_.AsNativePath();
    std::string binary = binary_.AsNativePath();
    criu_internal::InitControl ctl = {
        socket_path.c_str(), binary.c_str(), images.c_str(),   ns_pid.c_str(),
        pid_file.c_str(),    info.c_str(),   snapshot.c_str(), sentinel.c_str(),
        request.c_str(),     compress};
    control_ = fork();
    ASSERT_GE(control_, 0);
    if (control_ == 0) {
      int conn = accept(listener, nullptr, nullptr);
      bool stop = criu_internal::HandleCheckpointRequest(ctl, conn);
      close(conn);
      _exit(stop ? 2 : 0);
    }
    close(listener);
  }

  void ExpectControlExit(bool stop) {
    int status;
    ASSERT_EQ(waitpid(control_, &status, 0), control_);
    control_ = -1;
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), stop ? 2 : 0);
  }

  blaze_util::Path images_;
  blaze_util::Path binary_;
  std::vector<std::string> args_ = {"bazel",
                                    "-Xmx1g",
                                    "-Dsetting=first",
                                    "-Dsetting=last",
                                    "--criu",
                                    "--client_debug=false",
                                    "--option_sources=original"};
  std::string serialized_args_;
  int control_ = -1;
};

TEST_F(CheckpointControlTest, LeaveRunningRecordsMetadataAndDumpFlags) {
  StartControl();
  EXPECT_EQ(
      CriuCheckpoint(output_base_, "install-key", false, [] { return false; }),
      0);
  ExpectControlExit(false);
  std::string args, pid, key, snapshot, startup_args;
  ASSERT_TRUE(
      blaze_util::ReadFile(output_base_.GetRelative("dump-args"), &args));
  EXPECT_NE(args.find("dump\n--tree\n3\n"), std::string::npos);
  for (const auto* flag :
       {"--leave-running", "--tcp-close", "--file-locks", "--unprivileged"}) {
    EXPECT_NE(args.find(flag), std::string::npos);
  }
  EXPECT_EQ(args.find("--compress\n"), std::string::npos);
  ASSERT_TRUE(blaze_util::ReadFile(images_.GetRelative("ns-pid"), &pid));
  EXPECT_EQ(pid, "3");
  ASSERT_TRUE(blaze_util::ReadFile(images_.GetRelative("install-key"), &key));
  EXPECT_EQ(key, "install-key");
  ASSERT_TRUE(blaze_util::ReadFile(images_.GetRelative("server_info.rawproto"),
                                   &snapshot));
  EXPECT_EQ(snapshot, "connection-metadata");
  ASSERT_TRUE(
      blaze_util::ReadFile(images_.GetRelative("startup-args"), &startup_args));
  EXPECT_EQ(startup_args, serialized_args_);
  EXPECT_TRUE(CriuCheckpointExists(output_base_, "install-key", args_));
}

TEST_F(CheckpointControlTest, CompressesDumpWhenSupported) {
  StartControl(/*compress=*/true);
  EXPECT_EQ(CriuCheckpoint(output_base_, "key", false, [] { return false; }),
            0);
  ExpectControlExit(false);
  std::string args;
  ASSERT_TRUE(
      blaze_util::ReadFile(output_base_.GetRelative("dump-args"), &args));
  EXPECT_NE(args.find("--compress\n"), std::string::npos);
}

TEST_F(CheckpointControlTest, ProbesHelpWithoutShellAndScalesRestoreThreads) {
  // Literal quotes and spaces in the executable path must never reach a shell.
  const auto quoted = output_base_.GetRelative("criu ' quoted");
  ASSERT_TRUE(blaze_util::WriteFile(
      "#!/bin/sh\n[ \"$1\" = --help ] || exit 1\n"
      "printf '  --compress\\n  --decompress-threads N\\n' >&2\n",
      quoted, 0700));
  auto features = criu_internal::ProbeCriuFeatures(quoted.AsNativePath());
  EXPECT_TRUE(features.compress);
  EXPECT_TRUE(features.decompress_threads);
  const auto args =
      criu_internal::RestoreArgs(quoted.AsNativePath(), images_, features);
  const auto flag = std::find(args.begin(), args.end(), "--decompress-threads");
  ASSERT_NE(flag, args.end());
  ASSERT_NE(flag + 1, args.end());
  EXPECT_EQ(*(flag + 1), "0");
}

TEST_F(CheckpointControlTest, OlderOrFailedCriuDoesNotReceiveUnsupportedFlags) {
  for (const char* body :
       {"printf '  --compression\\n  --decompress-threads-extra\\n'\n",
        "printf '  --compress\\n  --decompress-threads N\\n'\nexit 1\n"}) {
    ASSERT_TRUE(blaze_util::WriteFile(std::string("#!/bin/sh\n") + body,
                                      binary_, 0700));
    auto features = criu_internal::ProbeCriuFeatures(binary_.AsNativePath());
    EXPECT_FALSE(features.compress);
    EXPECT_FALSE(features.decompress_threads);
    const auto args =
        criu_internal::RestoreArgs(binary_.AsNativePath(), images_, features);
    EXPECT_EQ(std::find(args.begin(), args.end(), "--decompress-threads"),
              args.end());
  }
}

TEST_F(CheckpointControlTest, HangingProbeIsBoundedAndReaped) {
  auto pid_file = output_base_.GetRelative("probe-pid");
  ASSERT_TRUE(blaze_util::WriteFile("#!/bin/sh\necho $$ > '" +
                                        pid_file.AsNativePath() +
                                        "'\nexec sleep 600\n",
                                    binary_, 0700));
  const auto start = std::chrono::steady_clock::now();
  auto features = criu_internal::ProbeCriuFeatures(
      binary_.AsNativePath(), std::chrono::milliseconds(100));
  EXPECT_FALSE(features.compress);
  EXPECT_FALSE(features.decompress_threads);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(2));
  std::string pid;
  ASSERT_TRUE(blaze_util::ReadFile(pid_file, &pid));
  EXPECT_EQ(kill(std::stoi(pid), 0), -1);
  EXPECT_EQ(errno, ESRCH);
}

TEST_F(CheckpointControlTest, OversizedHelpDoesNotEnableCompression) {
  ASSERT_TRUE(blaze_util::WriteFile("#!/bin/sh\nexec yes -- --compress\n",
                                    binary_, 0700));
  auto features = criu_internal::ProbeCriuFeatures(binary_.AsNativePath());
  EXPECT_FALSE(features.compress);
  EXPECT_FALSE(features.decompress_threads);
}

TEST_F(CheckpointControlTest, CompatibilityUsesImmutableOrderedStartupArgs) {
  StartControl();
  ASSERT_EQ(CriuCheckpoint(output_base_, "key", false, [] { return false; }),
            0);
  ExpectControlExit(false);
  EXPECT_TRUE(CriuCheckpointExists(output_base_, "key", args_));
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "different-binary", args_));

  auto changed = args_;
  changed[1] = "-Xmx2g";
  // Even if a subsequent cold start left matching live arguments, the immutable
  // checkpoint must still reject different JVM settings.
  std::string serialized;
  blaze_util::JoinStrings(changed, '\0', &serialized);
  Write("cmdline", serialized);
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "key", changed));
  EXPECT_TRUE(CriuCheckpointExists(output_base_, "key", args_));

  changed = args_;
  std::swap(changed[2], changed[3]);
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "key", changed));
  changed = args_;
  changed.insert(changed.begin() + 2, "-Xmx1g");
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "key", changed));

  changed = args_;
  changed[5] = "--client_debug=true";
  changed[6] = "--option_sources=another-bazelrc";
  changed.push_back("--connect_timeout_secs=60");
  EXPECT_TRUE(CriuCheckpointExists(output_base_, "key", changed));

  ASSERT_TRUE(blaze_util::UnlinkPath(images_.GetRelative("startup-args")));
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "key", args_));
}

TEST_F(CheckpointControlTest, RecreatesServerFilesForRepeatedRestore) {
  StartControl();
  ASSERT_EQ(CriuCheckpoint(output_base_, "key", false, [] { return false; }),
            0);
  ExpectControlExit(false);
  for (int restore = 0; restore < 2; ++restore) {
    // A graceful JVM shutdown deletes its live files, possibly the whole dir.
    blaze_util::RemoveRecursively(server_dir_);
    int ns_pid = 0;
    ASSERT_TRUE(criu_internal::PrepareRestore(output_base_, &ns_pid));
    EXPECT_EQ(ns_pid, 3);
    std::string contents;
    ASSERT_TRUE(blaze_util::ReadFile(server_dir_.GetRelative("server.pid.txt"),
                                     &contents));
    EXPECT_EQ(contents, "3");  // A newline would make the JVM halt on resume.
    ASSERT_TRUE(blaze_util::ReadFile(
        server_dir_.GetRelative("server_info.rawproto"), &contents));
    EXPECT_EQ(contents, "connection-metadata");
    ASSERT_TRUE(
        blaze_util::ReadFile(server_dir_.GetRelative("cmdline"), &contents));
    EXPECT_EQ(contents, serialized_args_);
    EXPECT_TRUE(CriuCheckpointExists(output_base_, "key", args_));
  }
}

TEST_F(CheckpointControlTest, RestoreReplacesStaleFilesBeforeResuming) {
  StartControl();
  ASSERT_EQ(CriuCheckpoint(output_base_, "key", false, [] { return false; }),
            0);
  ExpectControlExit(false);
  Write("server.pid.txt", "12345");
  Write("server.host_pid", "67890");
  Write("server.starttime", "old-start-time");
  Write("server_info.rawproto", "wrong-port-and-cookies");
  Write("cmdline", "different-jvm-settings");
  int ns_pid = 0;
  ASSERT_TRUE(criu_internal::PrepareRestore(output_base_, &ns_pid));
  std::string contents;
  ASSERT_TRUE(blaze_util::ReadFile(
      server_dir_.GetRelative("server_info.rawproto"), &contents));
  EXPECT_EQ(contents, "connection-metadata");
  ASSERT_TRUE(
      blaze_util::ReadFile(server_dir_.GetRelative("cmdline"), &contents));
  EXPECT_EQ(contents, serialized_args_);
  EXPECT_FALSE(
      blaze_util::PathExists(server_dir_.GetRelative("server.host_pid")));
  EXPECT_FALSE(
      blaze_util::PathExists(server_dir_.GetRelative("server.starttime")));
}

TEST_F(CheckpointControlTest, RestoreRejectsMissingMetadataAndInvalidLocalPid) {
  StartControl();
  ASSERT_EQ(CriuCheckpoint(output_base_, "key", false, [] { return false; }),
            0);
  ExpectControlExit(false);
  ASSERT_TRUE(blaze_util::WriteFile("1", images_.GetRelative("ns-pid")));
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "key", args_));
  int ns_pid;
  EXPECT_FALSE(criu_internal::PrepareRestore(output_base_, &ns_pid));
  ASSERT_TRUE(blaze_util::WriteFile("3", images_.GetRelative("ns-pid")));
  ASSERT_TRUE(
      blaze_util::UnlinkPath(images_.GetRelative("server_info.rawproto")));
  EXPECT_FALSE(CriuCheckpointExists(output_base_, "key", args_));
  EXPECT_FALSE(criu_internal::PrepareRestore(output_base_, &ns_pid));
}

TEST_F(CheckpointControlTest, DefaultStopRequestsTeardownAfterSuccessfulDump) {
  StartControl();
  EXPECT_EQ(CriuCheckpoint(output_base_, "key", true, [] { return false; }), 0);
  ExpectControlExit(true);
}

TEST_F(CheckpointControlTest, FailedDumpReleasesLeaseAndInvalidatesOldImages) {
  ASSERT_TRUE(blaze_util::WriteFile("#!/bin/sh\nexit 1\n", binary_, 0700));
  ASSERT_TRUE(
      blaze_util::WriteFile("stale-key", images_.GetRelative("install-key")));
  StartControl();
  EXPECT_EQ(CriuCheckpoint(output_base_, "key", true, [] { return false; }),
            36);
  ExpectControlExit(false);
  EXPECT_FALSE(blaze_util::PathExists(images_.GetRelative("install-key")));
  EXPECT_FALSE(
      blaze_util::PathExists(server_dir_.GetRelative("checkpoint.request")));
  EXPECT_FALSE(
      blaze_util::PathExists(server_dir_.GetRelative("checkpoint.sentinel")));
}

TEST_F(CheckpointControlTest, FailedArgumentRecordingDoesNotPublishSuccessKey) {
  ASSERT_TRUE(
      blaze_util::MakeDirectories(images_.GetRelative("startup-args"), 0700));
  StartControl();
  EXPECT_EQ(CriuCheckpoint(output_base_, "key", false, [] { return false; }),
            36);
  ExpectControlExit(false);
  EXPECT_FALSE(blaze_util::PathExists(images_.GetRelative("install-key")));
}

TEST_F(CheckpointControlTest, CancelKillsAndReapsDumpBeforeReleasingServer) {
  auto started = output_base_.GetRelative("dump-started");
  ASSERT_TRUE(blaze_util::WriteFile(
      "#!/bin/sh\necho $$ > '" + started.AsNativePath() + "'\nexec sleep 600\n",
      binary_, 0700));
  StartControl();
  EXPECT_EQ(CriuCheckpoint(output_base_, "key", true,
                           [&] { return blaze_util::PathExists(started); }),
            8);
  ExpectControlExit(false);
  std::string pid;
  ASSERT_TRUE(blaze_util::ReadFile(started, &pid));
  EXPECT_EQ(kill(atoi(pid.c_str()), 0), -1);
  EXPECT_EQ(errno, ESRCH);
  EXPECT_FALSE(
      blaze_util::PathExists(server_dir_.GetRelative("checkpoint.request")));
  EXPECT_FALSE(
      blaze_util::PathExists(server_dir_.GetRelative("checkpoint.sentinel")));
}

TEST_F(CheckpointControlTest, RefusesDumpWithoutMatchingReadiness) {
  Write("checkpoint.sentinel", "previous-request");
  StartControl();
  EXPECT_EQ(CriuCheckpoint(output_base_, "key", false, [] { return false; }),
            36);
  ExpectControlExit(false);
  EXPECT_FALSE(blaze_util::PathExists(output_base_.GetRelative("dump-args")));
}
#endif

}  // namespace
}  // namespace blaze
