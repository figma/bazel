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

function require_criu_namespaces() {
  if ! command -v "${BAZEL_CRIU_BINARY:-criu}" >/dev/null 2>&1; then
    echo "Skipping namespaced lifecycle: CRIU executable is unavailable"
    return 1
  fi
  if ! unshare --user --map-root-user --pid --fork --mount --mount-proc \
      /bin/true >/dev/null 2>&1; then
    echo "Skipping namespaced lifecycle: user namespaces or private /proc unavailable"
    return 1
  fi
}

function assert_process_gone() {
  local pid="$1"
  local attempt state
  for attempt in $(seq 1 100); do
    if [[ ! -f "/proc/${pid}/status" ]]; then
      return 0
    fi
    state=$(awk '/^State:/ {print $2}' "/proc/${pid}/status" 2>/dev/null || true)
    [[ "${state}" == Z || "${state}" == X || -z "${state}" ]] && return 0
    sleep 0.1
  done
  fail "Process ${pid} survived namespace teardown"
}

function test_criu_reuses_namespaced_server() {
  require_criu_namespaces || return 0

  local ob="${TEST_TMPDIR}/criu-reuse"
  local first_pid first_starttime second_pid init_pid
  bazel --criu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Namespaced startup failed"
  first_pid=$(cat "${ob}/server/server.host_pid")
  expect_log "^${first_pid}$"
  [[ "$(cat "${ob}/server/server.pid.txt")" == \
      "$(awk '/^NSpid:/ {print $NF}' "/proc/${first_pid}/status")" ]] \
      || fail "PID watcher file does not contain the namespace-local PID"
  sleep 4  # Run through at least one JVM PID-watcher check.
  first_starttime=$(cat "${ob}/server/server.starttime")
  bazel --criu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Could not attach to namespaced server"
  second_pid=$(cat "${ob}/server/server.host_pid")
  expect_log "^${second_pid}$"
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
  [[ "${init_pid}" == "$(cat "${ob}/server/server.init_host_pid")" ]] \
      || fail "Recorded init host PID does not match the server parent"
  [[ "$(awk '/^NSpid:/ {print $NF}' "/proc/${init_pid}/status")" == 1 ]] \
      || fail "Server parent is not the namespace's PID-1 init"
  if grep -q hsperfdata "/proc/${first_pid}/maps"; then
    fail "Server maps a temporary hsperfdata file"
  fi
  bazel --criu --output_base="${ob}" shutdown >"${TEST_log}" 2>&1 \
      || fail "Namespaced shutdown failed"
  assert_process_gone "${first_pid}"
  assert_process_gone "${init_pid}"
}

function test_criu_takeover_reaps_entire_namespace() {
  require_criu_namespaces || return 0
  local ob="${TEST_TMPDIR}/criu-takeover"
  bazel --criu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Namespaced startup failed"
  local old_server old_init
  old_server=$(cat "${ob}/server/server.host_pid")
  old_init=$(cat "${ob}/server/server.init_host_pid")
  printf corrupt >"${ob}/server/server_info.rawproto"
  bazel --criu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Namespaced takeover failed"
  assert_process_gone "${old_server}"
  assert_process_gone "${old_init}"
  [[ "${old_server}" != "$(cat "${ob}/server/server.host_pid")" ]] \
      || fail "Takeover did not replace the server"
}

function test_plain_server_takes_over_namespaced_server() {
  require_criu_namespaces || return 0
  local ob="${TEST_TMPDIR}/criu-mode-switch"
  bazel --criu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Namespaced startup failed"
  local old_server old_init
  old_server=$(cat "${ob}/server/server.host_pid")
  old_init=$(cat "${ob}/server/server.init_host_pid")
  bazel --nocriu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Plain server takeover failed"
  assert_process_gone "${old_server}"
  assert_process_gone "${old_init}"
  [[ ! -e "${ob}/server/server.init_host_pid" ]] || fail "Stale init record"
}

function test_criu_third_interrupt_reaps_entire_namespace() {
  require_criu_namespaces || return 0
  local ob="${TEST_TMPDIR}/criu-cancel"
  local marker="${TEST_TMPDIR}/criu-action-started"
  mkdir -p criu_cancel
  rm -f "${marker}"
  cat >criu_cancel/BUILD <<EOF
genrule(
    name = "wait",
    outs = ["wait.out"],
    local = True,
    cmd = "touch ${marker}; sleep 600; touch \$@",
)
EOF
  bazel --criu --output_base="${ob}" info server_pid >"${TEST_log}" 2>&1 \
      || fail "Namespaced startup failed"
  local server_pid init_pid client_pid attempt
  server_pid=$(cat "${ob}/server/server.host_pid")
  init_pid=$(cat "${ob}/server/server.init_host_pid")
  set -m
  bazel --criu --output_base="${ob}" build //criu_cancel:wait >"${TEST_log}" 2>&1 &
  client_pid=$!
  for attempt in $(seq 1 300); do
    [[ -e "${marker}" ]] && break
    sleep 0.1
  done
  [[ -e "${marker}" ]] || fail "Build action did not start"
  # Freeze the JVM so cancellation cannot complete before the third interrupt.
  kill -STOP "${server_pid}"
  for attempt in 1 2 3; do
    kill -INT "${client_pid}"
    sleep 1
  done
  wait "${client_pid}" && fail "Expected interrupted client to fail"
  set +m
  expect_log "caught third interrupt signal; server killed"
  assert_process_gone "${server_pid}"
  assert_process_gone "${init_pid}"
}

function tear_down() {
  local ob init_pid
  for ob in "${TEST_TMPDIR}"/criu-{reuse,takeover,mode-switch,cancel}; do
    if [[ -f "${ob}/server/server.init_host_pid" ]]; then
      init_pid=$(cat "${ob}/server/server.init_host_pid")
      if [[ -f "/proc/${init_pid}/status" ]] && \
          [[ "$(awk '/^NSpid:/ {print $NF}' "/proc/${init_pid}/status")" == 1 ]]; then
        kill -KILL "${init_pid}" 2>/dev/null || true
      fi
    elif [[ -f "${ob}/server/server_info.rawproto" ]]; then
      bazel --output_base="${ob}" shutdown >/dev/null 2>&1 || true
    fi
  done
  rm -rf criu_cancel
}

run_suite "Checkpointable server identity and lifecycle"
