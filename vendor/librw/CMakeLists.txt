set(librw_MAINPROJECT ON)
if(DEFINED PROJECT_NAME)
    set(librw_MAINPROJECT OFF)
endif()

cmake_minimum_required(VERSION 3.13)
project(librw
        VERSION 0.0.1
        LANGUAGES C CXX
)
set(librw_AUTHOR aap)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# ===========================================================================
# Платформы
# ===========================================================================
if(WIN32)
    set(LIBRW_PLATFORMS "NULL" "GL3" "D3D9" "WGPU")
    set(LIBRW_PLATFORM_GL3_REQUIRES_OPENGL ON)
elseif(NINTENDO_SWITCH)
    set(LIBRW_PLATFORMS "NULL" "GL3")
    set(LIBRW_PLATFORM_GL3_REQUIRES_OPENGL OFF)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/nx")
    include(NXFunctions)
elseif(PS2)
    set(LIBRW_PLATFORMS "PS2")
    set(LIBRW_PLATFORM_GL3_REQUIRES_OPENGL OFF)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/ps2")
    include(PS2Functions)
else()
    set(LIBRW_PLATFORMS "NULL" "GL3" "WGPU")
    set(LIBRW_PLATFORM_GL3_REQUIRES_OPENGL ON)
endif()

list(GET LIBRW_PLATFORMS 0 LIBRW_PLATFORM_DEFAULT)
set(LIBRW_PLATFORM "${LIBRW_PLATFORM_DEFAULT}" CACHE STRING "Platform (choices=${LIBRW_PLATFORMS})")
set_property(CACHE LIBRW_PLATFORM PROPERTY STRINGS ${LIBRW_PLATFORMS})
message(STATUS "LIBRW_PLATFORM = ${LIBRW_PLATFORM} (choices=${LIBRW_PLATFORMS})")
set("LIBRW_PLATFORM_${LIBRW_PLATFORM}" ON)
if(NOT LIBRW_PLATFORM IN_LIST LIBRW_PLATFORMS)
    message(FATAL_ERROR "Illegal LIBRW_PLATFORM=${LIBRW_PLATFORM}")
endif()

# ===========================================================================
# GL3 gfxlib (только для GL3)
# ===========================================================================
if(LIBRW_PLATFORM_GL3)
    set(LIBRW_GL3_GFXLIBS "GLFW" "SDL2" "SDL3")
    set(LIBRW_GL3_GFXLIB "GLFW" CACHE STRING "gfxlib for gl3 (choices=${LIBRW_GL3_GFXLIBS})")
    set_property(CACHE LIBRW_GL3_GFXLIB PROPERTY STRINGS ${LIBRW_GL3_GFXLIBS})
    message(STATUS "LIBRW_GL3_GFXLIB = ${LIBRW_GL3_GFXLIB} (choices=${LIBRW_GL3_GFXLIBS})")
    if(NOT LIBRW_GL3_GFXLIB IN_LIST LIBRW_GL3_GFXLIBS)
        message(FATAL_ERROR "Illegal LIBRW_GL3_GFXLIB=${LIBRW_GL3_GFXLIB}")
    endif()
endif()

# WGPU всегда использует GLFW — задаём переменную для skeleton и tools
if(LIBRW_PLATFORM_WGPU)
    set(LIBRW_GL3_GFXLIB "GLFW")
endif()

if(LIBRW_PLATFORM_PS2)
    enable_language(DSM)
endif()

if(NOT COMMAND librw_platform_target)
    function(librw_platform_target)
    endfunction()
endif()

include(CMakeDependentOption)

option(LIBRW_TOOLS    "Build librw tools"    ${librw_MAINPROJECT})
option(LIBRW_INSTALL  "Install librw files"  ${librw_MAINPROJECT})
cmake_dependent_option(LIBRW_EXAMPLES "Build librw examples" ON
        "LIBRW_TOOLS;NOT LIBRW_PLATFORM_NULL" OFF)

if(LIBRW_INSTALL)
    include(GNUInstallDirs)
    set(LIBRW_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}/librw")
endif()

# ===========================================================================
# WGPU: находим wgpu-native до add_subdirectory(src)
# ===========================================================================
if(LIBRW_PLATFORM_WGPU)
    message(STATUS "[wgpu] Configuring WebGPU backend...")

    if(WIN32)
        set(WGPU_OS_SUBDIR "windows-x64")
    elseif(UNIX)
        set(WGPU_OS_SUBDIR "linux-x64")
    else()
        message(FATAL_ERROR "[wgpu] Unsupported OS for wgpu-native.")
    endif()

    set(WGPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/wgpu-native/${WGPU_OS_SUBDIR}"
            CACHE PATH "Path to wgpu-native")

    find_path(WGPU_INCLUDE_DIR
            NAMES webgpu/wgpu.h
            PATHS "${WGPU_DIR}/include"
            NO_DEFAULT_PATH)
    find_library(WGPU_LIBRARY
            NAMES wgpu_native
            PATHS "${WGPU_DIR}/lib"
            NO_DEFAULT_PATH)

    if(NOT WGPU_INCLUDE_DIR OR NOT WGPU_LIBRARY)
        message(FATAL_ERROR "[wgpu] wgpu-native not found in ${WGPU_DIR}")
    endif()
    message(STATUS "[wgpu] Headers : ${WGPU_INCLUDE_DIR}")
    message(STATUS "[wgpu] Library : ${WGPU_LIBRARY}")

    # Импортируемая цель
    # Стало:
    if(WIN32)
        # На Windows статика недоступна (только .dll + .lib импорт), оставляем SHARED
        add_library(wgpu_native SHARED IMPORTED GLOBAL)
        set_target_properties(wgpu_native PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${WGPU_INCLUDE_DIR}"
                # wgpu_native.dll.lib — импорт-либа для DLL (61К). НЕ статическая
                # wgpu_native.lib (53М): та потянула бы весь Rust-рантайм и его
                # системные зависимости (ws2_32/bcrypt/ntdll/d3d12/…) в линковку.
                IMPORTED_IMPLIB   "${WGPU_DIR}/lib/wgpu_native.dll.lib"
                IMPORTED_LOCATION "${WGPU_DIR}/lib/wgpu_native.dll"
        )
    else()
        # Linux/Mac: берём .a — wgpu вшивается в librw, .so не нужна
        find_library(WGPU_STATIC_LIBRARY
                NAMES libwgpu_native.a
                PATHS "${WGPU_DIR}/lib"
                NO_DEFAULT_PATH)
        add_library(wgpu_native STATIC IMPORTED GLOBAL)
        set_target_properties(wgpu_native PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${WGPU_INCLUDE_DIR}"
                IMPORTED_LOCATION "${WGPU_STATIC_LIBRARY}"
                # Rust runtime deps
                INTERFACE_LINK_LIBRARIES "pthread;dl;m;gcc_s"
        )
    endif()

    # GLFW нужен и для WGPU
    if(NOT TARGET glfw)
        find_package(glfw3 REQUIRED)
    endif()

    # Wayland headers могут лежать в нестандартном месте (openSUSE: /usr/include/wayland)
    find_path(WAYLAND_INCLUDE_DIR
            NAMES wayland-client.h
            PATHS /usr/include/wayland /usr/include
    )
    if(WAYLAND_INCLUDE_DIR)
        message(STATUS "[wgpu] Wayland headers: ${WAYLAND_INCLUDE_DIR}")
    endif()
endif()

# ===========================================================================
add_subdirectory(src)
# ===========================================================================

# После add_subdirectory(src) таргет librw уже существует — линкуем wgpu
if(LIBRW_PLATFORM_WGPU)
    target_compile_definitions(librw PUBLIC LIBRW_GLFW)
    target_link_libraries(librw PUBLIC wgpu_native glfw)
    # pthread/dl/m — только Unix; на Windows их нет и линковка бы упала
    if(NOT WIN32)
        target_link_libraries(librw PRIVATE pthread dl m)
    endif()

    if(WAYLAND_INCLUDE_DIR)
        target_include_directories(librw PRIVATE "${WAYLAND_INCLUDE_DIR}")
    endif()

    message(STATUS "[wgpu] wgpu-native linked to librw")
endif()

# ===========================================================================
# skeleton и tools
# ===========================================================================
if(LIBRW_TOOLS AND NOT LIBRW_PLATFORM_PS2 AND NOT LIBRW_PLATFORM_NULL)
    add_subdirectory(skeleton)
endif()

add_subdirectory(tools)

# ===========================================================================
# Install / CPack
# ===========================================================================
if(LIBRW_INSTALL)
    include(CMakePackageConfigHelpers)
    configure_package_config_file(cmake/librw-config.cmake.in librw-config.cmake
            INSTALL_DESTINATION "${CMAKE_INSTALL_PREFIX}"
    )
    install(
            FILES "${CMAKE_CURRENT_BINARY_DIR}/librw-config.cmake"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    )
    install(
            EXPORT librw-targets NAMESPACE librw::
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}"
    )
    export(TARGETS librw NAMESPACE librw:: FILE librw-targets.cmake)

    if(DEFINED LIBRW_GL3_GFXLIB AND LIBRW_GL3_GFXLIB STREQUAL "SDL2")
        install(
                FILES "${CMAKE_CURRENT_LIST_DIR}/cmake/FindSDL2.cmake"
                DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake"
        )
    endif()

    string(REPLACE "." ";" cmake_c_compiler_version_list "${CMAKE_C_COMPILER_VERSION}")
    list(GET cmake_c_compiler_version_list 0 cmake_c_compiler_version_major)

    string(TOLOWER "${LIBRW_PLATFORM}" librw_platform)
    set(compiler)
    set(os)
    if(NOT LIBRW_PLATFORM STREQUAL "PS2")
        if(MSVC)
            set(compiler "-msvc${MSVC_VERSION}")
        elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
            set(compiler "-gcc${cmake_c_compiler_version_major}")
        elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
            set(compiler "-clang${cmake_c_compiler_version_major}")
        elseif(CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
            set(compiler "-appleclang${cmake_c_compiler_version_major}")
        else()
            set(compiler "-UNK")
            message(WARNING "Unknown compiler.")
        endif()
    endif()
    if(LIBRW_PLATFORM_NULL)
        set(platform "-null")
    elseif(LIBRW_PLATFORM_PS2)
        set(platform "-ps2")
    elseif(LIBRW_PLATFORM_GL3)
        if(LIBRW_GL3_GFXLIB STREQUAL "GLFW")
            set(platform "-gl3-glfw")
        elseif(LIBRW_GL3_GFXLIB STREQUAL "SDL2")
            set(platform "-gl3-sdl2")
        elseif(LIBRW_GL3_GFXLIB STREQUAL "SDL3")
            set(platform "-gl3-sdl3")
        endif()
    elseif(LIBRW_PLATFORM_D3D9)
        set(platform "-d3d9")
    elseif(LIBRW_PLATFORM_WGPU)
        set(platform "-wgpu-glfw")
    endif()
    if(NOT LIBRW_PLATFORM_PS2)
        if(WIN32)
            set(os "-win")
        elseif(NINTENDO_SWITCH)
            set(os "-switch")
        elseif(PS2)
            set(os "-ps2")
        elseif(APPLE)
            set(os "-apple")
        elseif(UNIX)
            set(os "-linux")
        else()
            set(os "-UNK")
            message(WARNING "Unknown OS.")
        endif()
    endif()

    set(CPACK_PACKAGE_NAME "${PROJECT_NAME}${platform}${os}${compiler}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A (partial) re-implementation of RenderWare Graphics")
    set(CPACK_PACKAGE_VENDOR "aap")
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_PACKAGE_NAME}")
    set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}")
    set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}")
    set(CPACK_GENERATOR "TXZ")
    include(CPack)
endif()