cmake_minimum_required(VERSION 3.14)

set(EXECUTABLE reVC)
set(PROJECT REVC)

project(${EXECUTABLE} C CXX)
set(${PROJECT}_AUTHOR "${PROJECT} Team")
set(CMAKE_CXX_STANDARD 23)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GetGitRevisionDescription)
get_git_head_revision(GIT_REFSPEC GIT_SHA1 "ALLOW_LOOKING_ABOVE_CMAKE_SOURCE_DIR")
message(STATUS "Building ${CMAKE_PROJECT_NAME} GIT SHA1: ${GIT_SHA1}")


if(NINTENDO_SWITCH)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/nx")
    include(NXFunctions)
endif()

if(NOT COMMAND reVC_platform_target)
    function(reVC_platform_target)
    endfunction()
endif()

if(WIN32)
    set(${PROJECT}_AUDIOS "OAL" "MSS")
else()
    set(${PROJECT}_AUDIOS "OAL")
endif()

set(${PROJECT}_AUDIO "OAL" CACHE STRING "Audio")

option(${PROJECT}_WITH_OPUS "Build ${EXECUTABLE} with opus support" OFF)
option(${PROJECT}_WITH_LIBSNDFILE "Build ${EXECUTABLE} with libsndfile (instead of internal decoder)" OFF)

set_property(CACHE ${PROJECT}_AUDIO PROPERTY STRINGS ${${PROJECT}_AUDIOS})
message(STATUS "${PROJECT}_AUDIO = ${${PROJECT}_AUDIO} (choices=${${PROJECT}_AUDIOS})")
set("${PROJECT}_AUDIO_${${PROJECT}_AUDIO}" ON)

if(NOT ${PROJECT}_AUDIO IN_LIST ${PROJECT}_AUDIOS)
    message(FATAL_ERROR "Illegal ${PROJECT}_AUDIO=${${PROJECT}_AUDIO}")
endif()

# ── Discord RPC ────────────────────────────────────────────────────────────────
option(${PROJECT}_WITH_DISCORD_RPC "Build ${EXECUTABLE} with Discord Rich Presence"                OFF)

if(${PROJECT}_WITH_DISCORD_RPC)
    set(DISCORD_RPC_DIR "${PROJECT_SOURCE_DIR}/vendor/discord-rpc"
        CACHE PATH "Path to discord-rpc source directory")

    # discord-rpc uses different IPC backends per OS.
    # Windows: connection_win.cpp + discord_register_win.cpp
    # Linux:   connection_unix.cpp + discord_register_linux.cpp
    # macOS:   connection_unix.cpp + discord_register_osx.cpp
    set(DISCORD_RPC_SOURCES
        "${DISCORD_RPC_DIR}/src/discord_rpc.cpp"
        "${DISCORD_RPC_DIR}/src/rpc_connection.cpp"
        "${DISCORD_RPC_DIR}/src/serialization.cpp"
    )

    if(WIN32)
        list(APPEND DISCORD_RPC_SOURCES
            "${DISCORD_RPC_DIR}/src/connection_win.cpp"
            "${DISCORD_RPC_DIR}/src/discord_register_win.cpp"
        )
    elseif(APPLE)
        list(APPEND DISCORD_RPC_SOURCES
            "${DISCORD_RPC_DIR}/src/connection_unix.cpp"
            "${DISCORD_RPC_DIR}/src/discord_register_osx.m"
        )
    else()  # Linux / BSD
        list(APPEND DISCORD_RPC_SOURCES
            "${DISCORD_RPC_DIR}/src/connection_unix.cpp"
            "${DISCORD_RPC_DIR}/src/discord_register_linux.cpp"
        )
    endif()

    add_library(discord-rpc STATIC ${DISCORD_RPC_SOURCES})

    target_include_directories(discord-rpc PUBLIC
        "${DISCORD_RPC_DIR}/include"
        "${DISCORD_RPC_DIR}/thirdparty/rapidjson/include"
    )

    target_compile_options(discord-rpc PRIVATE -Wno-template-body)

    set_target_properties(discord-rpc PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS ON
    )

    # discord-rpc on Linux needs pthread for its I/O thread
    if(UNIX AND NOT APPLE)
        target_link_libraries(discord-rpc PUBLIC pthread)
    endif()

    message(STATUS "Discord RPC enabled (${DISCORD_RPC_DIR})")
endif()

# ── librw ─────────────────────────────────────────────────────────────────────
option(${PROJECT}_VENDORED_LIBRW "Use vendored librw" ON)

if(${PROJECT}_VENDORED_LIBRW)
    add_subdirectory(vendor/librw)
else()
    find_package(librw REQUIRED)
endif()

# ── Main project ──────────────────────────────────────────────────────────────
add_subdirectory(src)

# ── CPack / install ───────────────────────────────────────────────────────────
if(${PROJECT}_INSTALL)
    install(DIRECTORY gamefiles/ DESTINATION ".")

    if(LIBRW_PLATFORM_NULL)
        set(platform "-null")
    elseif(LIBRW_PLATFORM_PS2)
        set(platform "-ps2")
    elseif(LIBRW_PLATFORM_GL3)
        if(LIBRW_GL3_GFXLIB STREQUAL "GLFW")
            set(platform "-gl3-glfw")
        else()
            set(platform "-gl3-sdl2")
        endif()
    elseif(LIBRW_PLATFORM_D3D9)
        set(platform "-d3d9")
    endif()

    if(${PROJECT}_AUDIO_OAL)
        set(audio "-oal")
    elseif(${PROJECT}_AUDIO_MSS)
        set(audio "-mss")
    endif()

    if(${PROJECT}_WITH_OPUS)
        set(audio "${audio}-opus")
    endif()

    if(NOT LIBRW_PLATFORM_PS2)
        if(WIN32)
            set(os "-win")
        elseif(APPLE)
            set(os "-apple")
        elseif(UNIX)
            set(os "-linux")
        elseif(NINTENDO_SWITCH)
            set(os "-switch")
        else()
            set(compiler "-UNK")
            message(WARNING "Unknown os. Created cpack package will be wrong. (override using cpack -P)")
        endif()
    endif()

    if(${PROJECT}_WITH_DISCORD_RPC)
        set(os "${os}-drpc")
    endif()

    set(CPACK_PACKAGE_NAME "${PROJECT_NAME}${platform}${audio}${os}${compiler}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "GTA Vice City reversed")
    set(CPACK_PACKAGE_VENDOR "GTAModding")
    # FIXME: missing license
    # set(CPACK_PACKAGE_DESCRIPTION_FILE "${PROJECT_SOURCE_DIR}/LICENSE")
    # set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_PACKAGE_NAME}")
    set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}")
    set(CPACK_GENERATOR "TXZ")
    include(CPack)
endif()
