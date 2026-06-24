if(NOT DEFINED build_dir)
    message(FATAL_ERROR "build_dir is required")
endif()
if(NOT DEFINED package_dir)
    message(FATAL_ERROR "package_dir is required")
endif()
if(NOT DEFINED executable_name)
    message(FATAL_ERROR "executable_name is required")
endif()

set(cpack_config "${build_dir}/CPackConfig.cmake")
if(NOT EXISTS "${cpack_config}")
    message(FATAL_ERROR "CPack config missing: ${cpack_config}")
endif()

get_filename_component(cmake_bin_dir "${CMAKE_COMMAND}" DIRECTORY)
find_program(CPACK_EXECUTABLE cpack HINTS "${cmake_bin_dir}" REQUIRED)

file(REMOVE_RECURSE "${package_dir}")
file(MAKE_DIRECTORY "${package_dir}")

set(cpack_command "${CPACK_EXECUTABLE}" --config "${cpack_config}" -G ZIP -B "${package_dir}")
if(DEFINED install_config AND NOT install_config STREQUAL "")
    list(APPEND cpack_command -C "${install_config}")
endif()

execute_process(
    COMMAND ${cpack_command}
    RESULT_VARIABLE cpack_result
    OUTPUT_VARIABLE cpack_stdout
    ERROR_VARIABLE cpack_stderr
)
if(NOT cpack_result EQUAL 0)
    message(FATAL_ERROR
        "cpack failed (${cpack_result})\n${cpack_stdout}\n${cpack_stderr}")
endif()

file(GLOB archives "${package_dir}/*.zip")
list(LENGTH archives archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR "expected one package archive in ${package_dir}, found ${archive_count}")
endif()
list(GET archives 0 archive)

set(extract_dir "${package_dir}/extract")
file(MAKE_DIRECTORY "${extract_dir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
    WORKING_DIRECTORY "${extract_dir}"
    RESULT_VARIABLE extract_result
    OUTPUT_VARIABLE extract_stdout
    ERROR_VARIABLE extract_stderr
)
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR
        "package extraction failed (${extract_result})\n${extract_stdout}\n${extract_stderr}")
endif()

file(GLOB package_roots LIST_DIRECTORIES true "${extract_dir}/*")
list(LENGTH package_roots package_root_count)
if(NOT package_root_count EQUAL 1)
    message(FATAL_ERROR "expected one package root, found ${package_root_count}")
endif()
list(GET package_roots 0 package_root)

set(package_binary "${package_root}/bin/${executable_name}")
set(vertex_shader "${package_root}/share/mirage/shaders/nv12_to_rgb.vert.spv")
set(fragment_shader "${package_root}/share/mirage/shaders/nv12_to_rgb.frag.spv")

if(NOT EXISTS "${package_binary}")
    message(FATAL_ERROR "packaged binary missing: ${package_binary}")
endif()
if(NOT EXISTS "${vertex_shader}")
    message(FATAL_ERROR "packaged vertex shader missing: ${vertex_shader}")
endif()
if(NOT EXISTS "${fragment_shader}")
    message(FATAL_ERROR "packaged fragment shader missing: ${fragment_shader}")
endif()

set(smoke_state "${package_dir}/state")
set(smoke_config "${package_dir}/config")
file(MAKE_DIRECTORY "${smoke_state}" "${smoke_config}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "XDG_CONFIG_HOME=${smoke_config}"
            "XDG_STATE_HOME=${smoke_state}"
            "${package_binary}" doctor --no-mdns --port 0
    RESULT_VARIABLE doctor_result
    OUTPUT_VARIABLE doctor_stdout
    ERROR_VARIABLE doctor_stderr
)
if(NOT doctor_result EQUAL 0)
    message(FATAL_ERROR
        "packaged doctor failed (${doctor_result})\n${doctor_stdout}\n${doctor_stderr}")
endif()

string(FIND "${doctor_stdout}" "assets:" assets_pos)
if(assets_pos EQUAL -1)
    message(FATAL_ERROR "packaged doctor did not print assets section\n${doctor_stdout}")
endif()

string(FIND "${doctor_stdout}" "shaders:" shaders_pos)
if(shaders_pos EQUAL -1)
    message(FATAL_ERROR "packaged doctor did not print shader check\n${doctor_stdout}")
endif()

string(FIND "${doctor_stdout}" "result: ready" ready_pos)
if(ready_pos EQUAL -1)
    message(FATAL_ERROR "packaged doctor did not report ready\n${doctor_stdout}")
endif()
