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
"""Measure image size and complete restore through the first info command.

Uses an existing checkpoint and shuts down its output-base server between runs.
Refuses to report a cold fallback or an attachment to an already running server.
"""

import argparse
import datetime
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import time


def measure(bazel, workspace, output_base, startup_args, runs):
  """Restore the same checkpoint repeatedly, including graceful shutdown."""
  images = output_base / 'criu'
  for name in ('inventory.img', 'install-key', 'startup-args', 'ns-pid'):
    if not (images / name).is_file():
      raise ValueError(f'Missing checkpoint file: {images / name}')
  image_stats = [p.stat() for p in images.glob('*.img*') if p.is_file()]
  command = [str(bazel), *startup_args, '--criu', f'--output_base={output_base}']

  def invoke(args):
    result = subprocess.run(
        command + args, cwd=workspace, capture_output=True, text=True, timeout=600
    )
    if result.returncode:
      raise RuntimeError(
          f'Bazel {" ".join(args)} exited {result.returncode}:\n{result.stderr}'
      )
    return result

  samples = []
  # A warm attachment must never be counted as a restore. Shutdown is outside
  # the measured interval, including the initial shutdown of an existing server.
  invoke(['shutdown'])
  for _ in range(runs):
    try:
      start = time.perf_counter_ns()
      result = invoke(['info', 'server_pid'])
      elapsed_ms = (time.perf_counter_ns() - start) / 1_000_000
      log = result.stderr
      if ('criu: restored server at host PID ' not in log or
          'Starting local ' in log or 'falling back to cold startup' in log):
        raise RuntimeError(f'Command did not use a restored server:\n{log}')
      native_time = re.search(r'criu: restore took ([0-9]+)ms', log)
      samples.append({
          'server_pid': int(result.stdout.strip()),
          'criu_restore_ms': int(native_time[1]) if native_time else None,
          'restore_to_first_command_ms': round(elapsed_ms, 3),
      })
    finally:
      invoke(['shutdown'])

  latencies = [s['restore_to_first_command_ms'] for s in samples]
  return {
      'output_base': str(output_base),
      'checkpoint_created_utc': datetime.datetime.fromtimestamp(
          (images / 'ns-pid').stat().st_mtime, datetime.timezone.utc
      ).isoformat(),
      'image_file_bytes': sum(s.st_size for s in image_stats),
      'image_allocated_bytes': sum(s.st_blocks * 512 for s in image_stats),
      'first_command': ['info', 'server_pid'],
      'samples': samples,
      'restore_to_first_command_ms': {
          'min': min(latencies),
          'median': statistics.median(latencies),
          'max': max(latencies),
      },
  }


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('--bazel', required=True, type=Path)
  parser.add_argument('--workspace', type=Path, default=Path.cwd())
  parser.add_argument('--output-base', required=True, type=Path)
  parser.add_argument('--runs', type=int, default=5)
  parser.add_argument(
      '--startup-arg', action='append', default=[],
      help='Repeat for checkpoint-time settings, e.g. --startup-arg=--host_jvm_args=-Xmx2g',
  )
  args = parser.parse_args()
  if sys.platform != 'linux':
    parser.error('CRIU benchmarks require Linux')
  if args.runs < 1:
    parser.error('--runs must be positive')
  try:
    result = measure(
        args.bazel.resolve(), args.workspace.resolve(), args.output_base.resolve(),
        args.startup_arg, args.runs,
    )
  except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
    parser.exit(1, f'{error}\n')
  print(json.dumps(result, indent=2))


if __name__ == '__main__':
  main()
