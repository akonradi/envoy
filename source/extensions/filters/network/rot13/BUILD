load(
    "@rust_cxx//tools/bazel:rust.bzl",
    "rust_library",
)
load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_extension",
    "envoy_cc_library",
    "envoy_package",
)

licenses(["notice"])  # Apache 2

envoy_package()

rust_library(
    name = "rot13",
    srcs = ["rot13.rs"],
    deps = [
        "//include/envoy/buffer:buffer_interface",
        "//include/envoy/network:connection_interface",
        "//include/envoy/network:filter_interface",
        "//source/common/common:assert_lib",
        "//source/common/common:minimal_logger_lib",
    ],
)

genrule(
    name = "rot13_cxx_header_gen",
    srcs = ["rot13.rs"],
    outs = ["rot13_cxx.h"],
    cmd = "cxxbridge $< --header > $@",
)

genrule(
    name = "rot13_cxx_srcs_gen",
    srcs = ["rot13.rs"],
    outs = ["rot13_cxx.cc"],
    cmd = "cxxbridge $< > $@",
)

cc_library(
    name = "rot13_cxx",
    srcs = ["rot13_cxx.cc"],
    hdrs = ["rot13_cxx.h"],
    deps = [
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    security_posture = "unknown",
    deps = [
        ":rot13_cxx",
        "//include/envoy/registry",
        "//include/envoy/server:filter_config_interface",
        "//source/extensions/filters/network:well_known_names",
        "//source/extensions/filters/network/common:factory_base_lib",
    ],
)
