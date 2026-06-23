set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

file(GLOB_RECURSE ${PROJECT}_SOURCES "*.cpp" "*.h" "*.rc")

function(header_directories RETURN_LIST)
    file(GLOB_RECURSE ALL_SRCS *.h *.cpp *.c)
    set(RELDIRS)
    foreach(SRC ${ALL_SRCS})
        file(RELATIVE_PATH RELSRC "${CMAKE_CURRENT_SOURCE_DIR}" "${SRC}")
        get_filename_component(RELDIR "${RELSRC}" DIRECTORY)
        list(APPEND RELDIRS ${RELDIR})
    endforeach()
    list(REMOVE_DUPLICATES RELDIRS)
    set(${RETURN_LIST} ${RELDIRS} PARENT_SCOPE)
endfunction()

header_directories(${PROJECT}_INCLUDES)

list(REMOVE_ITEM ${PROJECT}_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/extras/GitSHA1.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp" @ONLY)
list(APPEND ${PROJECT}_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/extras/GitSHA1.cpp")

if(WIN32)
    add_executable(${EXECUTABLE} WIN32 ${${PROJECT}_SOURCES})
else()
    add_executable(${EXECUTABLE} ${${PROJECT}_SOURCES})
endif()

if(${PROJECT}_WITH_DISCORD_RPC)
    target_compile_definitions(${EXECUTABLE} PRIVATE USE_DISCORD_RPC)
    target_include_directories(${EXECUTABLE} PRIVATE "${DISCORD_RPC_DIR}/include")
    target_link_libraries(${EXECUTABLE} PRIVATE discord-rpc)
endif()

target_link_libraries(${EXECUTABLE} PRIVATE
        librw::librw
        Threads::Threads
)

target_include_directories(${EXECUTABLE}
        PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${${PROJECT}_INCLUDES}>
)

target_compile_definitions(${EXECUTABLE}
        PRIVATE
        $<IF:$<CONFIG:DEBUG>,DEBUG,NDEBUG>
        LIBRW
        CMAKE_NO_AUTOLINK
        CMAKE_BUILD
)

if(LIBRW_PLATFORM_D3D9)
    target_compile_definitions(${EXECUTABLE} PUBLIC USE_D3D9)
endif()

if(${PROJECT}_AUDIO STREQUAL "OAL")
    find_package(OpenAL REQUIRED)
    target_include_directories(${EXECUTABLE} PRIVATE ${OPENAL_INCLUDE_DIR})
    target_link_libraries(${EXECUTABLE} PRIVATE ${OPENAL_LIBRARY})
    target_compile_definitions(${EXECUTABLE} PRIVATE ${OPENAL_DEFINITIONS} AUDIO_OAL)

    find_package(SndFile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE SndFile::SndFile)

elseif(${PROJECT}_AUDIO STREQUAL "MSS")
    find_package(MilesSDK REQUIRED)
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_MSS)
    target_link_libraries(${EXECUTABLE} PRIVATE MilesSDK::MilesSDK)
endif()

find_package(mpg123 REQUIRED)
target_link_libraries(${EXECUTABLE} PRIVATE MPG123::libmpg123)

if(${PROJECT}_WITH_OPUS)
    find_package(opusfile REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE opusfile::opusfile)
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OPUS)
endif()

if(${PROJECT}_WITH_LIBSNDFILE)
    target_compile_definitions(${EXECUTABLE} PRIVATE AUDIO_OAL_USE_SNDFILE)
endif()

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(${EXECUTABLE} PRIVATE X11::X11)
endif()

if(${PROJECT}_WITH_SANITIZERS)
    target_compile_options(${EXECUTABLE} PUBLIC
            -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,
            implicit-integer-truncation,implicit-integer-arithmetic-value-change,
            local-bounds,nullability
            -g3 -fno-omit-frame-pointer)
    target_link_options(${EXECUTABLE} PUBLIC
            -fsanitize=undefined,float-divide-by-zero,integer,implicit-conversion,
            implicit-integer-truncation,implicit-integer-arithmetic-value-change,
            local-bounds,nullability)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(${EXECUTABLE} PRIVATE -Wall)
    if(NOT LIBRW_PLATFORM_PS2)
        target_compile_options(${EXECUTABLE} PRIVATE
                -Wextra
                -Wdouble-promotion
                -Wpedantic
        )
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${EXECUTABLE} PUBLIC /Zc:sizedDealloc-)
endif()

set_target_properties(${EXECUTABLE}
        PROPERTIES
        C_STANDARD 11
        C_EXTENSIONS OFF
        C_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
)

if(${PROJECT}_INSTALL)
    install(
            TARGETS ${EXECUTABLE}
            EXPORT ${EXECUTABLE}-targets
            RUNTIME DESTINATION "."
    )
    if(MSVC)
        install(FILES $<TARGET_PDB_FILE:${EXECUTABLE}> DESTINATION "." OPTIONAL)
    endif()
endif()