cmake_minimum_required(VERSION 3.16)

project(pvr_cctv VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Kodi 21 for Windows is built with the dynamic MSVC runtime (/MD), and add-on
# binaries must use the same runtime because the PVR ABI passes std::string.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

find_package(Kodi REQUIRED)

if(NOT WIN32)
  find_package(Threads REQUIRED)
endif()

set(PVR_CCTV_SOURCES
    src/addon.cpp
    src/channels.cpp
    src/epg.cpp
    src/h5e.cpp
    src/http.cpp
    src/json.cpp
    src/proxy.cpp
    src/stream.cpp
    src/url.cpp
)

add_library(pvr_cctv MODULE ${PVR_CCTV_SOURCES})

target_include_directories(pvr_cctv PRIVATE "${KODI_INCLUDE_DIRS}")

if(MSVC)
  target_compile_options(pvr_cctv PRIVATE /W4 /utf-8 /EHsc)
else()
  target_compile_options(pvr_cctv PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The add-on fetches everything through Kodi's VFS on Linux, macOS and Android
# (CCTV_HTTP_KODI_VFS below), so Kodi owns TLS, redirects, proxy settings and
# the cache - and on Android there is no libcurl that an add-on could link
# against. Windows keeps its own WinHTTP client: it needs no extra runtime, is
# already verified end to end, and does not depend on how Kodi's VFS treats
# request headers.
if(WIN32)
  # TARGET_WINDOWS is required: without it Kodi's c-api/addon_base.h defines
  # __declspec(X) to nothing on MSVC, which strips __declspec(dllimport) from
  # every STL declaration included afterwards and breaks the link with
  # unresolved std::locale symbols.
  target_compile_definitions(pvr_cctv PRIVATE
      TARGET_WINDOWS
      _CRT_SECURE_NO_WARNINGS
      NOMINMAX
      WIN32_LEAN_AND_MEAN)
  # WinHTTP is part of Windows, so the add-on has no external DLL dependency
  # and keeps working with a stock Kodi installation.
  target_link_libraries(pvr_cctv PRIVATE winhttp ws2_32)
elseif(ANDROID)
  target_compile_definitions(pvr_cctv PRIVATE
      CCTV_HTTP_KODI_VFS
      TARGET_POSIX
      TARGET_ANDROID)
  target_link_libraries(pvr_cctv PRIVATE Threads::Threads)
elseif(APPLE)
  target_compile_definitions(pvr_cctv PRIVATE
      CCTV_HTTP_KODI_VFS
      TARGET_POSIX
      TARGET_DARWIN)
  target_link_libraries(pvr_cctv PRIVATE Threads::Threads)
elseif(UNIX)
  # timegm()/gmtime_r() are POSIX/GNU extensions; -std=c++17 sets
  # __STRICT_ANSI__ and can otherwise hide their declarations in glibc.
  target_compile_definitions(pvr_cctv PRIVATE
      CCTV_HTTP_KODI_VFS
      TARGET_POSIX
      TARGET_LINUX
      _GNU_SOURCE)
  target_link_libraries(pvr_cctv PRIVATE Threads::Threads)
endif()

set_target_properties(pvr_cctv PROPERTIES
    OUTPUT_NAME "pvr.cctv"
    PREFIX ""
)

if(WIN32)
  set_target_properties(pvr_cctv PROPERTIES SUFFIX ".dll")
elseif(APPLE)
  set_target_properties(pvr_cctv PROPERTIES SUFFIX ".dylib")
else()
  set_target_properties(pvr_cctv PROPERTIES SUFFIX ".so")
endif()

# Keep the binary in a predictable place for the packaging scripts, also with
# multi-config generators (Visual Studio).
set_target_properties(pvr_cctv PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
foreach(CONFIG ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${CONFIG}" CONFIG_UPPER)
  set_target_properties(pvr_cctv PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_BINARY_DIR}/addon"
      LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_BINARY_DIR}/addon"
      ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_BINARY_DIR}/lib"
  )
endforeach()

message(STATUS "Kodi headers: ${KODI_INCLUDE_DIRS}")
if(ANDROID)
  message(STATUS "Target: Android ${ANDROID_ABI} (HTTP backend: Kodi VFS)")
elseif(WIN32)
  message(STATUS "Target: Windows (HTTP backend: WinHTTP)")
else()
  message(STATUS "Target: ${CMAKE_SYSTEM_NAME} (HTTP backend: Kodi VFS)")
endif()

# Standalone end-to-end check of the decrypting proxy (no Kodi required). It
# cannot use the Kodi VFS, so it keeps its own HTTP client (WinHTTP on Windows,
# libcurl elsewhere). Android cannot run it, so it is off by default there.
if(ANDROID)
  set(PVR_CCTV_SMOKE_TEST_DEFAULT OFF)
else()
  set(PVR_CCTV_SMOKE_TEST_DEFAULT ON)
endif()
option(PVR_CCTV_BUILD_SMOKE_TEST "Build the standalone HLS proxy smoke test"
       ${PVR_CCTV_SMOKE_TEST_DEFAULT})

if(PVR_CCTV_BUILD_SMOKE_TEST)
  if(NOT WIN32)
    find_package(CURL REQUIRED)
  endif()

  add_executable(pvr_cctv_proxy_smoke
      tests/proxy_smoke.cpp
      src/h5e.cpp
      src/http.cpp
      src/proxy.cpp
      src/url.cpp
  )
  target_include_directories(pvr_cctv_proxy_smoke PRIVATE "${PROJECT_SOURCE_DIR}/src")
  if(MSVC)
    target_compile_options(pvr_cctv_proxy_smoke PRIVATE /W4 /utf-8 /EHsc)
    target_compile_definitions(pvr_cctv_proxy_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  endif()
  if(WIN32)
    target_link_libraries(pvr_cctv_proxy_smoke PRIVATE winhttp ws2_32)
  elseif(TARGET CURL::libcurl)
    target_link_libraries(pvr_cctv_proxy_smoke PRIVATE CURL::libcurl Threads::Threads)
  else()
    target_include_directories(pvr_cctv_proxy_smoke PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(pvr_cctv_proxy_smoke PRIVATE ${CURL_LIBRARIES} Threads::Threads)
  endif()
  set_target_properties(pvr_cctv_proxy_smoke PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/addon")
endif()
