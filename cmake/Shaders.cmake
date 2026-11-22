set(ANYBOX_GAME_PACKAGE "" CACHE PATH "Title package to build shaders from (enables the shaders targets and tests)")
set(ANYBOX_SHADER_TRANSLATOR "" CACHE PATH "Checkout of the external shader translator")
set(ANYBOX_PIPELINE_CHECK "" CACHE FILEPATH "Pipeline checker executable (default: the translator's own build)")
set(ANYBOX_SHADER_JOBS "0" CACHE STRING "Workers for the translator and the pipeline check (0: their default)")
set(ANYBOX_PIPELINE_CHUNK "500" CACHE STRING "Index entries per pipeline checker process (0: one process for all)")
set(ANYBOX_WAVE64_EMULATE "auto" CACHE STRING "The translator's wave64 emulation mode: auto, on or off")
set_property(CACHE ANYBOX_WAVE64_EMULATE PROPERTY STRINGS auto on off)

set(ANYBOX_SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
set(ANYBOX_SHADERS_ENABLED OFF)

if(ANYBOX_GAME_PACKAGE OR ANYBOX_SHADER_TRANSLATOR)
    if(NOT ANYBOX_GAME_PACKAGE OR NOT ANYBOX_SHADER_TRANSLATOR)
        message(FATAL_ERROR "Set both ANYBOX_GAME_PACKAGE and ANYBOX_SHADER_TRANSLATOR to build shaders, or neither.")
    endif()
    if(NOT Python3_FOUND)
        message(FATAL_ERROR "Building shaders needs Python 3.")
    endif()
    if(NOT ANYBOX_PIPELINE_CHUNK MATCHES "^[0-9]+$")
        message(FATAL_ERROR "ANYBOX_PIPELINE_CHUNK must be a whole number.")
    endif()
    if(NOT ANYBOX_WAVE64_EMULATE MATCHES "^(auto|on|off)$")
        message(FATAL_ERROR "ANYBOX_WAVE64_EMULATE must be auto, on or off.")
    endif()
    set(ANYBOX_SHADERS_ENABLED ON)
    set(anybox_shader_args
        --package ${ANYBOX_GAME_PACKAGE} --translator ${ANYBOX_SHADER_TRANSLATOR} --out ${ANYBOX_SHADER_DIR}
        --jobs ${ANYBOX_SHADER_JOBS} --wave64-emulate ${ANYBOX_WAVE64_EMULATE} --pipeline-chunk ${ANYBOX_PIPELINE_CHUNK})
    if(ANYBOX_PIPELINE_CHECK)
        list(APPEND anybox_shader_args --pipeline-check ${ANYBOX_PIPELINE_CHECK})
    endif()
    add_custom_target(shaders
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/build_shaders.py ${anybox_shader_args} --steps translate,store
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL VERBATIM)
    add_custom_target(shader_store
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/build_shaders.py ${anybox_shader_args} --steps store
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL VERBATIM)
    add_custom_target(shader_pipelines
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/build_shaders.py ${anybox_shader_args} --steps pipelines
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} USES_TERMINAL VERBATIM)
    message(STATUS "Shaders: build with the shaders and shader_pipelines targets into ${ANYBOX_SHADER_DIR}, test with ctest -L shaders")
endif()
