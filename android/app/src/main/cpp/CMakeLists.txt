
# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project(HasuKikaisann)

add_compile_options(-fdeclspec)
add_compile_options(-Wno-undefined-bool-conversion)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++23")

find_package(xdl REQUIRED CONFIG)
find_package(shadowhook REQUIRED CONFIG)

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
# Shared offline server (repo/server) and vendored libraries (repo/third_party).
get_filename_component(LINKURA_REPO_DIR "${CMAKE_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(LINKURA_SERVER_DIR "${LINKURA_REPO_DIR}/server")
set(LINKURA_THIRD_PARTY_DIR "${LINKURA_REPO_DIR}/third_party")

add_library(${CMAKE_PROJECT_NAME} SHARED
    # List C/C++ source files with relative paths to this CMakeLists.txt.
        libHasuKikaisann.cpp
        LinkuraLocalify/Plugin.cpp
        LinkuraLocalify/Hook.cpp
        LinkuraLocalify/Log.cpp
        LinkuraLocalify/Misc.cpp
        LinkuraLocalify/Local.cpp
        LinkuraLocalify/HttpMock.cpp
        LinkuraLocalify/config/Config.cpp
        LinkuraLocalify/config/version_compatibility.cpp
        # Shared offline server
        ${LINKURA_SERVER_DIR}/src/RouteRegistry.cpp
        ${LINKURA_SERVER_DIR}/src/backend/HttpMockBackend.cpp
        # Hook modules
        LinkuraLocalify/hooks/HookOfflineFixes.cpp
        LinkuraLocalify/hooks/HookShare.cpp
        LinkuraLocalify/hooks/HookSqlCipher.cpp
        LinkuraLocalify/hooks/HookVirtualClock.cpp
        )

# Generate built-in offline API mock json constants (header) at build time.
set(OFFLINE_API_BUILTIN_JSON_DIR "${LINKURA_SERVER_DIR}/data/builtin")
set(OFFLINE_API_BUILTIN_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(OFFLINE_API_BUILTIN_HEADER "${OFFLINE_API_BUILTIN_GEN_DIR}/offline_api_mock_builtin.hpp")
set(HTTP_MOCK_SQL_BUILTIN_DIR "${LINKURA_SERVER_DIR}/db")
set(HTTP_MOCK_SQL_BUILTIN_HEADER "${OFFLINE_API_BUILTIN_GEN_DIR}/http_mock_backend_builtin_sql.hpp")

file(GLOB OFFLINE_API_BUILTIN_FILES CONFIGURE_DEPENDS
    "${OFFLINE_API_BUILTIN_JSON_DIR}/*.json"
    "${OFFLINE_API_BUILTIN_JSON_DIR}/*.headers"
    "${OFFLINE_API_BUILTIN_JSON_DIR}/*.hdr"
)

add_custom_command(
    OUTPUT "${OFFLINE_API_BUILTIN_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            -DINPUT_DIR:PATH=${OFFLINE_API_BUILTIN_JSON_DIR}
            -DOUTPUT_HEADER:FILEPATH=${OFFLINE_API_BUILTIN_HEADER}
            -P "${LINKURA_SERVER_DIR}/cmake/generate_builtin_json_header.cmake"
    DEPENDS
            "${LINKURA_SERVER_DIR}/cmake/generate_builtin_json_header.cmake"
            ${OFFLINE_API_BUILTIN_FILES}
    VERBATIM
)

add_custom_target(generate_offline_api_builtin_header DEPENDS "${OFFLINE_API_BUILTIN_HEADER}")
add_dependencies(${CMAKE_PROJECT_NAME} generate_offline_api_builtin_header)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${OFFLINE_API_BUILTIN_GEN_DIR}")

file(GLOB_RECURSE HTTP_MOCK_SQL_BUILTIN_FILES CONFIGURE_DEPENDS
    "${HTTP_MOCK_SQL_BUILTIN_DIR}/schema/*.sql"
    "${HTTP_MOCK_SQL_BUILTIN_DIR}/seed/*.sql"
    "${HTTP_MOCK_SQL_BUILTIN_DIR}/player-state/*.sql"
    "${HTTP_MOCK_SQL_BUILTIN_DIR}/queries/*.sql"
)

add_custom_command(
    OUTPUT "${HTTP_MOCK_SQL_BUILTIN_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            -DINPUT_DIR:PATH=${HTTP_MOCK_SQL_BUILTIN_DIR}
            -DOUTPUT_HEADER:FILEPATH=${HTTP_MOCK_SQL_BUILTIN_HEADER}
            -P "${LINKURA_SERVER_DIR}/cmake/generate_builtin_sql_header.cmake"
    DEPENDS
            "${LINKURA_SERVER_DIR}/cmake/generate_builtin_sql_header.cmake"
            ${HTTP_MOCK_SQL_BUILTIN_FILES}
    VERBATIM
)

add_custom_target(generate_http_mock_sql_builtin_header DEPENDS "${HTTP_MOCK_SQL_BUILTIN_HEADER}")
add_dependencies(${CMAKE_PROJECT_NAME} generate_http_mock_sql_builtin_header)

set(LINKURA_SQLITE_DIR "${LINKURA_THIRD_PARTY_DIR}/sqlite")
set(LINKURA_SQLITE_C "${LINKURA_SQLITE_DIR}/sqlite3.c")
set(LINKURA_SQLITE_H "${LINKURA_SQLITE_DIR}/sqlite3.h")

if(NOT EXISTS "${LINKURA_SQLITE_C}" OR NOT EXISTS "${LINKURA_SQLITE_H}")
    message(FATAL_ERROR "sqlite is required: expected ${LINKURA_SQLITE_C} and ${LINKURA_SQLITE_H}")
endif()

add_library(linkura_sqlite STATIC "${LINKURA_SQLITE_C}")
target_include_directories(linkura_sqlite PUBLIC "${LINKURA_SQLITE_DIR}")
target_link_libraries(${CMAKE_PROJECT_NAME} linkura_sqlite)

target_link_libraries(${CMAKE_PROJECT_NAME} xdl::xdl)
target_link_libraries(${CMAKE_PROJECT_NAME} shadowhook::shadowhook)

include_directories(LinkuraLocalify)
include_directories(${CMAKE_SOURCE_DIR}/deps)
include_directories(${LINKURA_THIRD_PARTY_DIR})
include_directories(${LINKURA_SERVER_DIR}/src)

set(FMT_DIR "${CMAKE_SOURCE_DIR}/deps/fmt-11.0.2")
include_directories(${FMT_DIR}/include)
add_library(fmt STATIC ${FMT_DIR}/src/format.cc)

# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
target_link_libraries(${CMAKE_PROJECT_NAME}
    # List libraries link to the target library
    android
    log
    fmt)

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_23)
