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

#include "src/main/cpp/blaze_util.h"
#include "googletest/include/gtest/gtest.h"

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
#endif

}  // namespace
}  // namespace blaze
