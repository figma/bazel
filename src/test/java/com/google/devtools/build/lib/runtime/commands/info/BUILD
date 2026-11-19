load("@rules_java//java:java_test.bzl", "java_test")

package(
    default_applicable_licenses = ["//:license"],
    default_testonly = 1,
    default_visibility = ["//src/test/java/com/google/devtools/build/lib:test_lib"],
)

filegroup(
    name = "srcs",
    testonly = 0,
    srcs = glob(["**"]),
    visibility = ["//src:__subpackages__"],
)

java_test(
    name = "StdoutInfoItemHandlerTest",
    size = "small",
    srcs = ["StdoutInfoItemHandlerTest.java"],
    deps = [
        "//src/main/java/com/google/devtools/build/lib/runtime:blaze_command_cluster",
        "//src/main/java/com/google/devtools/build/lib/runtime/commands/info",
        "//src/main/java/com/google/devtools/build/lib/util/io:out-err",
        "//third_party/java/junit",
        "//third_party/java/mockito",
        "//third_party/java/truth",
    ],
)

java_test(
    name = "JavaHomeInfoItemTest",
    size = "small",
    srcs = ["JavaHomeInfoItemTest.java"],
    deps = [
        "//src/main/java/com/google/devtools/build/lib/runtime/commands/info",
        "//third_party/java/junit",
        "//third_party/java/truth",
    ],
)

java_test(
    name = "RemoteRequestedInfoItemHandlerTest",
    size = "small",
    srcs = ["RemoteRequestedInfoItemHandlerTest.java"],
    deps = [
        "//src/main/java/com/google/devtools/build/lib/runtime:blaze_command_cluster",
        "//src/main/java/com/google/devtools/build/lib/runtime/commands:paths_to_replace_utils",
        "//src/main/java/com/google/devtools/build/lib/runtime/commands/info",
        "//src/main/protobuf:command_server_java_proto",
        "//src/test/java/com/google/devtools/build/lib/buildtool/util",
        "//third_party/java/junit",
        "//third_party/java/protobuf:protobuf_lite",
        "//third_party/java/truth",
    ],
)

java_test(
    name = "ServerPidInfoItemTest",
    size = "small",
    srcs = ["ServerPidInfoItemTest.java"],
    runtime_deps = ["//third_party/allocation_instrumenter:allocation_instrumenter_runtime"],
    deps = [
        "//src/main/java/com/google/devtools/build/lib/runtime:blaze_command_cluster",
        "//src/main/java/com/google/devtools/build/lib/runtime:blaze_server_startup_options",
        "//src/main/java/com/google/devtools/build/lib/runtime/commands/info",
        "//src/main/java/com/google/devtools/build/lib/vfs",
        "//src/main/java/com/google/devtools/build/lib/vfs/inmemoryfs",
        "//src/main/java/com/google/devtools/common/options",
        "//third_party:junit4",
        "//third_party:mockito",
        "//third_party:truth",
    ],
)
