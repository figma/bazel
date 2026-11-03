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
package com.google.devtools.build.lib.runtime;

import static com.google.common.truth.Truth.assertThat;
import static java.util.concurrent.TimeUnit.SECONDS;
import static org.junit.Assert.assertThrows;

import com.google.common.collect.ImmutableList;
import com.google.devtools.build.lib.analysis.BlazeDirectories;
import com.google.devtools.build.lib.analysis.ServerDirectories;
import com.google.devtools.build.lib.runtime.CommandDispatcher.LockingMode;
import com.google.devtools.build.lib.runtime.CommandDispatcher.UiVerbosity;
import com.google.devtools.build.lib.runtime.commands.CheckpointCommand;
import com.google.devtools.build.lib.runtime.proto.InvocationPolicyOuterClass.InvocationPolicy;
import com.google.devtools.build.lib.testutil.Scratch;
import com.google.devtools.build.lib.util.ExitCode;
import com.google.devtools.build.lib.util.io.RecordingOutErr;
import com.google.devtools.build.lib.vfs.FileSystemUtils;
import com.google.devtools.build.lib.vfs.Path;
import com.google.devtools.common.options.OptionsParser;
import com.google.devtools.common.options.OptionsParsingResult;
import java.util.Arrays;
import java.util.Optional;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeoutException;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

/** Real command dispatch must serialize checkpointing with active and subsequent commands. */
@RunWith(JUnit4.class)
public final class CheckpointCommandTest {
  private final Scratch scratch = new Scratch();
  private final ExecutorService executor = Executors.newCachedThreadPool();
  private final CountDownLatch workStarted = new CountDownLatch(1);
  private final CountDownLatch workFinished = new CountDownLatch(1);
  private BlazeCommandDispatcher dispatcher;
  private OptionsParser startup;
  private Path request;
  private Path sentinel;

  @Command(name = "work", shortDescription = "", help = "")
  private final class WorkCommand implements BlazeCommand {
    @Override
    public void editOptions(OptionsParser optionsParser) {}

    @Override
    public BlazeCommandResult exec(CommandEnvironment env, OptionsParsingResult options) {
      workStarted.countDown();
      try {
        workFinished.await();
      } catch (InterruptedException e) {
        Thread.currentThread().interrupt();
      }
      return BlazeCommandResult.success();
    }
  }

  @Before
  public void setUp() throws Exception {
    ServerDirectories directories =
        new ServerDirectories(
            scratch.dir("install_base"), scratch.dir("output_base"), scratch.dir("user_root"));
    startup = OptionsParser.builder().optionsClasses(BlazeServerStartupOptions.class).build();
    startup.parse("--criu");
    BlazeRuntime runtime =
        new BlazeRuntime.Builder()
            .setFileSystem(scratch.getFileSystem())
            .setServerDirectories(directories)
            .setProductName("bazel")
            .setStartupOptionsProvider(startup)
            .build();
    runtime.initWorkspace(
        new BlazeDirectories(directories, scratch.dir("workspace"), null, "bazel"), null);
    runtime.overrideCommands(ImmutableList.of(new CheckpointCommand(), new WorkCommand()));
    runtime.getServerDirectory().createDirectoryAndParents();
    request = runtime.getServerDirectory().getChild("checkpoint.request");
    sentinel = runtime.getServerDirectory().getChild("checkpoint.sentinel");
    dispatcher = new BlazeCommandDispatcher(runtime);
  }

  @After
  public void tearDown() throws Exception {
    workFinished.countDown();
    request.delete();
    if (sentinel.exists() && !sentinel.isDirectory()) sentinel.delete();
    executor.shutdownNow();
    assertThat(executor.awaitTermination(10, SECONDS)).isTrue();
  }

  private BlazeCommandResult run(String... args) throws Exception {
    return dispatcher.exec(
        InvocationPolicy.getDefaultInstance(),
        Arrays.asList(args),
        new RecordingOutErr(),
        LockingMode.WAIT,
        UiVerbosity.NORMAL,
        "checkpoint test",
        System.currentTimeMillis(),
        Optional.empty(),
        () -> ImmutableList.of(),
        ImmutableList.of(),
        ext -> {});
  }

  private Future<BlazeCommandResult> checkpoint() throws Exception {
    FileSystemUtils.writeContentAsLatin1(request, "request-token");
    return executor.submit(() -> run("checkpoint", "--checkpoint_token=request-token"));
  }

  private void awaitReadiness() throws Exception {
    long deadline = System.nanoTime() + SECONDS.toNanos(10);
    while (!sentinel.exists() && System.nanoTime() < deadline) Thread.sleep(10);
    assertThat(sentinel.exists()).isTrue();
    assertThat(new String(FileSystemUtils.readContentAsLatin1(sentinel)))
        .isEqualTo("request-token");
  }

  @Test
  public void waitsForActiveCommandAndHoldsExclusiveLockUntilReleased() throws Exception {
    Future<BlazeCommandResult> active = executor.submit(() -> run("work"));
    assertThat(workStarted.await(10, SECONDS)).isTrue();
    Future<BlazeCommandResult> checkpoint = checkpoint();
    assertThrows(
        TimeoutException.class,
        () -> checkpoint.get(100, java.util.concurrent.TimeUnit.MILLISECONDS));
    assertThat(sentinel.exists()).isFalse();
    workFinished.countDown();
    assertThat(active.get(10, SECONDS).getExitCode()).isEqualTo(ExitCode.SUCCESS);
    awaitReadiness();
    Future<BlazeCommandResult> next = executor.submit(() -> run("work"));
    assertThrows(
        TimeoutException.class, () -> next.get(100, java.util.concurrent.TimeUnit.MILLISECONDS));
    sentinel.delete();
    assertThat(checkpoint.get(10, SECONDS).getExitCode()).isEqualTo(ExitCode.SUCCESS);
    assertThat(next.get(10, SECONDS).getExitCode()).isEqualTo(ExitCode.SUCCESS);
  }

  @Test
  public void failedOrCancelledDumpReleasesParkedCommandByRevokingLease() throws Exception {
    Future<BlazeCommandResult> checkpoint = checkpoint();
    awaitReadiness();
    request.delete();
    assertThat(checkpoint.get(10, SECONDS).getExitCode()).isEqualTo(ExitCode.SUCCESS);
    assertThat(sentinel.exists()).isFalse();
  }

  @Test
  public void cancelledQueuedRequestCannotParkAfterAcquiringLock() throws Exception {
    Future<BlazeCommandResult> active = executor.submit(() -> run("work"));
    assertThat(workStarted.await(10, SECONDS)).isTrue();
    Future<BlazeCommandResult> checkpoint = checkpoint();
    request.delete();
    workFinished.countDown();
    active.get(10, SECONDS);
    assertThat(checkpoint.get(10, SECONDS).getExitCode()).isEqualTo(ExitCode.INTERRUPTED);
    assertThat(sentinel.exists()).isFalse();
  }

  @Test
  public void sentinelIoFailureReturnsWithoutParking() throws Exception {
    sentinel.createDirectory();
    Future<BlazeCommandResult> checkpoint = checkpoint();
    assertThat(checkpoint.get(10, SECONDS).getExitCode())
        .isEqualTo(ExitCode.LOCAL_ENVIRONMENTAL_ERROR);
  }

  @Test
  public void refusesOrdinaryServer() throws Exception {
    startup.parse("--nocriu");
    assertThat(run("checkpoint").getExitCode()).isEqualTo(ExitCode.COMMAND_LINE_ERROR);
    assertThat(sentinel.exists()).isFalse();
  }
}
