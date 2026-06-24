if(NOT DEFINED build_dir)
    message(FATAL_ERROR "build_dir is required")
endif()
if(NOT DEFINED install_prefix)
    message(FATAL_ERROR "install_prefix is required")
endif()
if(NOT DEFINED executable_name)
    message(FATAL_ERROR "executable_name is required")
endif()

file(REMOVE_RECURSE "${install_prefix}")

set(install_command "${CMAKE_COMMAND}" --install "${build_dir}" --prefix "${install_prefix}")
if(DEFINED install_config AND NOT install_config STREQUAL "")
    list(APPEND install_command --config "${install_config}")
endif()

execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "cmake install failed (${install_result})\n${install_stdout}\n${install_stderr}")
endif()

set(installed_binary "${install_prefix}/bin/${executable_name}")
set(vertex_shader "${install_prefix}/share/mirage/shaders/nv12_to_rgb.vert.spv")
set(fragment_shader "${install_prefix}/share/mirage/shaders/nv12_to_rgb.frag.spv")

if(NOT EXISTS "${installed_binary}")
    message(FATAL_ERROR "installed binary missing: ${installed_binary}")
endif()
if(NOT EXISTS "${vertex_shader}")
    message(FATAL_ERROR "installed vertex shader missing: ${vertex_shader}")
endif()
if(NOT EXISTS "${fragment_shader}")
    message(FATAL_ERROR "installed fragment shader missing: ${fragment_shader}")
endif()

set(smoke_state "${install_prefix}/state")
set(smoke_config "${install_prefix}/config")
file(MAKE_DIRECTORY "${smoke_state}" "${smoke_config}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
            "XDG_CONFIG_HOME=${smoke_config}"
            "XDG_STATE_HOME=${smoke_state}"
            "${installed_binary}" doctor --no-mdns --port 0
    RESULT_VARIABLE doctor_result
    OUTPUT_VARIABLE doctor_stdout
    ERROR_VARIABLE doctor_stderr
)
if(NOT doctor_result EQUAL 0)
    message(FATAL_ERROR
        "installed doctor failed (${doctor_result})\n${doctor_stdout}\n${doctor_stderr}")
endif()

string(FIND "${doctor_stdout}" "assets:" assets_pos)
if(assets_pos EQUAL -1)
    message(FATAL_ERROR "installed doctor did not print assets section\n${doctor_stdout}")
endif()

string(FIND "${doctor_stdout}" "shaders:" shaders_pos)
if(shaders_pos EQUAL -1)
    message(FATAL_ERROR "installed doctor did not print shader check\n${doctor_stdout}")
endif()

string(FIND "${doctor_stdout}" "result: ready" ready_pos)
if(ready_pos EQUAL -1)
    message(FATAL_ERROR "installed doctor did not report ready\n${doctor_stdout}")
endif()
