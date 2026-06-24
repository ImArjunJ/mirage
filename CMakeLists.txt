cmake_minimum_required(VERSION 3.25)
project(mirage VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

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
set(MIRAGE_INSTALL_SHADER_DIR ${CMAKE_INSTALL_DATADIR}/mirage/shaders)
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

add_custom_target(mirage_shaders DEPENDS ${SHADER_OUTPUTS})

set(MIRAGE_CORE_SOURCES
    src/core/error.cpp
    src/core/cli_options.cpp
    src/core/config.cpp
    src/core/doctor_checks.cpp
    src/core/port_probe.cpp
    src/core/receiver_identity.cpp
    src/core/receiver_adapter.cpp
    src/core/receiver_source.cpp
    src/core/runtime_assets.cpp
    src/core/runtime_paths.cpp
    src/core/runtime_process.cpp
    src/core/runtime_status.cpp
    src/core/sanitizer_suppressions.cpp
    src/core/status_report.cpp
)

set(MIRAGE_CRYPTO_SOURCES
    src/crypto/ed25519_keypair.cpp
    src/crypto/fairplay_pairing.cpp
    src/crypto/fairplay_sap.cpp
    src/crypto/hash.cpp
    src/crypto/srp6a.cpp
    src/crypto/aes_ctr.cpp
    src/crypto/tlv8_chacha.cpp
)

set(MIRAGE_DISCOVERY_SOURCES
    src/discovery/mdns_broadcaster.cpp
    src/discovery/service_records.cpp
    src/discovery/interface_enum.cpp
)

set(MIRAGE_MEDIA_SOURCES
    src/media/local_media_sink.cpp
    src/media/url_media_player.cpp
    src/media/vaapi_decoder.cpp
    src/media/nvdec_decoder.cpp
    src/media/ffmpeg_decoder.cpp
    src/audio/audio_player.cpp
    src/audio/audio_decoder.cpp
    src/render/vulkan_renderer.cpp
)

set(MIRAGE_PROTOCOL_SOURCES
    src/protocols/rtsp_server.cpp
    src/protocols/rtsp_message.cpp
    src/protocols/cast_channel.cpp
    src/protocols/cast/framing.cpp
    src/protocols/cast/message.cpp
    src/protocols/cast/probe.cpp
    src/protocols/cast/tls.cpp
    src/protocols/wfd/control.cpp
    src/protocols/wfd_session.cpp
    src/protocols/receiver_sources.cpp
    src/protocols/airplay/source.cpp
    src/protocols/airplay/media_source.cpp
    src/protocols/airplay/media_metadata.cpp
    src/protocols/airplay/rtsp_state.cpp
    src/protocols/airplay/receiver_session.cpp
    src/protocols/cast/source.cpp
    src/protocols/cast/receiver_session.cpp
    src/protocols/wfd/source.cpp
    src/protocols/wfd/receiver_session.cpp
)

set(MIRAGE_IO_SOURCES
    src/io/tcp.cpp
    src/io/udp.cpp
)

set(MIRAGE_PLATFORM_SOURCES
    src/platform/window.cpp
    src/platform/audio_out.cpp
)

function(mirage_apply_target_defaults target)
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${target} PRIVATE ${MIRAGE_CXX_FLAGS})
    target_link_options(${target} PRIVATE ${MIRAGE_LINK_FLAGS})
endfunction()

add_executable(mirage
    src/main.cpp
    ${MIRAGE_CORE_SOURCES}
    ${MIRAGE_CRYPTO_SOURCES}
    ${MIRAGE_DISCOVERY_SOURCES}
    ${MIRAGE_MEDIA_SOURCES}
    ${MIRAGE_PROTOCOL_SOURCES}
    ${MIRAGE_IO_SOURCES}
    ${MIRAGE_PLATFORM_SOURCES}
    ${SHADER_OUTPUTS}
)
add_dependencies(mirage mirage_shaders)

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

mirage_apply_target_defaults(mirage)
if(EXISTS /opt/cuda/include)
    target_include_directories(mirage SYSTEM PRIVATE /opt/cuda/include)
endif()
target_compile_definitions(mirage PRIVATE
    MIRAGE_BUILD_SHADER_DIR="${SHADER_OUT}"
    MIRAGE_INSTALL_SHADER_DIR="${MIRAGE_INSTALL_SHADER_DIR}"
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

if(WIN32)
    install(TARGETS mirage
        RUNTIME_DEPENDENCY_SET mirage_runtime_dependencies
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(RUNTIME_DEPENDENCY_SET mirage_runtime_dependencies
        PRE_EXCLUDE_REGEXES
            "api-ms-.*"
            "ext-ms-.*"
        POST_EXCLUDE_REGEXES
            ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\][Ss]ystem32[/\\\\].*\\.dll"
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
else()
    install(TARGETS mirage RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES ${SHADER_OUTPUTS} DESTINATION ${MIRAGE_INSTALL_SHADER_DIR})
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES docs/user-guide.md DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(PROGRAMS packaging/install.sh DESTINATION .)
install(FILES packaging/install.ps1 DESTINATION .)

set(CPACK_PACKAGE_NAME "mirage")
set(CPACK_PACKAGE_VENDOR "mirage")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "cross-platform receiver")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_FILE_NAME
    "${PROJECT_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)

include(CTest)
if(BUILD_TESTING)
    function(mirage_add_unit_test test_name target)
        add_executable(${target} ${ARGN})
        mirage_apply_target_defaults(${target})
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    mirage_add_unit_test(srp6a mirage_srp6a_test
        tests/test_srp6a.cpp
        src/crypto/srp6a.cpp
    )
    target_link_libraries(mirage_srp6a_test PRIVATE OpenSSL::Crypto)

    mirage_add_unit_test(fairplay_sap mirage_fairplay_sap_test
        tests/test_fairplay_sap.cpp
        src/crypto/aes_ctr.cpp
        src/crypto/fairplay_sap.cpp
        src/crypto/hash.cpp
    )
    target_link_libraries(mirage_fairplay_sap_test PRIVATE OpenSSL::Crypto)

    mirage_add_unit_test(airplay_protocol mirage_airplay_protocol_test
        tests/test_airplay_protocol.cpp
    )

    mirage_add_unit_test(airplay_media_metadata mirage_airplay_media_metadata_test
        tests/test_airplay_media_metadata.cpp
        src/protocols/airplay/media_metadata.cpp
    )

    mirage_add_unit_test(airplay_rtsp_state mirage_airplay_rtsp_state_test
        tests/test_airplay_rtsp_state.cpp
        src/protocols/airplay/rtsp_state.cpp
    )

    mirage_add_unit_test(cast_probe mirage_cast_probe_test
        tests/test_cast_probe.cpp
        src/protocols/cast/framing.cpp
        src/protocols/cast/probe.cpp
    )

    mirage_add_unit_test(rtsp_message mirage_rtsp_message_test
        tests/test_rtsp_message.cpp
        src/protocols/rtsp_message.cpp
    )

    mirage_add_unit_test(cast_framing mirage_cast_framing_test
        tests/test_cast_framing.cpp
        src/protocols/cast/framing.cpp
        src/protocols/cast/probe.cpp
    )

    mirage_add_unit_test(cast_message mirage_cast_message_test
        tests/test_cast_message.cpp
        src/protocols/cast/message.cpp
    )

    mirage_add_unit_test(wfd_control mirage_wfd_control_test
        tests/test_wfd_control.cpp
        src/protocols/wfd/control.cpp
        src/protocols/rtsp_message.cpp
    )

    mirage_add_unit_test(receiver_adapter mirage_receiver_adapter_test
        tests/test_receiver_adapter.cpp
        src/core/receiver_adapter.cpp
        src/core/receiver_source.cpp
    )

    mirage_add_unit_test(status_report mirage_status_report_test
        tests/test_status_report.cpp
        src/core/receiver_adapter.cpp
        src/core/status_report.cpp
    )

    mirage_add_unit_test(config mirage_config_test
        tests/test_config.cpp
        src/core/config.cpp
    )

    mirage_add_unit_test(cli_options mirage_cli_options_test
        tests/test_cli_options.cpp
        src/core/cli_options.cpp
        src/core/config.cpp
    )

    mirage_add_unit_test(runtime_paths mirage_runtime_paths_test
        tests/test_runtime_paths.cpp
        src/core/runtime_paths.cpp
    )

    mirage_add_unit_test(runtime_process mirage_runtime_process_test
        tests/test_runtime_process.cpp
        src/core/runtime_process.cpp
    )

    mirage_add_unit_test(doctor_checks mirage_doctor_checks_test
        tests/test_doctor_checks.cpp
        src/core/doctor_checks.cpp
        src/core/runtime_assets.cpp
    )
    add_dependencies(mirage_doctor_checks_test mirage_shaders)
    target_compile_definitions(mirage_doctor_checks_test PRIVATE
        MIRAGE_BUILD_SHADER_DIR="${SHADER_OUT}"
        MIRAGE_INSTALL_SHADER_DIR="${MIRAGE_INSTALL_SHADER_DIR}"
    )
    target_link_libraries(mirage_doctor_checks_test PRIVATE
        OpenSSL::Crypto
        Vulkan::Vulkan
        PkgConfig::FFMPEG
    )

    mirage_add_unit_test(runtime_assets mirage_runtime_assets_test
        tests/test_runtime_assets.cpp
        src/core/runtime_assets.cpp
    )
    add_dependencies(mirage_runtime_assets_test mirage_shaders)
    target_compile_definitions(mirage_runtime_assets_test PRIVATE
        MIRAGE_BUILD_SHADER_DIR="${SHADER_OUT}"
        MIRAGE_INSTALL_SHADER_DIR="${MIRAGE_INSTALL_SHADER_DIR}"
    )

    mirage_add_unit_test(port_probe mirage_port_probe_test
        tests/test_port_probe.cpp
        src/core/port_probe.cpp
    )
    if(WIN32)
        target_link_libraries(mirage_port_probe_test PRIVATE ws2_32)
    endif()

    mirage_add_unit_test(runtime_status mirage_runtime_status_test
        tests/test_runtime_status.cpp
        src/core/runtime_process.cpp
        src/core/runtime_status.cpp
        src/core/status_report.cpp
        src/core/receiver_adapter.cpp
        src/core/receiver_source.cpp
    )

    mirage_add_unit_test(receiver_sources mirage_receiver_sources_test
        tests/test_receiver_sources.cpp
        src/core/error.cpp
        src/protocols/receiver_sources.cpp
        src/core/receiver_source.cpp
        src/core/receiver_identity.cpp
        src/protocols/airplay/source.cpp
        src/protocols/cast/source.cpp
        src/protocols/wfd/source.cpp
        src/crypto/ed25519_keypair.cpp
        src/crypto/hash.cpp
    )
    target_link_libraries(mirage_receiver_sources_test PRIVATE OpenSSL::Crypto)

    mirage_add_unit_test(receiver_identity mirage_receiver_identity_test
        tests/test_receiver_identity.cpp
        src/core/receiver_identity.cpp
        src/core/error.cpp
        src/discovery/service_records.cpp
        src/crypto/ed25519_keypair.cpp
        src/crypto/hash.cpp
    )
    target_link_libraries(mirage_receiver_identity_test PRIVATE OpenSSL::Crypto)

    mirage_add_unit_test(receiver_session mirage_receiver_session_test
        tests/test_receiver_session.cpp
        src/core/receiver_adapter.cpp
    )

    mirage_add_unit_test(media_pipeline mirage_media_pipeline_test
        tests/test_media_pipeline.cpp
    )

    mirage_add_unit_test(airplay_media_source mirage_airplay_media_source_test
        tests/test_airplay_media_source.cpp
        src/protocols/airplay/media_source.cpp
        src/crypto/aes_ctr.cpp
        src/crypto/fairplay_sap.cpp
        src/crypto/hash.cpp
    )
    target_link_libraries(mirage_airplay_media_source_test PRIVATE OpenSSL::Crypto)

    if(UNIX)
        add_test(NAME raop_smoke
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/smoke_raop.sh $<TARGET_FILE:mirage>)
        add_test(NAME cast_probe_smoke
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/smoke_cast_probe.sh $<TARGET_FILE:mirage>)
        add_test(NAME wfd_probe_smoke
            COMMAND bash ${CMAKE_SOURCE_DIR}/tests/smoke_wfd_probe.sh $<TARGET_FILE:mirage>)
    endif()

    add_test(NAME install_smoke
        COMMAND ${CMAKE_COMMAND}
            -Dbuild_dir=${CMAKE_BINARY_DIR}
            -Dinstall_prefix=${CMAKE_BINARY_DIR}/install-smoke
            -Dinstall_config=$<CONFIG>
            -Dexecutable_name=$<TARGET_FILE_NAME:mirage>
            -P ${CMAKE_SOURCE_DIR}/tests/install_smoke.cmake
    )

    add_test(NAME package_smoke
        COMMAND ${CMAKE_COMMAND}
            -Dbuild_dir=${CMAKE_BINARY_DIR}
            -Dpackage_dir=${CMAKE_BINARY_DIR}/package-smoke
            -Dinstall_config=$<CONFIG>
            -Dexecutable_name=$<TARGET_FILE_NAME:mirage>
            -P ${CMAKE_SOURCE_DIR}/tests/package_smoke.cmake
    )
endif()
