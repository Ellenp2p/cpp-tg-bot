cmake_minimum_required(VERSION 3.25)
project(tg_bot VERSION 0.1.0 LANGUAGES CXX)

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
    set(TB_BOOST_TARGET Boost::headers)
else()
    set(TB_BOOST_TARGET Boost::boost)
endif()
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(tg_bot
    src/main.cpp
    src/http_client.cpp
    src/http_client.hpp
    src/bot_client.cpp
    src/bot_client.hpp
    src/dispatcher.cpp
    src/dispatcher.hpp
    src/long_poll.cpp
    src/long_poll.hpp
    src/rate_limiter.cpp
    src/rate_limiter.hpp
)

# 源码是 UTF-8，MSVC 需要显式指定，否则中文/宽字符字符串字面量会乱码
if(MSVC)
    target_compile_options(tg_bot PRIVATE /utf-8)
endif()

target_link_libraries(tg_bot PRIVATE
    ${TB_BOOST_TARGET}
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Threads::Threads
)

# ---- CA 证书包 ----
# Unix 上 OpenSSL 的默认校验路径通常可用；Windows 上没有系统 CA 库，
# 这里在配置阶段下载 Mozilla CA bundle 作为兜底，并放到 exe 旁边。
set(TB_CACERT_PATH "${CMAKE_BINARY_DIR}/cacert.pem")
if(NOT EXISTS "${TB_CACERT_PATH}")
    file(DOWNLOAD "https://curl.se/ca/cacert.pem" "${TB_CACERT_PATH}"
        TIMEOUT 30
        STATUS TB_DOWNLOAD_STATUS)
    if(NOT TB_DOWNLOAD_STATUS MATCHES "^0;")
        message(WARNING "Failed to download cacert.pem (${TB_DOWNLOAD_STATUS}); "
            "TLS verification may fail on platforms without system CA store.")
    endif()
endif()
if(EXISTS "${TB_CACERT_PATH}")
    file(TO_CMAKE_PATH "${TB_CACERT_PATH}" TB_CACERT_PATH_CMAKE)
    target_compile_definitions(tg_bot PRIVATE TB_CACERT_PEM="${TB_CACERT_PATH_CMAKE}")
    add_custom_command(TARGET tg_bot POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TB_CACERT_PATH}" $<TARGET_FILE_DIR:tg_bot>)
endif()

# ---- Linux 全静态链接 ----
option(TB_STATIC "Link a fully static binary on Linux (musl recommended)" ON)
if(TB_STATIC AND UNIX AND NOT APPLE)
    target_link_options(tg_bot PRIVATE -static -static-libgcc -static-libstdc++)
endif()
