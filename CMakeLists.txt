cmake_minimum_required(VERSION 3.25)
project(tg_bot_framework VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # 静态链接 CRT（/MT），产出单 exe 不依赖 vcruntime DLL
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # Asio 要求显式指定 Windows 版本目标（Windows 10）
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

# ---- 依赖（全部由 vcpkg 提供最新版本） ----
find_package(Boost REQUIRED)
if(TARGET Boost::headers)
    set(TGBOT_BOOST_TARGET Boost::headers)
else()
    set(TGBOT_BOOST_TARGET Boost::boost)
endif()
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ---- 框架库 ----
add_library(tgbot STATIC
    src/http_client.cpp
    src/bot_client.cpp
    src/dispatcher.cpp
    src/long_poll.cpp
    src/rate_limiter.cpp
    src/sqlite_state_store.cpp
    src/webhook.cpp
)

target_include_directories(tgbot PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(tgbot PUBLIC
    ${TGBOT_BOOST_TARGET}
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    unofficial::sqlite3::sqlite3
    Threads::Threads
)

# 源码是 UTF-8，MSVC 需要显式指定，否则中文/宽字符字符串字面量会乱码
if(MSVC)
    target_compile_options(tgbot PUBLIC /utf-8)
endif()

# ---- CA 证书包 ----
# Unix 上 OpenSSL 的默认校验路径通常可用；Windows 上没有系统 CA 库，
# 这里在配置阶段下载 Mozilla CA bundle 作为兜底，并放到 exe 旁边。
set(TGBOT_CACERT_PATH "${CMAKE_BINARY_DIR}/cacert.pem")
if(NOT EXISTS "${TGBOT_CACERT_PATH}")
    file(DOWNLOAD "https://curl.se/ca/cacert.pem" "${TGBOT_CACERT_PATH}"
        TIMEOUT 30
        STATUS TGBOT_DOWNLOAD_STATUS)
    if(NOT TGBOT_DOWNLOAD_STATUS MATCHES "^0;")
        message(WARNING "Failed to download cacert.pem (${TGBOT_DOWNLOAD_STATUS}); "
            "TLS verification may fail on platforms without system CA store.")
    endif()
endif()
if(EXISTS "${TGBOT_CACERT_PATH}")
    file(TO_CMAKE_PATH "${TGBOT_CACERT_PATH}" TGBOT_CACERT_PATH_CMAKE)
    target_compile_definitions(tgbot PRIVATE TB_CACERT_PEM="${TGBOT_CACERT_PATH_CMAKE}")
endif()

# ---- Linux 全静态链接 ----
option(TGBOT_STATIC "Link fully static binaries on Linux (musl recommended)" ON)
if(TGBOT_STATIC AND UNIX AND NOT APPLE)
    target_link_options(tgbot PUBLIC -static -static-libgcc -static-libstdc++)
endif()

# ---- 示例机器人 ----
option(TGBOT_BUILD_EXAMPLES "Build example bots" ON)
if(TGBOT_BUILD_EXAMPLES)
    # echo 演示：回声 / /start / inline 键盘
    add_executable(echo_bot examples/echo_bot/main.cpp)
    target_link_libraries(echo_bot PRIVATE tgbot)
    if(EXISTS "${TGBOT_CACERT_PATH}")
        add_custom_command(TARGET echo_bot POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${TGBOT_CACERT_PATH}" $<TARGET_FILE_DIR:echo_bot>)
    endif()

    # 群管理：贴纸发出 1 分钟后自动删除，/sticker on|off 由群管理员控制
    add_executable(group_admin examples/group_admin/main.cpp)
    target_link_libraries(group_admin PRIVATE tgbot)
    if(EXISTS "${TGBOT_CACERT_PATH}")
        add_custom_command(TARGET group_admin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${TGBOT_CACERT_PATH}" $<TARGET_FILE_DIR:group_admin>)
    endif()
endif()
