cmake_minimum_required(VERSION 3.24)

project(mcp_stdio_proxy_adapter
    VERSION 0.1.0
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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

target_compile_definitions(mcp_stdio_proxy_adapter
    PRIVATE
        MCP_PROXY_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    target_link_libraries(mcp_stdio_proxy_adapter PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_definitions(mcp_stdio_proxy_adapter PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(mcp_stdio_proxy_adapter PRIVATE /W4)
else()
    target_compile_options(mcp_stdio_proxy_adapter PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS mcp_stdio_proxy_adapter
    RUNTIME DESTINATION bin
)
