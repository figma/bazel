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
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sched.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#endif

#include "googletest/include/gtest/gtest.h"
#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/util/file.h"

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
    std::string pattern = ::testing::TempDir() + "criu-identity-XXXXXX";
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
#endif

}  // namespace
}  // namespace blaze
