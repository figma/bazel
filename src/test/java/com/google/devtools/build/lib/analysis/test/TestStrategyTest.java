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
package com.google.devtools.build.lib.analysis.test;

import static com.google.common.truth.Truth.assertThat;

import com.google.common.collect.ImmutableList;
import com.google.devtools.build.lib.analysis.config.PerLabelOptions;
import com.google.devtools.build.lib.cmdline.Label;
import com.google.devtools.build.lib.exec.ExecutionOptions;
import com.google.devtools.build.lib.util.RegexFilter;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

@RunWith(JUnit4.class)
public final class TestStrategyTest {

  private static final Label LABEL = Label.parseCanonicalUnchecked("//pkg:test");

  @Test
  public void numericOverrideAppliesToDeterministicTargets() {
    ExecutionOptions options = optionsWithCatchAllAttempts("3");
    assertThat(TestStrategy.computeTestAttempts(0, options, LABEL)).isEqualTo(3);
  }

  @Test
  public void numericOverrideIgnoresFlakyAttribute() {
    ExecutionOptions options = optionsWithCatchAllAttempts("3");
    assertThat(TestStrategy.computeTestAttempts(1, options, LABEL)).isEqualTo(3);
    assertThat(TestStrategy.computeTestAttempts(2, options, LABEL)).isEqualTo(3);
  }

  @Test
  public void defaultFlagUsesBaselinePlusFlakyAttribute() {
    ExecutionOptions options = optionsWithCatchAllAttempts("default");
    assertThat(TestStrategy.computeTestAttempts(0, options, LABEL)).isEqualTo(1);
    assertThat(TestStrategy.computeTestAttempts(1, options, LABEL)).isEqualTo(2);
    assertThat(TestStrategy.computeTestAttempts(2, options, LABEL)).isEqualTo(3);
  }

  @Test
  public void unsetFlagBehavesLikeDefault() {
    ExecutionOptions options = new ExecutionOptions();
    assertThat(TestStrategy.computeTestAttempts(0, options, LABEL)).isEqualTo(1);
    assertThat(TestStrategy.computeTestAttempts(1, options, LABEL)).isEqualTo(2);
  }

  @Test
  public void attemptsAreCappedAtTen() {
    ExecutionOptions options = optionsWithCatchAllAttempts("12");
    assertThat(TestStrategy.computeTestAttempts(0, options, LABEL)).isEqualTo(10);
  }

  private static ExecutionOptions optionsWithCatchAllAttempts(String attempts) {
    ExecutionOptions options = new ExecutionOptions();
    options.testAttempts =
        ImmutableList.of(
            new PerLabelOptions(
                new RegexFilter(ImmutableList.of(".*"), ImmutableList.of()),
                ImmutableList.of(attempts)));
    return options;
  }
}
