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

// Linux namespace support for a server that rootless CRIU can checkpoint.
// The server shares host networking and runs under a persistent PID-1 reaper
// in fresh user, PID, and mount namespaces with a private /proc.

#ifndef BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_H_
#define BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_H_

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "src/main/cpp/blaze_util.h"
#include "src/main/cpp/blaze_util_platform.h"
#include "src/main/cpp/util/path.h"

namespace blaze {

// Latch the parsed startup flag before any platform code queries the mode.
void SetCriuModeActive(bool active);

// True only when --criu was requested on Linux.
bool CriuModeActive();

// Check namespace sysctls, pidfd support, and an executable criu binary. On failure,
// report the observed settings and how to enable the missing prerequisites.
// No-op when the mode is inactive or the platform is not Linux.
bool CriuPreflight(std::string *error);

// Starts the bazel server inside a fresh user+PID+mount namespace, mirroring
// the contract of ExecuteDaemon: it spawns `exe args_vector` via the daemonize
// helper, but does so as the child of a persistent PID-1 init that outlives the
// launcher and holds the namespace (and the warm server) open for a later
// checkpoint.
//
// Unlike the plain ExecuteDaemon, this blocks until the server is up and its
// server_info.rawproto advertises the server's *host* pid (so ordinary host
// clients can attach over loopback), and returns that host
// pid. *server_startup is set to an always-alive stub: liveness is established
// by the subsequent gRPC connect rather than by an inherited socket.
//
// Crashes (BAZEL_DIE) on unrecoverable failure.
int ExecuteDaemonInNamespace(
    const blaze_util::Path &exe, const std::vector<std::string> &args_vector,
    const std::map<std::string, EnvVarValue> &env,
    const blaze_util::Path &daemon_output, bool daemon_output_append,
    const blaze_util::Path &binaries_dir, const blaze_util::Path &server_dir,
    BlazeServerStartup **server_startup);

// Coordination files and the init's host-reachable Unix control socket.
blaze_util::Path CriuCheckpointSentinelPath(
    const blaze_util::Path& output_base);
blaze_util::Path ControlSocketPath(const blaze_util::Path& output_base);

// Dump a quiescent server from inside its namespaces. A successful stop request
// also tears down init and the entire server tree. Cancellation aborts the
// dump.
int CriuCheckpoint(const blaze_util::Path& output_base,
                   const std::string& install_key, bool stop,
                   const std::function<bool()>& cancelled);

// Check immutable checkpoint metadata before changing any live server files.
// JVM arguments are compared in order, including repeated flags. Missing
// metadata (including checkpoints predating startup-argument recording) rejects
// reuse. Client-only startup options may differ.
bool CriuCheckpointExists(const blaze_util::Path& output_base,
                          const std::string& install_key,
                          const std::vector<std::string>& server_args);

// Restore a previously validated checkpoint under a fresh init. Recreate live
// files before resuming the JVM, reconcile its host identity, and leave the
// namespace ready for Connect(). On failure, report the cause and tear down
// the new namespace so the caller can cold-start safely.
bool CriuRestore(const blaze_util::Path& output_base);

// Kill the recorded PID-1 init and wait for the entire PID namespace to exit.
// Validate its start time and namespace-local PID before signalling through a
// pidfd. Safe regardless of the current --criu flag. Returns true if an init
// record existed (including stale records); false for an ordinary server.
bool KillNamespacedServer(const blaze_util::Path &server_dir);

}  // namespace blaze

#endif  // BAZEL_SRC_MAIN_CPP_BLAZE_CRIU_H_
