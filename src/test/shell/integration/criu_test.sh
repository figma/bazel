#!/usr/bin/env bash
#
# Copyright 2026 The Bazel Authors. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Tests for Linux checkpointable server startup.

# --- begin runfiles.bash initialization ---
set -euo pipefail
if [[ ! -d "${RUNFILES_DIR:-/dev/null}" && ! -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  if [[ -f "$0.runfiles_manifest" ]]; then
    export RUNFILES_MANIFEST_FILE="$0.runfiles_manifest"
  elif [[ -f "$0.runfiles/MANIFEST" ]]; then
    export RUNFILES_MANIFEST_FILE="$0.runfiles/MANIFEST"
  elif [[ -f "$0.runfiles/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
    export RUNFILES_DIR="$0.runfiles"
  fi
fi
if [[ -f "${RUNFILES_DIR:-/dev/null}/bazel_tools/tools/bash/runfiles/runfiles.bash" ]]; then
  source "${RUNFILES_DIR}/bazel_tools/tools/bash/runfiles/runfiles.bash"
elif [[ -f "${RUNFILES_MANIFEST_FILE:-/dev/null}" ]]; then
  source "$(grep -m1 "^bazel_tools/tools/bash/runfiles/runfiles.bash " \
            "$RUNFILES_MANIFEST_FILE" | cut -d ' ' -f 2-)"
else
  echo >&2 "ERROR: cannot find @bazel_tools//tools/bash/runfiles:runfiles.bash"
  exit 1
fi
# --- end runfiles.bash initialization ---

source "$(rlocation "io_bazel/src/test/shell/integration_test_setup.sh")" \
  || { echo "integration_test_setup.sh not found!" >&2; exit 1; }

function test_criu_reports_missing_binary() {
  BAZEL_CRIU_BINARY="${TEST_TMPDIR}/nonexistent-criu" \
      bazel --criu info server_pid >"${TEST_log}" 2>&1 && fail "Expected preflight failure"
  expect_log "missing or not executable"
  expect_log "Observed state:"
}

function test_criu_rejects_batch() {
  bazel --criu --batch info server_pid >"${TEST_log}" 2>&1 && fail "Expected batch rejection"
  expect_log "cannot be used with --batch"
}

function test_criu_reuses_namespaced_server() {
  if ! command -v "${BAZEL_CRIU_BINARY:-criu}" >/dev/null 2>&1; then
    echo "Skipping namespaced attachment: CRIU executable is unavailable"
    return 0
  fi
  if ! unshare --user --map-root-user --pid --fork --mount --mount-proc \
      /bin/true >/dev/null 2>&1; then
    echo "Skipping namespaced attachment: user namespaces or private /proc unavailable"
    return 0
  fi

  local ob="${TEST_TMPDIR}/criu-reuse"
  local first_pid first_starttime second_pid init_pid
  bazel --criu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Namespaced startup failed"
  first_pid=$(cat "${ob}/server/server.pid.txt")
  first_starttime=$(cat "${ob}/server/server.starttime")
  bazel --criu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Could not attach to namespaced server"
  second_pid=$(cat "${ob}/server/server.pid.txt")
  [[ "${first_pid}" == "${second_pid}" ]] || fail "Client started a different server"
  [[ "${first_starttime}" == "$(cat "${ob}/server/server.starttime")" ]] \
      || fail "Server start time changed"
  [[ "$(readlink "/proc/${first_pid}/ns/pid")" != "$(readlink "/proc/$$/ns/pid")" ]] \
      || fail "Server is in the client's PID namespace"
  [[ "$(readlink "/proc/${first_pid}/ns/mnt")" != "$(readlink "/proc/$$/ns/mnt")" ]] \
      || fail "Server is in the client's mount namespace"
  [[ "$(readlink "/proc/${first_pid}/ns/user")" != "$(readlink "/proc/$$/ns/user")" ]] \
      || fail "Server is in the client's user namespace"
  [[ "$(readlink "/proc/${first_pid}/ns/net")" == "$(readlink "/proc/$$/ns/net")" ]] \
      || fail "Server does not share host networking"
  init_pid=$(awk '/^PPid:/ {print $2}' "/proc/${first_pid}/status")
  [[ "$(awk '/^NSpid:/ {print $NF}' "/proc/${init_pid}/status")" == 1 ]] \
      || fail "Server parent is not the namespace's PID-1 init"
  if grep -q hsperfdata "/proc/${first_pid}/maps"; then
    fail "Server maps a temporary hsperfdata file"
  fi
  bazel --criu --output_base="${ob}" shutdown >"${TEST_log}" 2>&1 \
      || fail "Namespaced shutdown failed"
}

function tear_down() {
  local ob="${TEST_TMPDIR}/criu-reuse"
  if [[ -f "${ob}/server/server.pid.txt" ]]; then
    bazel --criu --output_base="${ob}" shutdown >/dev/null 2>&1 || true
  fi
}

run_suite "Checkpointable server startup"
