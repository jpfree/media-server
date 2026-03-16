cmake_minimum_required(VERSION 3.10)
project(media_server)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set output directory for build binary
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/../output)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_LINUX")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

option(ENABLE_HTTPS "Enable https support" OFF)
option(ENABLE_RTC "Enable webrtc support" OFF)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/src/base)

# External dependencies
find_package(Threads REQUIRED)

# ---- FFmpeg (static) ----
# 假设 FFmpeg 静态库和头文件安装在 /usr/local
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h
    PATHS /usr/local/include /usr/include
)

find_library(AVCODEC_LIBRARY avcodec
    PATHS /usr/local/lib /usr/lib
)
find_library(AVFORMAT_LIBRARY avformat
    PATHS /usr/local/lib /usr/lib
)
find_library(AVUTIL_LIBRARY avutil
    PATHS /usr/local/lib /usr/lib
)
find_library(SWRESAMPLE_LIBRARY swresample
    PATHS /usr/local/lib /usr/lib
)

if(NOT FFMPEG_INCLUDE_DIR OR NOT AVCODEC_LIBRARY OR NOT AVFORMAT_LIBRARY OR NOT AVUTIL_LIBRARY OR NOT SWRESAMPLE_LIBRARY)
    message(FATAL_ERROR "FFmpeg libraries or headers not found")
endif()

# ---- SQLite3 (static) ----
find_path(SQLITE3_INCLUDE_DIR sqlite3.h
    PATHS /usr/local/include /usr/include
)
find_library(SQLITE3_LIBRARY sqlite3
    PATHS /usr/local/lib /usr/lib
)

if(NOT SQLITE3_INCLUDE_DIR OR NOT SQLITE3_LIBRARY)
    message(FATAL_ERROR "SQLite3 library or headers not found")
endif()

# Add source files from src directory
set(MAIN_SOURCES
    src/MediaServer.cpp
    src/MsDbMgr.cpp
    src/MsDevMgr.cpp
    src/MsFileSource.cpp
    src/MsGbServer.cpp
    src/MsGbServerHandler.cpp
    src/MsGbSource.cpp
    src/MsHttpHandler.cpp
    src/MsHttpServer.cpp
    src/MsHttpStream.cpp
    src/MsHttpSink.cpp
    src/MsOnvifHandler.cpp
    src/MsResManager.cpp
    src/MsMediaSink.cpp
    src/MsMediaSource.cpp
    src/MsSourceFactory.cpp
    src/MsRtspSource.cpp
    src/MsRtspSink.cpp
    src/MsJtServer.cpp
    src/MsJtSource.cpp
    src/MsJtHandler.cpp
    src/MsRtmpServer.cpp
    src/MsRtmpHandler.cpp
    src/MsRtmpMsg.cpp
    src/MsRtmpSource.cpp
    src/tinyxml2/tinyxml2.cpp
)

# Add source files from src/base directory
set(BASE_SOURCES
    src/base/MsApp.cpp
    src/base/MsCommon.cpp
    src/base/MsConfig.cpp
    src/base/MsEvent.cpp
    src/base/MsHttpMsg.cpp
    src/base/MsInetAddr.cpp
    src/base/MsLog.cpp
    src/base/MsReactor.cpp
    src/base/MsRtspMsg.cpp
    src/base/MsPortAllocator.cpp
    src/base/MsSipMsg.cpp
    src/base/MsSocket.cpp
    src/base/MsTimer.cpp
    src/base/MsRingBuffer.cpp
    src/base/MsMd5.cpp
    src/base/MsSha1.cpp
    src/base/MsAmf.cpp
)

if(ENABLE_RTC)
    list(APPEND MAIN_SOURCES src/MsRtcServer.cpp)
    list(APPEND MAIN_SOURCES src/MsRtcSource.cpp)
    list(APPEND MAIN_SOURCES src/MsRtcSink.cpp)
endif()

if(ENABLE_HTTPS)
    list(APPEND BASE_SOURCES src/base/MsSslSock.cpp)
endif()

# Create executable
add_executable(media_server ${MAIN_SOURCES} ${BASE_SOURCES})

# Include dirs
target_include_directories(media_server PRIVATE
    ${FFMPEG_INCLUDE_DIR}
    ${SQLITE3_INCLUDE_DIR}
)

# Base libs
target_link_libraries(media_server PRIVATE
    Threads::Threads
    ${AVCODEC_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${SQLITE3_LIBRARY}
)

if(ENABLE_HTTPS)
    find_package(OpenSSL REQUIRED)
    target_compile_definitions(media_server PRIVATE ENABLE_HTTPS=1)
    target_link_libraries(media_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(ENABLE_RTC)
    find_package(LibDataChannel REQUIRED)
    target_compile_definitions(media_server PRIVATE ENABLE_RTC=1)
    target_link_libraries(media_server PRIVATE
        LibDataChannel::LibDataChannel
    )
endif()
