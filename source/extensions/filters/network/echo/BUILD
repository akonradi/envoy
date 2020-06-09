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

# Echo L4 network filter. This is primarily a simplistic example.
# Public docs: docs/root/configuration/network_filters/echo_filter.rst

envoy_package()

rust_library(
    name = "echo_rs",
    srcs = ["echo.rs"],
    deps = [
        ":echo_rs_cxx",
        "@rust_cxx//:cxx",
    ],
)

genrule(
    name = "echo_cxx_header_gen",
    srcs = ["echo.rs"],
    outs = ["echo_cxx.h"],
    cmd = "cxxbridge $< --header > $@",
)

genrule(
    name = "echo_cxx_srcs_gen",
    srcs = ["echo.rs"],
    outs = ["echo_cxx.cc"],
    cmd = "cxxbridge $< > $@",
)

cc_library(
    name = "echo_rs_cxx",
    srcs = ["echo_cxx.cc"],
    hdrs = ["echo_cxx.h"],
    deps = [
    ],
)

envoy_cc_library(
    name = "echo",
    srcs = ["echo.cc"],
    hdrs = ["echo.h"],
    deps = [
        ":echo_rs_cxx",
        "//include/envoy/buffer:buffer_interface",
        "//include/envoy/network:connection_interface",
        "//include/envoy/network:filter_interface",
        "//source/common/common:assert_lib",
        "//source/common/common:minimal_logger_lib",
    ],
)

envoy_cc_extension(
    name = "config",
    srcs = ["config.cc"],
    security_posture = "unknown",
    deps = [
        ":echo",
        "//include/envoy/registry",
        "//include/envoy/server:filter_config_interface",
        "//source/extensions/filters/network:well_known_names",
        "//source/extensions/filters/network/common:factory_base_lib",
        "@envoy_api//envoy/extensions/filters/network/echo/v3:pkg_cc_proto",
    ],
)
