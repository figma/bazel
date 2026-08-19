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

package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestEnsureXMLForPassingTest(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.xml")
	if err := ensureXML(
		path, "//pkg:test", 1234*time.Millisecond, childResult{exitCode: 0}); err != nil {
		t.Fatal(err)
	}

	contents, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	got := string(contents)
	for _, want := range []string{
		`<testsuite name="//pkg:test" tests="1" failures="0" errors="0" time="1.234">`,
		`<testcase name="//pkg:test" status="run" time="1.234"></testcase>`,
	} {
		if !strings.Contains(got, want) {
			t.Errorf("XML did not contain %q:\n%s", want, got)
		}
	}
}

func TestEnsureXMLForFailingTest(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.xml")
	if err := ensureXML(
		path,
		`//pkg:test&special`,
		2*time.Second,
		childResult{exitCode: 23, message: `test exited with code 23 & failed`},
	); err != nil {
		t.Fatal(err)
	}

	contents, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	got := string(contents)
	for _, want := range []string{
		`name="//pkg:test&amp;special"`,
		`errors="1"`,
		`<error message="test exited with code 23 &amp; failed"></error>`,
	} {
		if !strings.Contains(got, want) {
			t.Errorf("XML did not contain %q:\n%s", want, got)
		}
	}
}

func TestEnsureXMLDoesNotReplaceTestOutput(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.xml")
	const original = "<testsuite name=\"from-test\"/>\n"
	if err := os.WriteFile(path, []byte(original), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := ensureXML(path, "fallback", time.Second, childResult{}); err != nil {
		t.Fatal(err)
	}

	contents, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if got := string(contents); got != original {
		t.Fatalf("existing XML replaced: got %q, want %q", got, original)
	}
}

func TestSuperviseRunsChildAndCreatesXML(t *testing.T) {
	path := filepath.Join(t.TempDir(), "test.xml")
	t.Setenv("XML_OUTPUT_FILE", path)
	t.Setenv("TEST_TARGET", "//pkg:supervised")

	if got := supervise([]string{"/bin/sh", "-c", "exit 7"}, os.Stderr); got != 7 {
		t.Fatalf("supervise exit code = %d, want 7", got)
	}

	contents, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	got := string(contents)
	for _, want := range []string{
		`name="//pkg:supervised"`,
		`errors="1"`,
		`message="test exited with code 7"`,
	} {
		if !strings.Contains(got, want) {
			t.Errorf("XML did not contain %q:\n%s", want, got)
		}
	}
}
