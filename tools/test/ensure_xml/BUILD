load("@rules_go//go:def.bzl", "go_binary", "go_cross_binary", "go_library", "go_test")

package(default_visibility = ["//visibility:private"])

filegroup(
    name = "srcs",
    srcs = glob(["*"]),
    visibility = ["//tools/test:__pkg__"],
)

filegroup(
    name = "embedded_files",
    srcs = [
        "ensure_xml_darwin_arm64",
        "ensure_xml_linux_amd64",
        "ensure_xml_linux_arm64",
    ],
    visibility = ["//tools/test:__pkg__"],
)

go_library(
    name = "ensure_xml_lib",
    srcs = ["main.go"],
    importpath = "github.com/bazelbuild/bazel/tools/test/ensure_xml",
    visibility = ["//visibility:private"],
)

go_binary(
    name = "ensure_xml",
    embed = [":ensure_xml_lib"],
    gc_linkopts = [
        "-s",
        "-w",
    ],
    pure = "on",
)

go_test(
    name = "ensure_xml_test",
    srcs = ["main_test.go"],
    embed = [":ensure_xml_lib"],
    pure = "on",
)

[
    go_cross_binary(
        name = "ensure_xml_" + platform + "_build",
        platform = "@rules_go//go/toolchain:" + platform,
        target = ":ensure_xml",
    )
    for platform in [
        "darwin_amd64",
        "darwin_arm64",
        "linux_amd64",
        "linux_arm64",
        "windows_amd64",
        "windows_arm64",
    ]
]
