/**
 * @file test_path_encoder.cpp
 * @brief 项目路径编码工具单元测试
 */

#include <catch2/catch_test_macros.hpp>

#include "core/utils/path_encoder.h"

TEST_CASE("path_encoder: encodes Windows path", "[core][utils][path_encoder]") {
    // 路径分隔符 \ 和 : 全替换为 -
    REQUIRE(core::util::encode_project_path(R"(D:\develop\Workspace\workx)") ==
            "D--develop-Workspace-workx");
}

TEST_CASE("path_encoder: encodes POSIX path", "[core][utils][path_encoder]") {
    REQUIRE(core::util::encode_project_path("/home/user/workx") == "-home-user-workx");
}

TEST_CASE("path_encoder: handles trailing separator", "[core][utils][path_encoder]") {
    REQUIRE(core::util::encode_project_path(R"(D:\develop\workx\)") == "D--develop-workx-");
}

TEST_CASE("path_encoder: empty path returns empty", "[core][utils][path_encoder]") {
    REQUIRE(core::util::encode_project_path("").empty());
}

TEST_CASE("path_encoder: round-trip stable (deterministic)", "[core][utils][path_encoder]") {
    auto p1 = core::util::encode_project_path(R"(C:\Users\test\project)");
    auto p2 = core::util::encode_project_path(R"(C:\Users\test\project)");
    REQUIRE(p1 == p2);
    REQUIRE(p1 == "C--Users-test-project");
}
