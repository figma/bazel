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

package com.google.devtools.build.lib.bazel;

import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import com.google.devtools.build.lib.events.Reporter;
import com.google.devtools.build.lib.runtime.BlazeRuntime;
import com.google.devtools.build.lib.runtime.BlazeServerStartupOptions;
import com.google.devtools.build.lib.runtime.CommandEnvironment;
import com.google.devtools.build.lib.runtime.WorkspaceBuilder;
import com.google.devtools.build.lib.skyframe.LocalDiffAwareness;
import com.google.devtools.common.options.OptionsParser;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class BazelDiffAwarenessModuleTest {
  @Test
  public void criuDoesNotRegisterWatchers() throws Exception {
    WorkspaceBuilder builder = mock(WorkspaceBuilder.class);
    new BazelDiffAwarenessModule().workspaceInit(runtime(true), null, builder);
    verify(builder, never()).addDiffAwarenessFactory(any());
  }

  @Test
  public void normalStartupRegistersWatchers() throws Exception {
    WorkspaceBuilder builder = mock(WorkspaceBuilder.class);
    new BazelDiffAwarenessModule().workspaceInit(runtime(false), null, builder);
    verify(builder).addDiffAwarenessFactory(any());
  }

  @Test
  public void explicitWatchfsWarnsInCriuMode() throws Exception {
    CommandEnvironment env = environment(true, "--watchfs");
    new BazelDiffAwarenessModule().beforeCommand(env);
    verify(env.getReporter()).handle(any());
  }

  @Test
  public void nowatchfsDoesNotWarnInCriuMode() throws Exception {
    CommandEnvironment env = environment(true, "--nowatchfs");
    new BazelDiffAwarenessModule().beforeCommand(env);
    verify(env.getReporter(), never()).handle(any());
  }

  @Test
  public void watchfsDoesNotWarnInNormalMode() throws Exception {
    CommandEnvironment env = environment(false, "--watchfs");
    new BazelDiffAwarenessModule().beforeCommand(env);
    verify(env.getReporter(), never()).handle(any());
  }

  private static BlazeRuntime runtime(boolean criu) throws Exception {
    OptionsParser startup =
        OptionsParser.builder().optionsClasses(BlazeServerStartupOptions.class).build();
    if (criu) {
      startup.parse("--criu");
    }
    BlazeRuntime runtime = mock(BlazeRuntime.class);
    when(runtime.getStartupOptionsProvider()).thenReturn(startup);
    return runtime;
  }

  private static CommandEnvironment environment(boolean criu, String watchfs) throws Exception {
    CommandEnvironment env = mock(CommandEnvironment.class);
    BlazeRuntime runtime = runtime(criu);
    when(env.getRuntime()).thenReturn(runtime);
    OptionsParser options =
        OptionsParser.builder().optionsClasses(LocalDiffAwareness.Options.class).build();
    options.parse(watchfs);
    when(env.getOptions()).thenReturn(options);
    Reporter reporter = mock(Reporter.class);
    when(env.getReporter()).thenReturn(reporter);
    return env;
  }
}
