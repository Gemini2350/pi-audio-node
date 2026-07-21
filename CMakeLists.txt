cmake_minimum_required(VERSION 3.18)
project(pi-audio-node VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(pi-audio-node
    src/main.cpp
    src/config.cpp
    src/log.cpp
    src/ptp/ptpclient.cpp
    src/ptp/bmca.cpp
    src/rtp/sdp.cpp
    src/rtp/rtpsender.cpp
    src/rtp/rtpreceiver.cpp
    src/audio/sources.cpp
    src/audio/alsaout.cpp
    src/audio/meters.cpp
    src/nmos/node.cpp
    src/nmos/connection.cpp
    src/nmos/is12.cpp
    src/nmos/dnssd.cpp
    src/web/server.cpp
    third_party/civetweb.c
)

target_include_directories(pi-audio-node PRIVATE src third_party)
target_compile_definitions(pi-audio-node PRIVATE
    NO_SSL NO_CGI NO_CACHING USE_WEBSOCKET
    APP_VERSION="${PROJECT_VERSION}"
)
# civetweb is C - silence its warnings, keep ours strict
set_source_files_properties(third_party/civetweb.c PROPERTIES COMPILE_FLAGS "-w")
target_compile_options(pi-audio-node PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>
)

find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
find_library(SAMPLERATE_LIB samplerate REQUIRED)
target_link_libraries(pi-audio-node PRIVATE Threads::Threads ALSA::ALSA ${SAMPLERATE_LIB} resolv)

install(TARGETS pi-audio-node RUNTIME DESTINATION bin)
install(DIRECTORY webui/ DESTINATION share/pi-audio-node/webui)
