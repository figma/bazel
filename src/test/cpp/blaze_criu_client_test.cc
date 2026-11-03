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
#include "src/main/cpp/blaze_criu_client.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

#include "googletest/include/gtest/gtest.h"
#include "src/main/cpp/blaze_criu.h"
#include "src/main/cpp/util/exit_code.h"
#include "src/main/cpp/util/file.h"

namespace blaze {
namespace {
using namespace std::chrono_literals;

class CheckpointClientTest : public ::testing::Test,
                             public command_server::CommandServer::Service {
 protected:
  void SetUp() override {
    output_base_ =
        blaze_util::Path(::testing::TempDir())
            .GetRelative("checkpoint-client-" + std::to_string(sequence_++));
    ASSERT_TRUE(
        blaze_util::MakeDirectories(output_base_.GetRelative("server"), 0700));
    int port;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(this);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    client_ = command_server::CommandServer::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                            grpc::InsecureChannelCredentials()));
  }

  void TearDown() override {
    ReleaseWork();
    blaze_util::UnlinkPath(
        output_base_.GetRelative("server/checkpoint.request"));
    blaze_util::UnlinkPath(CriuCheckpointSentinelPath(output_base_));
    server_->Shutdown();
    server_->Wait();
    blaze_util::UnlinkPath(
        output_base_.GetRelative("server/checkpoint.request"));
    blaze_util::UnlinkPath(CriuCheckpointSentinelPath(output_base_));
    blaze_util::RemoveRecursively(output_base_);
  }

  grpc::Status Run(
      grpc::ServerContext* context, const command_server::RunRequest* request,
      grpc::ServerWriter<command_server::RunResponse>* writer) override {
    command_server::RunResponse response;
    response.set_cookie("response");
    response.set_command_id("checkpoint-id");
    writer->Write(response);
    if (reject_) {
      response.set_finished(true);
      response.set_exit_code(2);
      writer->Write(response);
      return grpc::Status::OK;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      active_context_ = context;
      started_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return allow_ready_; });
    }
    const std::string prefix = "--checkpoint_token=";
    std::string token = request->arg(1).substr(prefix.size());
    std::string lease;
    if (blaze_util::ReadFile(
            output_base_.GetRelative("server/checkpoint.request"), &lease) &&
        lease == token) {
      blaze_util::WriteFile(token, CriuCheckpointSentinelPath(output_base_));
      // Deliberately ignore RPC cancellation: --tcp-close must not release the
      // command lock while a dump is still running. The file lease releases it.
      while (blaze_util::PathExists(
                 output_base_.GetRelative("server/checkpoint.request")) &&
             blaze_util::PathExists(CriuCheckpointSentinelPath(output_base_))) {
        std::this_thread::sleep_for(2ms);
      }
      blaze_util::UnlinkPath(CriuCheckpointSentinelPath(output_base_));
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
      condition_.notify_all();
    }
    return grpc::Status::OK;
  }

  void ReleaseWork() {
    std::lock_guard<std::mutex> lock(mutex_);
    allow_ready_ = true;
    condition_.notify_all();
  }

  void AwaitStarted() {
    std::unique_lock<std::mutex> lock(mutex_);
    ASSERT_TRUE(condition_.wait_for(lock, 3s, [&] { return started_; }));
  }

  void ExpectReleased() {
    std::unique_lock<std::mutex> lock(mutex_);
    EXPECT_TRUE(condition_.wait_for(lock, 3s, [&] { return released_; }));
    EXPECT_FALSE(blaze_util::PathExists(
        output_base_.GetRelative("server/checkpoint.request")));
    EXPECT_FALSE(
        blaze_util::PathExists(CriuCheckpointSentinelPath(output_base_)));
  }

  int Checkpoint(
      const std::function<int()>& dump,
      const std::function<bool()>& cancelled = [] { return false; },
      std::chrono::milliseconds timeout = 3s) {
    CheckpointTimeouts timeouts;
    timeouts.quiesce = timeout;
    timeouts.rpc_cleanup = 200ms;
    return CoordinateCheckpoint(client_, "request", "response", output_base_,
                                dump, cancelled, timeouts);
  }

  static int sequence_;
  blaze_util::Path output_base_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<command_server::CommandServer::StubInterface> client_;
  std::mutex mutex_;
  std::condition_variable condition_;
  grpc::ServerContext* active_context_ = nullptr;
  bool started_ = false;
  bool allow_ready_ = false;
  bool released_ = false;
  bool reject_ = false;
};
int CheckpointClientTest::sequence_ = 0;

TEST_F(CheckpointClientTest, WaitsForActiveWorkAndIgnoresStaleSentinel) {
  blaze_util::WriteFile("old-request",
                        CriuCheckpointSentinelPath(output_base_));
  std::atomic<bool> dumped{false};
  auto invocation = std::async(std::launch::async, [&] {
    return Checkpoint([&] {
      dumped = true;
      return 0;
    });
  });
  AwaitStarted();
  EXPECT_FALSE(dumped);
  EXPECT_EQ(invocation.wait_for(50ms), std::future_status::timeout);
  ReleaseWork();
  ASSERT_EQ(invocation.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(invocation.get(), 0);
  EXPECT_TRUE(dumped);
  ExpectReleased();
}

TEST_F(CheckpointClientTest, FailedDumpReleasesServer) {
  ReleaseWork();
  EXPECT_EQ(Checkpoint([] { return 36; }), 36);
  ExpectReleased();
}

TEST_F(CheckpointClientTest, CancellationDuringDumpReleasesServer) {
  ReleaseWork();
  EXPECT_EQ(Checkpoint([] { return 8; }), 8);
  ExpectReleased();
}

TEST_F(CheckpointClientTest, TransportSeveredDuringDumpDoesNotHangCleanup) {
  ReleaseWork();
  EXPECT_EQ(Checkpoint([&] {
              std::lock_guard<std::mutex> lock(mutex_);
              active_context_->TryCancel();
              return 0;
            }),
            0);
  ExpectReleased();
}

TEST_F(CheckpointClientTest, ReadinessTimeoutCancelsRpcBeforeBoundedJoin) {
  std::atomic<bool> dumped{false};
  auto begin = std::chrono::steady_clock::now();
  EXPECT_EQ(Checkpoint(
                [&] {
                  dumped = true;
                  return 0;
                },
                [] { return false; }, 100ms),
            36);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
  EXPECT_FALSE(dumped);
  // The server deliberately did not finish the RPC; it only acquires the lock
  // after the launcher returned. The revoked token prevents a late park.
  ReleaseWork();
  ExpectReleased();
}

TEST_F(CheckpointClientTest, CancellationWhileQueuedCannotLeaveLateSentinel) {
  std::atomic<bool> cancelled{false};
  auto invocation = std::async(std::launch::async, [&] {
    return Checkpoint([] { return 0; }, [&] { return cancelled.load(); });
  });
  AwaitStarted();
  cancelled = true;
  ASSERT_EQ(invocation.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(invocation.get(), 8);
  ReleaseWork();
  ExpectReleased();
}

TEST_F(CheckpointClientTest, ServerRejectionDoesNotWaitForReadinessTimeout) {
  reject_ = true;
  auto begin = std::chrono::steady_clock::now();
  EXPECT_EQ(Checkpoint([] {
              ADD_FAILURE() << "Dump after rejection";
              return 0;
            }),
            2);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
}

}  // namespace
}  // namespace blaze
