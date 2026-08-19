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

// ensure_xml supervises a Bazel test command and creates a minimal JUnit XML
// result when the test runner does not create one itself.
package main

import (
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"sync"
	"syscall"
	"time"
)

type testSuites struct {
	XMLName xml.Name  `xml:"testsuites"`
	Suite   testSuite `xml:"testsuite"`
}

type testSuite struct {
	Name     string   `xml:"name,attr"`
	Tests    int      `xml:"tests,attr"`
	Failures int      `xml:"failures,attr"`
	Errors   int      `xml:"errors,attr"`
	Time     string   `xml:"time,attr"`
	Case     testCase `xml:"testcase"`
}

type testCase struct {
	Name   string     `xml:"name,attr"`
	Status string     `xml:"status,attr"`
	Time   string     `xml:"time,attr"`
	Error  *testError `xml:"error,omitempty"`
}

type testError struct {
	Message string `xml:"message,attr"`
}

type childResult struct {
	exitCode int
	message  string
}

func main() {
	os.Exit(supervise(os.Args[1:], os.Stderr))
}

func supervise(args []string, stderr io.Writer) int {
	start := time.Now()
	result := childResult{exitCode: 127, message: "test command was not provided"}

	if len(args) > 0 {
		command := exec.Command(args[0], args[1:]...)
		command.Stdin = os.Stdin
		command.Stdout = os.Stdout
		command.Stderr = os.Stderr

		if err := command.Start(); err != nil {
			result.message = fmt.Sprintf("could not start test: %v", err)
		} else {
			result = waitForChild(command)
		}
	}

	if err := ensureXML(os.Getenv("XML_OUTPUT_FILE"), testName(), time.Since(start), result); err != nil {
		fmt.Fprintf(stderr, "ensure_xml: %v\n", err)
	}
	return result.exitCode
}

func waitForChild(command *exec.Cmd) childResult {
	signals := make(chan os.Signal, 4)
	done := make(chan struct{})
	signal.Notify(signals, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(signals)

	var lastSignal os.Signal
	var mu sync.Mutex
	go func() {
		for {
			select {
			case received := <-signals:
				mu.Lock()
				lastSignal = received
				mu.Unlock()
				// The test setup normally signals the whole process group. Forwarding
				// explicitly also handles runners that signal only this wrapper.
				_ = command.Process.Signal(received)
			case <-done:
				return
			}
		}
	}()

	err := command.Wait()
	close(done)
	if err == nil {
		return childResult{exitCode: 0}
	}

	var exitError *exec.ExitError
	if errors.As(err, &exitError) {
		code := exitError.ExitCode()
		if code >= 0 {
			return childResult{exitCode: code, message: fmt.Sprintf("test exited with code %d", code)}
		}
		mu.Lock()
		defer mu.Unlock()
		if signalNumber, ok := lastSignal.(syscall.Signal); ok {
			code = 128 + int(signalNumber)
			return childResult{
				exitCode: code,
				message:  fmt.Sprintf("test terminated by signal %s", lastSignal),
			}
		}
		return childResult{exitCode: 1, message: "test terminated by signal"}
	}
	return childResult{exitCode: 1, message: fmt.Sprintf("could not wait for test: %v", err)}
}

func testName() string {
	if target := os.Getenv("TEST_TARGET"); target != "" {
		return target
	}
	if binary := os.Getenv("TEST_BINARY"); binary != "" {
		return binary
	}
	return "test"
}

func ensureXML(path string, name string, duration time.Duration, result childResult) error {
	if path == "" {
		return errors.New("XML_OUTPUT_FILE is not set")
	}
	if _, err := os.Stat(path); err == nil {
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("could not inspect %s: %w", path, err)
	}

	timeInSeconds := strconv.FormatFloat(duration.Seconds(), 'f', 3, 64)
	suite := testSuite{
		Name:  name,
		Tests: 1,
		Time:  timeInSeconds,
		Case: testCase{
			Name:   name,
			Status: "run",
			Time:   timeInSeconds,
		},
	}
	if result.exitCode != 0 {
		suite.Errors = 1
		suite.Case.Error = &testError{Message: result.message}
	}

	contents, err := xml.MarshalIndent(testSuites{Suite: suite}, "", "  ")
	if err != nil {
		return fmt.Errorf("could not encode test XML: %w", err)
	}
	contents = append([]byte(xml.Header), append(contents, '\n')...)

	directory := filepath.Dir(path)
	temporary, err := os.CreateTemp(directory, ".test.xml.*")
	if err != nil {
		return fmt.Errorf("could not create temporary XML: %w", err)
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)

	if _, err := temporary.Write(contents); err != nil {
		temporary.Close()
		return fmt.Errorf("could not write temporary XML: %w", err)
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("could not close temporary XML: %w", err)
	}

	// The child has exited, but recheck so an XML file it produced is never
	// replaced if it became visible after the first stat.
	if _, err := os.Stat(path); err == nil {
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("could not recheck %s: %w", path, err)
	}
	if err := os.Rename(temporaryPath, path); err != nil {
		return fmt.Errorf("could not publish test XML: %w", err)
	}
	return nil
}
