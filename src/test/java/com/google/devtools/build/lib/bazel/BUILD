load("@rules_java//java:defs.bzl", "java_test")

package(
    default_applicable_licenses = ["//:license"],
    default_testonly = 1,
    default_visibility = ["//src:__subpackages__"],
)

filegroup(
    name = "srcs",
    testonly = 0,
    srcs = glob(["*"]) + [
        "//src/test/java/com/google/devtools/build/lib/bazel/bzlmod:srcs",
        "//src/test/java/com/google/devtools/build/lib/bazel/debug:srcs",
        "//src/test/java/com/google/devtools/build/lib/bazel/repository:srcs",
        "//src/test/java/com/google/devtools/build/lib/bazel/rules:srcs",
    ],
    visibility = ["//src:__subpackages__"],
)

java_test(
    name = "BazelDiffAwarenessModuleTest",
    srcs = ["BazelDiffAwarenessModuleTest.java"],
    runtime_deps = ["//third_party/allocation_instrumenter:allocation_instrumenter_runtime"],
    deps = [
        "//src/main/java/com/google/devtools/build/lib:runtime",
        "//src/main/java/com/google/devtools/build/lib/bazel:modules",
        "//src/main/java/com/google/devtools/build/lib/events",
        "//src/main/java/com/google/devtools/build/lib/skyframe:local_diff_awareness",
        "//src/main/java/com/google/devtools/common/options",
        "//third_party:junit4",
        "//third_party:mockito",
    ],
)
