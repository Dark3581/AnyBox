add_library(fake_xgameruntime SHARED FakeXGameRuntime.cpp)
set_target_properties(fake_xgameruntime PROPERTIES PREFIX "" OUTPUT_NAME xgameruntime)
anybox_static_runtime(fake_xgameruntime)

add_executable(fake_title FakeTitle.cpp)
target_link_libraries(fake_title PRIVATE fake_xgameruntime)
anybox_static_runtime(fake_title)

if(Python3_FOUND)
    foreach(scenario IN ITEMS taskqueue stub crash crash_chained crash_recovered device_scarlett device_durango device_pc device_default device_none device_noexecutable device_unknown console_id sandbox_id async gamesave async_null async_terminated async_begin_terminated startup startup_options thread_assert)
        add_test(NAME e2e_${scenario}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e.py
                --relinker $<TARGET_FILE:relinker> --title $<TARGET_FILE:fake_title> --libs ${ANYBOX_LIBS_DIR}
                --work ${CMAKE_CURRENT_BINARY_DIR}/e2e_${scenario} --scenario ${scenario})
    endforeach()
endif()

add_executable(lib_probe LibProbe.cpp)
target_include_directories(lib_probe PRIVATE ${ANYBOX_LIBS_INCLUDE})
add_dependencies(lib_probe ab_xmem ab_d3d12_x)
anybox_static_runtime(lib_probe)

foreach(scenario IN ITEMS xmem rootsig)
    add_test(NAME lib_${scenario} COMMAND lib_probe ${ANYBOX_LIBS_DIR} ${scenario})
    set_tests_properties(lib_${scenario} PROPERTIES ENVIRONMENT ANYBOX_NO_REPORT=1)
endforeach()
foreach(scenario IN ITEMS free_unknown alloc_zero rootsig_version)
    add_test(NAME lib_${scenario} COMMAND lib_probe ${ANYBOX_LIBS_DIR} unsupported ${scenario})
    set_tests_properties(lib_${scenario} PROPERTIES ENVIRONMENT ANYBOX_NO_REPORT=1)
endforeach()

foreach(variant IN ITEMS ok fail stop crash)
    add_library(fake_module_${variant} SHARED FakeModule.cpp)
    set_target_properties(fake_module_${variant} PROPERTIES PREFIX "")
    string(TOUPPER ${variant} upper)
    target_compile_definitions(fake_module_${variant} PRIVATE FAKE_MODULE_${upper})
    anybox_static_runtime(fake_module_${variant})
endforeach()
target_link_libraries(fake_module_stop PRIVATE ab_d3d12_x)

if(Python3_FOUND)
    add_test(NAME module_check
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/module_check_test.py
            --tool ${CMAKE_SOURCE_DIR}/tools/module_check.py --build-dir ${CMAKE_BINARY_DIR} --libs ${ANYBOX_LIBS_DIR}
            --work ${CMAKE_CURRENT_BINARY_DIR}/module_check
            $<TARGET_FILE:fake_module_ok> $<TARGET_FILE:fake_module_fail> $<TARGET_FILE:fake_module_stop> $<TARGET_FILE:fake_module_crash>)
endif()

if(Python3_FOUND)
    add_test(NAME runtime_trace
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/trace_test.py
            --probe $<TARGET_FILE:lib_probe> --libs ${ANYBOX_LIBS_DIR} --work ${CMAKE_CURRENT_BINARY_DIR}/runtime_trace)
endif()

add_executable(shader_store_probe ShaderStoreProbe.cpp)
target_link_libraries(shader_store_probe PRIVATE d3d12_x_store)
target_include_directories(shader_store_probe PRIVATE ${ANYBOX_LIBS_INCLUDE})
anybox_static_runtime(shader_store_probe)

if(Python3_FOUND)
    add_test(NAME shader_store
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shader_store_test.py
            --probe $<TARGET_FILE:shader_store_probe> --libs ${ANYBOX_LIBS_DIR} --tool ${CMAKE_SOURCE_DIR}/tools/shader_store.py --work ${CMAKE_CURRENT_BINARY_DIR}/shader_store)
endif()

add_executable(pso_probe PsoProbe.cpp)
target_link_libraries(pso_probe PRIVATE d3d12_x_pso)
target_include_directories(pso_probe PRIVATE ${ANYBOX_LIBS_INCLUDE} ${CMAKE_SOURCE_DIR}/core/libs/probe)
anybox_static_runtime(pso_probe)

if(Python3_FOUND)
    add_test(NAME pipeline_state
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/pipeline_state_test.py
            --probe $<TARGET_FILE:pso_probe> --libs ${ANYBOX_LIBS_DIR} --tool ${CMAKE_SOURCE_DIR}/tools/shader_store.py --work ${CMAKE_CURRENT_BINARY_DIR}/pipeline_state)
endif()

if(Python3_FOUND)
    add_test(NAME pipeline_probe
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/pipeline_probe_test.py
            --probe $<TARGET_FILE:pipeline_probe> --work ${CMAKE_CURRENT_BINARY_DIR}/pipeline_probe)
    set_tests_properties(pipeline_probe PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP ")
endif()

if(ANYBOX_SHADERS_ENABLED)
    foreach(check IN ITEMS store gpu layout compute pso pipelines)
        add_test(NAME shaders_${check}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shader_build_test.py
                --shaders ${ANYBOX_SHADER_DIR} --probe $<TARGET_FILE:shader_store_probe> --libs ${ANYBOX_LIBS_DIR}
                --pipeline-probe $<TARGET_FILE:pipeline_probe> --pso-probe $<TARGET_FILE:pso_probe> --build-dir ${CMAKE_BINARY_DIR} --check ${check})
        set_tests_properties(shaders_${check} PROPERTIES LABELS shaders TIMEOUT 1800 SKIP_REGULAR_EXPRESSION "SKIP ")
    endforeach()
endif()

if(Python3_FOUND)
    add_test(NAME gpu_report COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/gpu_report_test.py --report $<TARGET_FILE:gpu_report>)
endif()
