# Test XML fallback wrapper

`ensure_xml` supervises a test command. If that command exits without creating
`$XML_OUTPUT_FILE`, the wrapper writes a minimal JUnit XML result and preserves
the command's exit status. It never replaces XML produced by the test.

The root cause remains a test runner that failed to honor Bazel's declared XML
output contract. This wrapper is an opt-in compatibility workaround for such
tests. Bazel's derived XML action remains as a last resort for cases where the
wrapper itself cannot run or publish XML.

The Linux executables are checked-in embedded tools because they must be action
inputs before an execution platform has a Go toolchain, and remote executors
should not need a JRE or libc. Regenerate them from source with:

```sh
bazel build //tools/test/ensure_xml:ensure_xml_linux_amd64
cp bazel-bin/tools/test/ensure_xml/ensure_xml_linux_amd64 \
  tools/test/ensure_xml/ensure_xml_linux_amd64
bazel build //tools/test/ensure_xml:ensure_xml_linux_arm64
cp bazel-bin/tools/test/ensure_xml/ensure_xml_linux_arm64 \
  tools/test/ensure_xml/ensure_xml_linux_arm64
```

Other Unix platforms use `pass_through.sh`, and Windows omits the supervisor.
Their missing XML is handled by the derived fallback action with the
`TestXmlGenerationWorkaround` mnemonic.
