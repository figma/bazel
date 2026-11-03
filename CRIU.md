# Checkpointable server startup

On Linux, the `--criu` startup option runs the Bazel server inside fresh user,
PID, and mount namespaces. A persistent PID-1 init launches the server and reaps
its children. The mount namespace has a private `/proc`; networking remains
shared with the host so clients can connect over loopback.

The launcher maps the real user's UID and GID to root inside the user namespace.
It preserves the user's home through `-Duser.home`, disables idle and low-memory
shutdown, and uses `-XX:+PerfDisableSharedMem` to avoid the JVM's temporary
`hsperfdata` mapping. Explicit `user.home` and JVM performance-data settings are
preserved. Filesystem watching is disabled because watcher state does not
survive restore; an explicit `--watchfs` produces a warning.

The launcher publishes the server's host PID in `server/server.host_pid` and
its host start time in `server/server.starttime`. The JVM waits for the host PID
before writing `server_info.rawproto`, so connection metadata always advertises
the host PID. `info server_pid` reads the same host PID. The daemon's
`server/server.pid.txt` contains only the namespace-local PID, as bare decimal
without a trailing newline, for the JVM's PID watcher.

After restore, the launcher must recreate that local PID file before resuming
the JVM, publish the new host PID and start time, and send `SIGWINCH`. The signal
handler survives checkpointing and rewrites connection metadata from the JVM's
in-memory address and cookies. The launcher waits for the advertised host PID to
match before attaching. The restore command comes later.

The launcher also records `server/server.init_host_pid` and
`server/server.init_starttime` before waiting for server startup. Shutdown,
forced cancellation, and takeover kill the entire PID namespace through init,
including when the next client runs without `--criu`. Teardown checks init's
start time and namespace-local PID 1 and uses a pidfd to avoid signalling a
recycled PID. Normal host PID/start-time validation and authenticated gRPC ping
remain in use.

## Host prerequisites

- Linux 5.3 or newer with unprivileged user, PID, and mount namespaces enabled.
- Permission to use `pidfd_open` and `pidfd_send_signal` for safe init teardown.
- A nonzero `user.max_user_namespaces` and, where present,
  `kernel.unprivileged_userns_clone=1` and
  `kernel.apparmor_restrict_unprivileged_userns=0`.
- Permission to mount a new proc filesystem inside the user namespace. Restricted
  containers can deny this even when the namespace sysctls permit startup.
- An executable `criu` on `PATH`, or a path supplied through `BAZEL_CRIU_BINARY`.
  CRIU 3.18 or newer is recommended for the subsequent checkpoint/restore work.

The launcher checks the sysctls and CRIU executable before startup, reports all
missing prerequisites together, and logs observed settings with `--client_debug`.
Namespace setup and private proc mount failures are reported directly.

## Verify startup and reuse

Build the packaged binary with `bazel build //src:bazel-bin`. Use a dedicated
output base and pass `--criu` consistently:

```sh
BAZEL="$PWD/bazel-bin/src/bazel"
OB=/tmp/bazel-criu-start
"$BAZEL" --criu --output_base="$OB" info server_pid
first_host_pid=$(cat "$OB/server/server.host_pid")
"$BAZEL" --criu --output_base="$OB" info server_pid
test "$first_host_pid" = "$(cat "$OB/server/server.host_pid")"
```

Both commands should use the same server. `info server_pid` and
`server/server.host_pid` report its host PID; `server/server.pid.txt` keeps its
namespace-local PID. Wait at least three seconds between invocations to exercise
the JVM's PID watcher. From the host,
check `/proc/<host-pid>/status` (`NSpid`), `/proc/<host-pid>/ns/{user,pid,mnt,net}`,
and the server's parent. The parent should be PID 1 inside the PID namespace;
its network namespace should match the client's. `server/cmdline` should contain
`--max_idle_secs=0`, `--noshutdown_on_low_sys_mem`, `-Duser.home`, and
`-XX:+PerfDisableSharedMem`. No `hsperfdata` file should be mapped.

Use the same flags with `shutdown` to stop the server and its init. `--batch`
cannot be combined with `--criu`. Restore is introduced separately.

The integration test `//src/test/shell/integration:criu_test` checks reuse,
shutdown, forced cancellation, and takeover. It skips the namespaced lifecycle
cases when CRIU or private proc mounts are unavailable. The C++ teardown tests
use real user/PID namespaces without mounting proc, so they can still check init
and child cleanup in containers that prohibit new proc mounts.

## Take a coordinated checkpoint

Start and warm a `--criu` server, then use the same output base:

```sh
"$BAZEL" --criu --output_base="$OB" checkpoint --leave_running
"$BAZEL" --criu --output_base="$OB" info server_pid
"$BAZEL" --criu --output_base="$OB" checkpoint
```

`checkpoint` stops the server and its init after a successful dump by default.
`--leave_running` keeps the warm server available. The boolean forms
`--leave_running=true`, `--leave_running=false`, and `--noleave_running` are also
accepted. Checkpointing requires an existing namespaced server and never starts
one implicitly. The running server must use the same packaged installation as
the launcher. It cannot run in batch mode.

The server-side command waits for active work to finish, takes the exclusive
command lock, runs GC and interner shrinking, and flushes its logs and standard
streams. It then writes `server/checkpoint.sentinel` and parks. A token-bearing
`server/checkpoint.request` identifies this invocation, so a cancelled command
that acquires the lock late cannot leave the server parked.

The init listens on `$OB/bazel-criu.sock` (owner access only). The launcher asks
it to run `criu dump` inside the user, PID, and mount namespaces, using
`--unprivileged`, `--tcp-close`, `--file-locks`, and `--leave-running`. On the
stop path init tears down the namespace after acknowledging success. Keep the
output-base path short enough for Linux's 108-byte Unix socket address limit.

Images and `dump.log` are written under `$OB/criu`, along with the
namespace-local `ns-pid`, a connection-metadata snapshot, and the `install-key`
identifying the packaged binary. The install key is published only after a
successful dump; failed attempts invalidate a previous key. Restore will be
added in the next group.

Readiness is bounded to 120 seconds and the dump to five minutes. Interrupting
the launcher cancels the dump; init terminates and reaps CRIU before replying.
Success, failure, and cancellation all revoke the request and remove readiness
to release the command lock. RPC cleanup cancels the transport and waits for a
bounded interval; a broken stream cannot cause an unbounded thread join.

`//src/test/java/com/google/devtools/build/lib/runtime:CheckpointCommandTest`
exercises the real command lock and late cancellation. Native tests cover dump
flags, control-socket cancellation, and stalled or severed RPCs with a stand-in
dump executable. The integration suite also checks real checkpoint-and-keep and
checkpoint-and-stop when CRIU and private proc mounts are available.
