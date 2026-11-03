// Copyright 2020 The Bazel Authors. All rights reserved.
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

import com.google.common.base.Supplier;
import com.google.devtools.build.lib.analysis.config.BuildConfigurationValue;
import com.google.devtools.build.lib.runtime.BlazeServerStartupOptions;
import com.google.devtools.build.lib.runtime.CommandEnvironment;
import com.google.devtools.build.lib.runtime.InfoItem;
import com.google.devtools.build.lib.vfs.FileSystemUtils;
import java.io.IOException;

/** Info item for server_pid. */
public final class ServerPidInfoItem extends InfoItem {
  public ServerPidInfoItem(String productName) {
    super("server_pid", productName + " process id", false);
  }

  @Override
  public byte[] get(
      Supplier<BuildConfigurationValue> configurationSupplier, CommandEnvironment env) {
    if (env.getRuntime()
        .getStartupOptionsProvider()
        .getOptions(BlazeServerStartupOptions.class)
        .criu) {
      try {
        int hostPid =
            Integer.parseInt(
                new String(
                    FileSystemUtils.readContentAsLatin1(
                        env.getRuntime().getServerDirectory().getChild("server.host_pid"))));
        if (hostPid > 0) {
          return print(hostPid);
        }
      } catch (IOException | NumberFormatException e) {
        throw new IllegalStateException("Cannot read CRIU server host PID", e);
      }
      throw new IllegalStateException("CRIU server host PID must be positive");
    }
    return print(ProcessHandle.current().pid());
  }
}
