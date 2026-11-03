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
#ifndef BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_CLIENT_H_
#define BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_CLIENT_H_

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "src/main/cpp/util/path.h"
#include "src/main/protobuf/command_server.grpc.pb.h"

namespace blaze {

// Bounds both readiness and RPC cleanup; injectable short limits support tests
// against an unresponsive server without waiting for production timeouts.
struct CheckpointTimeouts {
  std::chrono::milliseconds quiesce{120000};
  std::chrono::milliseconds rpc_cleanup{2000};
};

// Caller owns the output-base lock throughout. Readiness and release use a
// token-bearing file lease, surviving the RPC stream loss caused by
// --tcp-close.
int CoordinateCheckpoint(
    std::shared_ptr<command_server::CommandServer::StubInterface> client,
    const std::string& request_cookie, const std::string& response_cookie,
    const blaze_util::Path& output_base, const std::function<int()>& dump,
    const std::function<bool()>& cancelled,
    const CheckpointTimeouts& timeouts = CheckpointTimeouts());

}  // namespace blaze
#endif  // BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_CLIENT_H_
