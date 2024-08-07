cmake_minimum_required(VERSION 3.6.3)

project(zlm LANGUAGES C)

#set(OPEN_SRC_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/open-source" CACHE PATH "Libraries will be downloaded and build in this directory.")

message("OPEN_SRC_INSTALL_PREFIX=${OPEN_SRC_INSTALL_PREFIX}")

if (WIN32)
    if(NOT DEFINED PKG_CONFIG_EXECUTABLE)
      if(EXISTS "C:\\gstreamer\\1.0\\x86_64\\bin\\pkg-config.exe")
        set(PKG_CONFIG_EXECUTABLE "C:\\gstreamer\\1.0\\x86_64\\bin\\pkg-config.exe")
      endif()
    elseif(DEFINED PKG_CONFIG_EXECUTABLE)
      message(STATUS "Gstreamer pkg-config path set to ${PKG_CONFIG_EXECUTABLE}")
    else()
      message(FATAL_ERROR "Gstreamer not found in default path. Set the appropriate path with -DPKG_CONFIG_EXECUTABLE=<path>")
    endif()
endif()
  
find_package(PkgConfig REQUIRED)

pkg_check_modules(GST gstreamer-1.0)
if(GST_FOUND)

  if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/opt/libffi/lib/pkgconfig")
    find_library(Intl "intl" REQUIRED PATHS "/usr/local/opt/gettext/lib")
    link_directories("/usr/local/opt/gettext/lib")
  endif()

  pkg_check_modules(GLIB2 REQUIRED glib-2.0)
  pkg_check_modules(GST_APP REQUIRED gstreamer-app-1.0)
  pkg_check_modules(GOBJ2 REQUIRED gobject-2.0)
  message("gstreamer found. Will build gstreamer samples")

  set(OPEN_SRC_INCLUDE_DIRS
      ${OPEN_SRC_INCLUDE_DIRS} ${GLIB2_INCLUDE_DIRS} ${GST_INCLUDE_DIRS}
      ${GST_APP_INCLUDE_DIRS} ${GOBJ2_INCLUDE_DIRS})

  set(GST_SAMPLE_LIBRARIES ${GLIB2_LIBRARIES} ${GST_LIBRARIES}
                           ${GST_APP_LIBRARIES} ${GOBJ2_LIBRARIES} ${Intl})

  link_directories(${GLIB2_LIBRARY_DIRS})
  link_directories(${GST_LIBRARY_DIRS})
  link_directories(${GST_APP_LIBRARY_DIRS})
  link_directories(${GOBJ2_LIBRARY_DIRS})
else()
  message("gstreamer not found. Will not build gstreamer samples")
endif()

include_directories(${OPEN_SRC_INSTALL_PREFIX}/include)
include_directories(${OPEN_SRC_INCLUDE_DIRS})
link_directories(${OPEN_SRC_INSTALL_PREFIX}/lib)

# copy sample frames to this subproject build folder, in case developer runs sample program with command `kvsWebrtcClientMaster` from `build/samples` dir.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/pcmSample" DESTINATION .)
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../samples/h264SampleFrames" DESTINATION .)

file(GLOB WEBRTC_ZLM_CLIENT_SOURCE_FILES "${CMAKE_SOURCE_DIR}/src/source/Zlm/*.c")

add_library(cyWebrtcZlmClient ${LINKAGE} ${WEBRTC_ZLM_CLIENT_SOURCE_FILES})

if(USE_MBEDTLS)
  target_compile_definitions(cyWebrtcZlmClient PRIVATE LWS_WITH_MBEDTLS)
endif()

target_link_libraries(
  cyWebrtcZlmClient
  PUBLIC
        kvsCommonLws
        ${LIBWEBSOCKETS_LIBRARIES}
  PRIVATE
        kvspicUtils
        kvspicState
        ${CMAKE_THREAD_LIBS_INIT}
        ${EXTRA_DEPS}
        ${OPENSSL_SSL_LIBRARY}
        ${OPENSSL_CRYPTO_LIBRARY}
        ${GPERFTOOLS_MALLOC_LIBRARIES}
        ${GPERFTOOLS_PROFILER_LIBRARIES}
        ${MBEDTLS_LIBRARIES})

if(COMPILER_WARNINGS)
  target_compile_options(cyWebrtcZlmClient PUBLIC -Wall -Werror -pedantic -Wextra -Wno-unknown-warning-option)
endif()

add_executable(
  cySamplesZlmClientMaster
  ${CMAKE_CURRENT_SOURCE_DIR}/g711.c
  ${CMAKE_CURRENT_SOURCE_DIR}/g711_table.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Common.c
  ${CMAKE_CURRENT_SOURCE_DIR}/cySamplesZlmClientMaster.c
)
target_link_libraries(cySamplesZlmClientMaster
  kvsWebrtcClient
  cyWebrtcZlmClient
  ${EXTRA_DEPS}
  kvsCommonLws
  kvspicUtils
  websockets
)
target_include_directories(cySamplesZlmClientMaster
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/source/Signaling
)

add_executable(
  cyIpcZlmClientMaster
  ${CMAKE_CURRENT_SOURCE_DIR}/g711.c
  ${CMAKE_CURRENT_SOURCE_DIR}/g711_table.c
  ${CMAKE_CURRENT_SOURCE_DIR}/JzCommon.c
  ${CMAKE_CURRENT_SOURCE_DIR}/Common.c
  ${CMAKE_CURRENT_SOURCE_DIR}/cyIpcZlmClientMaster.c
)
target_link_libraries(cyIpcZlmClientMaster
  kvsWebrtcClient
  cyWebrtcZlmClient
  ${EXTRA_DEPS}
  kvsCommonLws
  kvspicUtils
  websockets
  ${CMAKE_CURRENT_SOURCE_DIR}/mips32/lib/libimp.a
  ${CMAKE_CURRENT_SOURCE_DIR}/mips32/lib/libsysutils.a
  ${CMAKE_CURRENT_SOURCE_DIR}/mips32/lib/libalog.a
  dl
  m
  pthread
  rt
)
target_include_directories(cyIpcZlmClientMaster
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/source/Signaling
    ${CMAKE_CURRENT_SOURCE_DIR}/mips32/include
)

install(TARGETS cySamplesZlmClientMaster cyIpcZlmClientMaster
  RUNTIME DESTINATION bin
)

