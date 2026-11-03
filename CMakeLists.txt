cmake_minimum_required(VERSION 3.25)
project(tg_bot_framework VERSION 0.2.0 LANGUAGES CXX)

# Warden 独立版本号；release workflow 会传入对应 tag 的版本。
set(WARDEN_VERSION "0.2.1" CACHE STRING "Warden application version")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码是 UTF-8（含中文注释），MSVC 必须显式指定，否则按系统代码页解析会报怪错
if(MSVC)
    add_compile_options(/utf-8 /permissive-)
endif()
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
find_package(unofficial-argon2 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ---- 框架库 ----
add_library(tgbot STATIC
    src/http_client.cpp
    src/amount.cpp
    src/bot_client.cpp
    src/chain_health.cpp
    src/dispatcher.cpp
    src/grab_queue.cpp
    src/job_queue.cpp
    src/metrics.cpp
    src/long_poll.cpp
    src/lottery_panel.cpp
    src/panel.cpp
    src/rate_limiter.cpp
    src/reconciliation.cpp
    src/red_packet_batch.cpp
    src/red_packet_ledger.cpp
    src/red_packet_panel.cpp
    src/red_packet_service.cpp
    src/rich_text.cpp
    src/sqlite_state_store.cpp
    src/stream_message.cpp
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

if(WIN32)
    target_link_libraries(tgbot PUBLIC psapi)
endif()

# 源码是 UTF-8，MSVC 需要显式指定，否则中文/宽字符字符串字面量会乱码
# /bigobj：webhook.cpp 的模板协程在 Debug(/Od) 下会触发 C1128（对象节数超限），
# Release 不触发，但没人想"只有 Debug 编不过"
if(MSVC)
    target_compile_options(tgbot PUBLIC /utf-8 /bigobj /permissive-)
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
# -static 能否成立，取决于依赖是不是静态库：
#   * Alpine/musl + triplets/x64-linux-musl.cmake（VCPKG_LIBRARY_LINKAGE
#     static）→ 拿到 .a，加 -static 产出"拷到任何 Linux 都能跑"的单文件；
#   * 普通 glibc + vcpkg 默认 x64-linux（VCPKG_LIBRARY_LINKAGE dynamic）→
#     拿到 .so，加 -static 会直接报 attempted static link of dynamic object。
# 所以默认值按 triplet 自动推断，仍然可以用 -DTGBOT_STATIC=ON/OFF 强制覆盖。
if(DEFINED TGBOT_STATIC)
    set(_tgbot_static "${TGBOT_STATIC}")            # 命令行/缓存里显式指定，尊重它
elseif(VCPKG_TARGET_TRIPLET MATCHES "musl")
    set(_tgbot_static ON)
else()
    set(_tgbot_static OFF)
endif()
set(TGBOT_STATIC "${_tgbot_static}" CACHE BOOL
    "Link fully static binaries on Linux (needs a static/musl triplet)")

if(TGBOT_STATIC AND UNIX AND NOT APPLE)
    if(NOT VCPKG_TARGET_TRIPLET MATCHES "musl|static")
        message(WARNING
            "TGBOT_STATIC=ON 但 triplet '${VCPKG_TARGET_TRIPLET}' 看起来是动态库，"
            "链接大概率失败；请改用 musl/静态 triplet，或设 -DTGBOT_STATIC=OFF")
    endif()
    target_link_options(tgbot PUBLIC -static -static-libgcc -static-libstdc++)
endif()

# ---- 单元测试 ----
option(TGBOT_BUILD_TESTS "Build unit tests" ON)
if(TGBOT_BUILD_TESTS)
    enable_testing()
    add_executable(tgbot_tests tests/tests.cpp)
    target_link_libraries(tgbot_tests PRIVATE tgbot)
    add_test(NAME tgbot_tests COMMAND tgbot_tests)

    # Warden 的持久层/解析函数（不需要 Telegram 网络）
    add_executable(warden_tests
        tests/warden_tests.cpp
        examples/warden/store.cpp
        examples/warden/health.cpp)
    target_link_libraries(warden_tests PRIVATE tgbot)
    target_include_directories(warden_tests PRIVATE examples/warden)
    add_test(NAME warden_tests COMMAND warden_tests)
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

    # Warden：多类型媒体定时清理 + 持久化任务队列 + 拥有者私聊后台
    add_executable(warden
        examples/warden/main.cpp
        examples/warden/store.cpp
        examples/warden/scheduler.cpp
        examples/warden/alerts.cpp
        examples/warden/health.cpp)
    target_link_libraries(warden PRIVATE tgbot)
    target_include_directories(warden PRIVATE examples/warden)
    target_compile_definitions(warden PRIVATE WARDEN_VERSION="${WARDEN_VERSION}")
    if(EXISTS "${TGBOT_CACERT_PATH}")
        add_custom_command(TARGET warden POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${TGBOT_CACERT_PATH}" $<TARGET_FILE_DIR:warden>)
    endif()
endif()

# ---- Sui SDK（链上部分，独立静态库） ----
add_library(sui_sdk STATIC
    sui/src/client.cpp
    sui/src/crypto.cpp
    sui/src/pow.cpp
    sui/src/transaction.cpp
)
target_include_directories(sui_sdk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sui/include")
target_link_libraries(sui_sdk PUBLIC OpenSSL::Crypto nlohmann_json::nlohmann_json unofficial::argon2::libargon2 tgbot)

if(TGBOT_BUILD_TESTS)
    add_executable(sui_tests sui/tests/sui_tests.cpp)
    target_link_libraries(sui_tests PRIVATE sui_sdk)
    add_test(NAME sui_tests COMMAND sui_tests)

    add_executable(sui_cli sui/tools/sui_cli.cpp)
    target_link_libraries(sui_cli PRIVATE sui_sdk)
endif()
