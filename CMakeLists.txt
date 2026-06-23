cmake_minimum_required(VERSION 3.25)
project(mirage VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(MIRAGE_CXX_FLAGS "")
set(MIRAGE_LINK_FLAGS "")

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(MIRAGE_CXX_FLAGS
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wsign-conversion -Wshadow
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual -Wformat=2
        -Wnull-dereference -Wdouble-promotion
        -Wimplicit-fallthrough -Wno-error=unused-parameter
        -Wno-error=old-style-cast
        -Wno-error=sign-conversion
        -Wno-error=shorten-64-to-32
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND MIRAGE_CXX_FLAGS -fsanitize=address,undefined)
        set(MIRAGE_LINK_FLAGS -fsanitize=address,undefined)
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(MIRAGE_CXX_FLAGS
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wsign-conversion -Wshadow
        -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual -Wformat=2
        -Wdouble-promotion -Wimplicit-fallthrough
        -Wduplicated-cond -Wduplicated-branches -Wlogical-op
        -Wno-error=unused-parameter
        -Wno-error=sign-conversion
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND MIRAGE_CXX_FLAGS -fsanitize=address,undefined)
        set(MIRAGE_LINK_FLAGS -fsanitize=address,undefined)
    endif()
elseif(MSVC)
    set(MIRAGE_CXX_FLAGS
        /W4 /WX /permissive-
        /wd4244 /wd4267 /wd4305 /wd4334 /wd4834
    )
endif()

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

find_package(OpenSSL REQUIRED)
find_package(Vulkan REQUIRED)

if(WIN32)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavformat libavutil libswresample)
    endif()
    if(NOT FFMPEG_FOUND)
        find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
        find_library(AVCODEC_LIB avcodec)
        find_library(AVFORMAT_LIB avformat)
        find_library(AVUTIL_LIB avutil)
        find_library(SWRESAMPLE_LIB swresample)
        if(AVCODEC_LIB)
            add_library(PkgConfig::FFMPEG INTERFACE IMPORTED)
            target_link_libraries(PkgConfig::FFMPEG INTERFACE
                ${AVCODEC_LIB} ${AVFORMAT_LIB} ${AVUTIL_LIB} ${SWRESAMPLE_LIB})
            target_include_directories(PkgConfig::FFMPEG INTERFACE ${FFMPEG_INCLUDE_DIR})
        endif()
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswresample)
endif()

find_program(GLSLC glslc HINTS ${Vulkan_GLSLC_EXECUTABLE} REQUIRED)

set(SHADER_DIR ${CMAKE_SOURCE_DIR}/src/render/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

foreach(SHADER nv12_to_rgb.vert nv12_to_rgb.frag)
    add_custom_command(
        OUTPUT ${SHADER_OUT}/${SHADER}.spv
        COMMAND ${GLSLC} -o ${SHADER_OUT}/${SHADER}.spv ${SHADER_DIR}/${SHADER}
        DEPENDS ${SHADER_DIR}/${SHADER}
        COMMENT "Compiling ${SHADER}"
        VERBATIM
    )
    list(APPEND SHADER_OUTPUTS ${SHADER_OUT}/${SHADER}.spv)
endforeach()

add_executable(mirage
    src/main.cpp
    src/core/error.cpp
    src/core/config.cpp
    src/core/receiver_identity.cpp
    src/core/receiver_adapter.cpp
    src/core/receiver_source.cpp
    src/crypto/fairplay_pairing.cpp
    src/crypto/fairplay_sap.cpp
    src/crypto/srp6a.cpp
    src/crypto/aes_ctr.cpp
    src/crypto/tlv8_chacha.cpp
    src/discovery/mdns_broadcaster.cpp
    src/discovery/service_records.cpp
    src/discovery/interface_enum.cpp
    src/media/local_media_sink.cpp
    src/media/vaapi_decoder.cpp
    src/media/nvdec_decoder.cpp
    src/media/ffmpeg_decoder.cpp
    src/audio/audio_player.cpp
    src/audio/audio_decoder.cpp
    src/render/vulkan_renderer.cpp
    src/protocols/rtsp_server.cpp
    src/protocols/cast_channel.cpp
    src/protocols/cast/framing.cpp
    src/protocols/cast/probe.cpp
    src/protocols/wfd_session.cpp
    src/protocols/receiver_sources.cpp
    src/protocols/airplay/source.cpp
    src/protocols/airplay/media_source.cpp
    src/protocols/airplay/receiver_session.cpp
    src/protocols/cast/source.cpp
    src/protocols/cast/receiver_session.cpp
    src/protocols/wfd/source.cpp
    src/protocols/wfd/receiver_session.cpp
    src/io/tcp.cpp
    src/io/udp.cpp
    src/platform/window.cpp
    src/platform/audio_out.cpp
    ${SHADER_OUTPUTS}
)

if(WIN32)
    target_sources(mirage PRIVATE
        src/io/iocp_loop.cpp
        src/platform/window_win32.cpp
        src/platform/audio_wasapi.cpp
    )
else()
    target_sources(mirage PRIVATE
        src/io/epoll_loop.cpp
        src/platform/window_wayland.cpp
        src/platform/window_x11.cpp
        src/platform/audio_pulse.cpp
        src/platform/audio_alsa.cpp
    )
endif()

target_include_directories(mirage PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(EXISTS /opt/cuda/include)
    target_include_directories(mirage SYSTEM PRIVATE /opt/cuda/include)
endif()
target_compile_options(mirage PRIVATE ${MIRAGE_CXX_FLAGS})
target_link_options(mirage PRIVATE ${MIRAGE_LINK_FLAGS})
target_compile_definitions(mirage PRIVATE
    MIRAGE_SHADER_DIR="${SHADER_OUT}"
    MIRAGE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(mirage PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    Vulkan::Vulkan
    PkgConfig::FFMPEG
    ${CMAKE_DL_LIBS}
)

if(WIN32)
    target_link_libraries(mirage PRIVATE ws2_32 mswsock ole32 uuid)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(mirage_srp6a_test
        tests/test_srp6a.cpp
        src/crypto/srp6a.cpp
    )
    target_include_directories(mirage_srp6a_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_srp6a_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_srp6a_test PRIVATE ${MIRAGE_LINK_FLAGS})
    target_link_libraries(mirage_srp6a_test PRIVATE OpenSSL::Crypto)
    add_test(NAME srp6a COMMAND mirage_srp6a_test)

    add_executable(mirage_airplay_protocol_test
        tests/test_airplay_protocol.cpp
    )
    target_include_directories(mirage_airplay_protocol_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_airplay_protocol_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_airplay_protocol_test PRIVATE ${MIRAGE_LINK_FLAGS})
    add_test(NAME airplay_protocol COMMAND mirage_airplay_protocol_test)

    add_executable(mirage_cast_probe_test
        tests/test_cast_probe.cpp
        src/protocols/cast/framing.cpp
        src/protocols/cast/probe.cpp
    )
    target_include_directories(mirage_cast_probe_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_cast_probe_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_cast_probe_test PRIVATE ${MIRAGE_LINK_FLAGS})
    add_test(NAME cast_probe COMMAND mirage_cast_probe_test)

    add_executable(mirage_cast_framing_test
        tests/test_cast_framing.cpp
        src/protocols/cast/framing.cpp
        src/protocols/cast/probe.cpp
    )
    target_include_directories(mirage_cast_framing_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_cast_framing_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_cast_framing_test PRIVATE ${MIRAGE_LINK_FLAGS})
    add_test(NAME cast_framing COMMAND mirage_cast_framing_test)

    add_executable(mirage_receiver_adapter_test
        tests/test_receiver_adapter.cpp
        src/core/receiver_adapter.cpp
        src/core/receiver_source.cpp
    )
    target_include_directories(mirage_receiver_adapter_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_receiver_adapter_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_receiver_adapter_test PRIVATE ${MIRAGE_LINK_FLAGS})
    add_test(NAME receiver_adapter COMMAND mirage_receiver_adapter_test)

    add_executable(mirage_receiver_sources_test
        tests/test_receiver_sources.cpp
        src/protocols/receiver_sources.cpp
        src/core/receiver_identity.cpp
        src/protocols/airplay/source.cpp
        src/protocols/cast/source.cpp
        src/protocols/wfd/source.cpp
        src/crypto/aes_ctr.cpp
        src/crypto/fairplay_pairing.cpp
        src/crypto/fairplay_sap.cpp
        src/crypto/srp6a.cpp
        src/crypto/tlv8_chacha.cpp
    )
    target_include_directories(mirage_receiver_sources_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_receiver_sources_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_receiver_sources_test PRIVATE ${MIRAGE_LINK_FLAGS})
    target_link_libraries(mirage_receiver_sources_test PRIVATE OpenSSL::Crypto)
    add_test(NAME receiver_sources COMMAND mirage_receiver_sources_test)

    add_executable(mirage_receiver_identity_test
        tests/test_receiver_identity.cpp
        src/core/receiver_identity.cpp
        src/core/error.cpp
        src/discovery/service_records.cpp
        src/crypto/aes_ctr.cpp
        src/crypto/fairplay_sap.cpp
    )
    target_include_directories(mirage_receiver_identity_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_receiver_identity_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_receiver_identity_test PRIVATE ${MIRAGE_LINK_FLAGS})
    target_link_libraries(mirage_receiver_identity_test PRIVATE OpenSSL::Crypto)
    add_test(NAME receiver_identity COMMAND mirage_receiver_identity_test)

    add_executable(mirage_receiver_session_test
        tests/test_receiver_session.cpp
        src/core/receiver_adapter.cpp
    )
    target_include_directories(mirage_receiver_session_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_receiver_session_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_receiver_session_test PRIVATE ${MIRAGE_LINK_FLAGS})
    add_test(NAME receiver_session COMMAND mirage_receiver_session_test)

    add_executable(mirage_media_pipeline_test
        tests/test_media_pipeline.cpp
    )
    target_include_directories(mirage_media_pipeline_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_media_pipeline_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_media_pipeline_test PRIVATE ${MIRAGE_LINK_FLAGS})
    add_test(NAME media_pipeline COMMAND mirage_media_pipeline_test)

    add_executable(mirage_airplay_media_source_test
        tests/test_airplay_media_source.cpp
        src/protocols/airplay/media_source.cpp
        src/crypto/aes_ctr.cpp
        src/crypto/fairplay_sap.cpp
    )
    target_include_directories(mirage_airplay_media_source_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(mirage_airplay_media_source_test PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(mirage_airplay_media_source_test PRIVATE ${MIRAGE_LINK_FLAGS})
    target_link_libraries(mirage_airplay_media_source_test PRIVATE OpenSSL::Crypto)
    add_test(NAME airplay_media_source COMMAND mirage_airplay_media_source_test)

    if(UNIX)
        add_test(NAME raop_smoke
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/smoke_raop.sh $<TARGET_FILE:mirage>)
        add_test(NAME cast_probe_smoke
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/smoke_cast_probe.sh $<TARGET_FILE:mirage>)
    endif()
endif()
