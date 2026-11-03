// Copyright 2016 The Bazel Authors. All rights reserved.
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
package com.google.devtools.build.lib.server;

import static com.google.common.truth.Truth.assertThat;
import static org.junit.Assert.assertThrows;

import com.google.devtools.build.lib.clock.JavaClock;
import com.google.devtools.build.lib.server.CommandProtos.ServerInfo;
import com.google.devtools.build.lib.server.signal.ControlSignalHandler;
import com.google.devtools.build.lib.util.AbruptExitException;
import com.google.devtools.build.lib.util.OS;
import com.google.devtools.build.lib.vfs.DigestHashFunction;
import com.google.devtools.build.lib.vfs.FileSystemUtils;
import com.google.devtools.build.lib.vfs.Path;
import com.google.devtools.build.lib.vfs.inmemoryfs.InMemoryFileSystem;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;
import sun.misc.Signal;

/** The local PID watcher and host connection metadata must survive different host PIDs. */
@RunWith(JUnit4.class)
public final class ServerHostPidTest {
  private final Path serverDirectory =
      new InMemoryFileSystem(DigestHashFunction.SHA256).getPath("/server");

  private PidFileWatcher pidFileWatcher;

  private GrpcServerImpl createServer(boolean criu) throws Exception {
    serverDirectory.createDirectoryAndParents();
    FileSystemUtils.writeContentAsLatin1(serverDirectory.getChild("server.pid.txt"), "3");
    pidFileWatcher = new PidFileWatcher(serverDirectory.getChild("server.pid.txt"), 3);
    return new GrpcServerImpl(
        null,
        ShutdownHooks.createUnregistered(),
        pidFileWatcher,
        new JavaClock(),
        0,
        "request-cookie",
        "response-cookie",
        serverDirectory,
        3,
        criu,
        0,
        false,
        false,
        null);
  }

  private void publishHostPid(String pid) throws Exception {
    FileSystemUtils.writeContentAsLatin1(serverDirectory.getChild("server.host_pid"), pid);
  }

  private ServerInfo readInfo() throws Exception {
    return ServerInfo.parseFrom(
        FileSystemUtils.readContent(serverDirectory.getChild("server_info.rawproto")));
  }

  @Test
  public void firstPublicationUsesHostPidAndLeavesWatcherPidAlone() throws Exception {
    GrpcServerImpl server = createServer(true);
    publishHostPid("12345");
    server.writeServerInfo("127.0.0.1:1234");
    assertThat(readInfo().getPid()).isEqualTo(12345);
    assertThat(pidFileWatcher.runPidFileChecks()).isTrue();
    assertThat(
            new String(
                FileSystemUtils.readContentAsLatin1(serverDirectory.getChild("server.pid.txt"))))
        .isEqualTo("3");
  }

  @Test
  public void signalReconcilesRestoredPidFromMemory() throws Exception {
    org.junit.Assume.assumeTrue(OS.getCurrent() == OS.LINUX);
    GrpcServerImpl server = createServer(true);
    publishHostPid("12345");
    server.writeServerInfo("127.0.0.1:1234");
    ServerInfo before = readInfo();
    // A checkpoint's mutable on-disk proto may have been deleted or replaced.
    FileSystemUtils.writeContentAsLatin1(
        serverDirectory.getChild("server_info.rawproto"), "corrupt");
    publishHostPid("23456");
    CountDownLatch reconciled = new CountDownLatch(1);
    ControlSignalHandler handler =
        new ControlSignalHandler(
            "WINCH",
            () -> {
              server.reconcileHostPid();
              reconciled.countDown();
            });
    try {
      Signal.raise(new Signal("WINCH"));
      assertThat(reconciled.await(10, TimeUnit.SECONDS)).isTrue();
    } finally {
      handler.uninstall();
    }
    assertThat(readInfo()).isEqualTo(before.toBuilder().setPid(23456).build());
    assertThat(pidFileWatcher.runPidFileChecks()).isTrue();
    assertThat(
            new String(
                FileSystemUtils.readContentAsLatin1(serverDirectory.getChild("server.pid.txt"))))
        .isEqualTo("3");
  }

  @Test
  public void invalidRestoredPidDoesNotReplaceWorkingMetadata() throws Exception {
    GrpcServerImpl server = createServer(true);
    publishHostPid("12345");
    server.writeServerInfo("127.0.0.1:1234");
    ServerInfo before = readInfo();
    for (String invalid : new String[] {"0", "-1", "12345\n", "garbage", "99999999999"}) {
      publishHostPid(invalid);
      server.reconcileHostPid();
      assertThat(readInfo()).isEqualTo(before);
    }
  }

  @Test
  public void ordinaryServerIgnoresStaleHostPid() throws Exception {
    GrpcServerImpl server = createServer(false);
    publishHostPid("12345");
    server.writeServerInfo("127.0.0.1:1234");
    server.reconcileHostPid();
    assertThat(readInfo().getPid()).isEqualTo(3);
  }

  @Test
  public void missingHostPidCannotAdvertiseNamespacePidOnInterruptedStartup() throws Exception {
    GrpcServerImpl server = createServer(true);
    Thread.currentThread().interrupt();
    try {
      assertThrows(AbruptExitException.class, () -> server.writeServerInfo("127.0.0.1:1234"));
      assertThat(serverDirectory.getChild("server_info.rawproto").exists()).isFalse();
      assertThat(Thread.currentThread().isInterrupted()).isTrue();
    } finally {
      Thread.interrupted();
    }
  }
}
