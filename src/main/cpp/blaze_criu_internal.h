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

#ifndef BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_INTERNAL_H_
#define BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_INTERNAL_H_

namespace blaze {
namespace criu_internal {

// Prepared before forking; all storage outlives the init's copied stack.
struct InitControl {
  const char* sock_path;
  const char* criu_bin;
  const char* images_dir;
  const char* ns_pid_file;
  const char* server_pid_file;
  // Live $output_base/server/server_info.rawproto, snapshotted at dump time.
  const char* server_info_file;
  // Where the snapshot is written (inside images_dir), so restore can recover
  // it after a graceful shutdown deletes the live file.
  const char* server_info_snapshot;
  // $output_base/server/checkpoint.sentinel. On restore the init unlinks this
  // from inside the namespace before resuming the server, releasing the parked
  // `checkpoint` command (see RunInit and CheckpointCommand.java).
  const char* sentinel_file;
  const char* request_file;
};

// Serve one connected Unix control socket from init. Returns whether a
// successful dump requested teardown. Uses only syscalls and stack storage.
bool HandleCheckpointRequest(const InitControl& paths, int connection);

}  // namespace criu_internal
}  // namespace blaze
#endif  // BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_INTERNAL_H_
