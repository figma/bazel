# Test XML fallback wrapper

`ensure_xml` supervises a test command. If that command exits without creating
`$XML_OUTPUT_FILE`, the wrapper writes a minimal JUnit XML result and preserves
the command's exit status. It never replaces XML produced by the test.

The root cause remains a test runner that failed to honor Bazel's declared XML
output contract. This wrapper is an opt-in compatibility workaround for such
tests. Workaround mode fails if the wrapper cannot publish XML; it does not hide
that failure behind Bazel's derived XML action.

The Linux executables are checked-in embedded tools because they must be action
inputs before an execution platform has a Go toolchain, and remote executors
should not need a JRE or libc. Regenerate them from source with:

```sh
bazel build //tools/test/ensure_xml:ensure_xml_linux_amd64_build
cp bazel-bin/tools/test/ensure_xml/ensure_xml_linux_amd64_build \
  tools/test/ensure_xml/ensure_xml_linux_amd64
bazel build //tools/test/ensure_xml:ensure_xml_linux_arm64_build
cp bazel-bin/tools/test/ensure_xml/ensure_xml_linux_arm64_build \
  tools/test/ensure_xml/ensure_xml_linux_arm64
bazel build //tools/test/ensure_xml:ensure_xml_darwin_arm64_build
cp bazel-bin/tools/test/ensure_xml/ensure_xml_darwin_arm64_build \
  tools/test/ensure_xml/ensure_xml_darwin_arm64
```

Other Unix platforms and Windows omit the supervisor. On those platforms,
workaround mode succeeds only when the test itself writes XML. Otherwise Bazel
reports a missing-output error.
