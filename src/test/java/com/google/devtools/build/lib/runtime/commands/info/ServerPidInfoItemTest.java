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
package com.google.devtools.build.lib.runtime.commands.info;

import static com.google.common.truth.Truth.assertThat;
import static java.nio.charset.StandardCharsets.UTF_8;
import static org.junit.Assert.assertThrows;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;

import com.google.devtools.build.lib.runtime.BlazeRuntime;
import com.google.devtools.build.lib.runtime.BlazeServerStartupOptions;
import com.google.devtools.build.lib.runtime.CommandEnvironment;
import com.google.devtools.build.lib.vfs.DigestHashFunction;
import com.google.devtools.build.lib.vfs.FileSystemUtils;
import com.google.devtools.build.lib.vfs.Path;
import com.google.devtools.build.lib.vfs.inmemoryfs.InMemoryFileSystem;
import com.google.devtools.common.options.OptionsParser;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

/** Host PID reporting must follow restore while ignoring stale files in ordinary servers. */
@RunWith(JUnit4.class)
public final class ServerPidInfoItemTest {
  private final Path serverDirectory =
      new InMemoryFileSystem(DigestHashFunction.SHA256).getPath("/server");

  private CommandEnvironment environment(boolean criu) throws Exception {
    serverDirectory.createDirectoryAndParents();
    OptionsParser startup =
        OptionsParser.builder().optionsClasses(BlazeServerStartupOptions.class).build();
    if (criu) {
      startup.parse("--criu");
    }
    BlazeRuntime runtime = mock(BlazeRuntime.class);
    when(runtime.getStartupOptionsProvider()).thenReturn(startup);
    when(runtime.getServerDirectory()).thenReturn(serverDirectory);
    CommandEnvironment env = mock(CommandEnvironment.class);
    when(env.getRuntime()).thenReturn(runtime);
    return env;
  }

  private String pid(CommandEnvironment env) {
    return new String(new ServerPidInfoItem("bazel").get(null, env), UTF_8).trim();
  }

  @Test
  public void criuReportsHostPidIncludingAfterRestore() throws Exception {
    CommandEnvironment env = environment(true);
    FileSystemUtils.writeContentAsLatin1(serverDirectory.getChild("server.host_pid"), "12345");
    assertThat(pid(env)).isEqualTo("12345");
    FileSystemUtils.writeContentAsLatin1(serverDirectory.getChild("server.host_pid"), "23456");
    assertThat(pid(env)).isEqualTo("23456");
  }

  @Test
  public void ordinaryServerIgnoresStaleHostPid() throws Exception {
    CommandEnvironment env = environment(false);
    FileSystemUtils.writeContentAsLatin1(serverDirectory.getChild("server.host_pid"), "12345");
    assertThat(pid(env)).isEqualTo(Long.toString(ProcessHandle.current().pid()));
  }

  @Test
  public void criuCannotReportLocalPidWhenHostIdentityIsMissingOrInvalid() throws Exception {
    CommandEnvironment env = environment(true);
    assertThrows(IllegalStateException.class, () -> pid(env));
    for (String invalid : new String[] {"0", "-1", "garbage", "12345\n"}) {
      FileSystemUtils.writeContentAsLatin1(serverDirectory.getChild("server.host_pid"), invalid);
      assertThrows(IllegalStateException.class, () -> pid(env));
    }
  }
}
