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

#include <condition_variable>
#include <mutex>
#include <thread>

#include "src/main/cpp/blaze_criu.h"
#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/exit_code.h"
#include "src/main/cpp/util/file.h"
#include "src/main/cpp/util/logging.h"

namespace blaze {
namespace {

struct CheckpointRpc {
  std::shared_ptr<command_server::CommandServer::StubInterface> client;
  grpc::ClientContext context;
  std::mutex mutex;
  std::condition_variable finished;
  bool done = false;
  int code = blaze_exit_code::INTERNAL_ERROR;
  std::string command_id;
};

// Every object used by the reader is owned by its shared state. If a broken
// transport fails even to honor TryCancel, cleanup can detach it safely instead
// of hanging the launcher or accessing a destroyed BlazeServer/ClientContext.
class CheckpointRequest {
 public:
  CheckpointRequest(
      std::shared_ptr<command_server::CommandServer::StubInterface> client,
      const std::string& cookie, const std::string& response_cookie,
      const std::string& token, std::chrono::milliseconds cleanup)
      : state_(std::make_shared<CheckpointRpc>()),
        cleanup_(cleanup),
        cookie_(cookie) {
    state_->client = std::move(client);
    state_->context.set_deadline(std::chrono::system_clock::now() +
                                 std::chrono::minutes(8));
    thread_ = std::thread([state = state_, cookie, response_cookie, token]() {
      command_server::RunRequest request;
      request.set_cookie(cookie);
      request.set_block_for_lock(true);
      request.set_client_description("pid=" + GetProcessIdAsString() +
                                     " (for checkpoint)");
      request.add_arg("checkpoint");
      request.add_arg("--checkpoint_token=" + token);
      auto reader = state->client->Run(&state->context, request);
      command_server::RunResponse response;
      int code = blaze_exit_code::INTERNAL_ERROR;
      while (reader->Read(&response)) {
        if (response.cookie() != response_cookie) {
          state->context.TryCancel();
          break;
        }
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          if (!response.command_id().empty())
            state->command_id = response.command_id();
        }
        if (response.finished()) code = response.exit_code();
      }
      reader->Finish();
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->code = code;
        state->done = true;
      }
      state->finished.notify_all();
    });
  }

  bool Done(int* code) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    *code = state_->code;
    return state_->done;
  }

  void CancelServer() {
    std::string command_id;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      command_id = state_->command_id;
    }
    if (command_id.empty()) return;
    grpc::ClientContext cancel_context;
    cancel_context.set_deadline(std::chrono::system_clock::now() +
                                std::chrono::seconds(1));
    command_server::CancelRequest request;
    command_server::CancelResponse response;
    request.set_cookie(cookie_);
    request.set_command_id(command_id);
    state_->client->Cancel(&cancel_context, request, &response);
  }

  ~CheckpointRequest() {
    // Cancel the transport first; do not join a reader still blocked in Read.
    state_->context.TryCancel();
    std::unique_lock<std::mutex> lock(state_->mutex);
    bool done =
        state_->finished.wait_for(lock, cleanup_, [&] { return state_->done; });
    lock.unlock();
    if (done) {
      thread_.join();
    } else {
      BAZEL_LOG(WARNING)
          << "criu: checkpoint RPC did not stop after cancellation";
      thread_.detach();
    }
  }

 private:
  std::shared_ptr<CheckpointRpc> state_;
  std::chrono::milliseconds cleanup_;
  std::thread thread_;
  std::string cookie_;
};

class CheckpointLease {
 public:
  explicit CheckpointLease(const blaze_util::Path& output_base)
      : request(output_base.GetRelative("server/checkpoint.request")),
        sentinel(CriuCheckpointSentinelPath(output_base)) {}
  ~CheckpointLease() { Release(); }
  bool Release() {
    bool ok = true;
    for (const auto& path : {request, sentinel}) {
      if (blaze_util::PathExists(path) && !blaze_util::UnlinkPath(path))
        ok = false;
    }
    return ok;
  }
  const blaze_util::Path request;
  const blaze_util::Path sentinel;
};

}  // namespace

int CoordinateCheckpoint(
    std::shared_ptr<command_server::CommandServer::StubInterface> client,
    const std::string& request_cookie, const std::string& response_cookie,
    const blaze_util::Path& output_base, const std::function<int()>& dump,
    const std::function<bool()>& cancelled,
    const CheckpointTimeouts& timeouts) {
  CheckpointLease lease(output_base);
  if (!lease.Release()) return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  const std::string token =
      GetProcessIdAsString() + "-" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  if (!blaze_util::WriteFile(token, lease.request, 0600)) {
    return blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  CheckpointRequest rpc(std::move(client), request_cookie, response_cookie,
                        token, timeouts.rpc_cleanup);
  auto deadline = std::chrono::steady_clock::now() + timeouts.quiesce;
  int result = blaze_exit_code::INTERNAL_ERROR;
  while (true) {
    if (cancelled()) {
      result = blaze_exit_code::INTERRUPTED;
      break;
    }
    std::string ready;
    if (blaze_util::ReadFile(lease.sentinel, &ready) && ready == token) {
      result = dump();
      break;
    }
    if (rpc.Done(&result)) {
      if (result == 0) result = blaze_exit_code::INTERNAL_ERROR;
      BAZEL_LOG(USER) << "criu: checkpoint command exited before readiness";
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      BAZEL_LOG(USER)
          << "criu: server did not become quiescent; aborting checkpoint";
      result = blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  // Revoke the lease before RPC cleanup. A late lock acquirer must not create
  // a fresh sentinel and park after the launcher has already given up.
  if (!lease.Release()) {
    BAZEL_LOG(USER) << "criu: could not remove checkpoint coordination files";
    result = blaze_exit_code::LOCAL_ENVIRONMENTAL_ERROR;
  }
  if (result != blaze_exit_code::SUCCESS) rpc.CancelServer();
  return result;
}

}  // namespace blaze
