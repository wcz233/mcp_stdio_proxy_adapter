cmake_minimum_required(VERSION 3.18)

project(mcp_stdio_proxy_adapter
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(MCP_TCP_SECURITY "mtls" CACHE STRING "TCP security: mtls or plaintext")
set_property(CACHE MCP_TCP_SECURITY PROPERTY STRINGS mtls plaintext)
if(NOT (MCP_TCP_SECURITY STREQUAL "mtls" OR MCP_TCP_SECURITY STREQUAL "plaintext"))
    message(FATAL_ERROR
        "MCP_TCP_SECURITY must be mtls or plaintext, got: ${MCP_TCP_SECURITY}")
endif()

option(MCP_PLATFORM_WINDOWS "Build mcp_stdio_proxy_adapter for Windows" OFF)
option(MCP_PLATFORM_LINUX "Build mcp_stdio_proxy_adapter for Linux" OFF)

if(MCP_PLATFORM_WINDOWS AND MCP_PLATFORM_LINUX)
    message(FATAL_ERROR "Only one of MCP_PLATFORM_WINDOWS and MCP_PLATFORM_LINUX may be enabled")
elseif(MCP_PLATFORM_WINDOWS)
    set(MCP_PROXY_PLATFORM "windows")
elseif(MCP_PLATFORM_LINUX)
    set(MCP_PROXY_PLATFORM "linux")
elseif(WIN32)
    set(MCP_PROXY_PLATFORM "windows")
elseif(UNIX AND NOT APPLE)
    set(MCP_PROXY_PLATFORM "linux")
else()
    message(FATAL_ERROR "Unsupported target platform for mcp_stdio_proxy_adapter")
endif()

if(MCP_PROXY_PLATFORM STREQUAL "windows" AND NOT WIN32)
    message(FATAL_ERROR "windows_defconfig.cmake requires a Windows target toolchain")
endif()

if(MCP_PROXY_PLATFORM STREQUAL "linux" AND NOT (UNIX AND NOT APPLE))
    message(FATAL_ERROR "linux_defconfig.cmake requires a Linux target toolchain")
endif()

message(STATUS "mcp_stdio_proxy_adapter platform: ${MCP_PROXY_PLATFORM}")

add_executable(mcp_stdio_proxy_adapter)

target_sources(mcp_stdio_proxy_adapter
    PRIVATE
        src/backend_client.c
        src/main.c
        src/stdio_frontend.c
)

target_include_directories(mcp_stdio_proxy_adapter
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(MCP_TCP_SECURITY STREQUAL "mtls")
    if(NOT TARGET MCP::mbedtls)
        find_package(MbedTLS CONFIG REQUIRED)
    endif()
    if(NOT TARGET MCP::jansson)
        find_package(jansson CONFIG REQUIRED)
    endif()

    target_sources(mcp_stdio_proxy_adapter PRIVATE src/network_security_config.c)
    if(TARGET MCP::mbedtls)
        target_link_libraries(mcp_stdio_proxy_adapter PRIVATE MCP::mbedtls)
    else()
        target_link_libraries(mcp_stdio_proxy_adapter PRIVATE MbedTLS::mbedtls)
    endif()
    if(TARGET MCP::jansson)
        target_link_libraries(mcp_stdio_proxy_adapter PRIVATE MCP::jansson)
    elseif(TARGET jansson::jansson)
        target_link_libraries(mcp_stdio_proxy_adapter PRIVATE jansson::jansson)
    elseif(TARGET jansson)
        target_link_libraries(mcp_stdio_proxy_adapter PRIVATE jansson)
    else()
        message(FATAL_ERROR "Jansson target not found")
    endif()

    set(_mcp_proxy_security_default "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../config/network/network_security.json")
        set(_mcp_proxy_security_default
            "${CMAKE_CURRENT_SOURCE_DIR}/../config/network/network_security.json")
    endif()
    target_compile_definitions(mcp_stdio_proxy_adapter
        PRIVATE
            MCP_TCP_SECURITY_MTLS=1
            MCP_NETWORK_SECURITY_DEFAULT_CONFIG="${_mcp_proxy_security_default}"
    )
else()
    target_compile_definitions(mcp_stdio_proxy_adapter PRIVATE MCP_TCP_SECURITY_MTLS=0)
endif()

if(MCP_PROXY_PLATFORM STREQUAL "windows")
    target_compile_definitions(mcp_stdio_proxy_adapter PRIVATE MCP_PLATFORM_WINDOWS=1)
    target_link_libraries(mcp_stdio_proxy_adapter PRIVATE ws2_32)
elseif(MCP_PROXY_PLATFORM STREQUAL "linux")
    target_compile_definitions(mcp_stdio_proxy_adapter PRIVATE MCP_PLATFORM_LINUX=1 _POSIX_C_SOURCE=200809L)
endif()

target_compile_definitions(mcp_stdio_proxy_adapter
    PRIVATE
        MCP_PROXY_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_definitions(mcp_stdio_proxy_adapter PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_stdio_proxy_adapter PRIVATE /W4)
else()
    target_compile_options(mcp_stdio_proxy_adapter PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS mcp_stdio_proxy_adapter
    RUNTIME DESTINATION bin
)
