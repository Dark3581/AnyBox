add_library(d3d12_x_store STATIC store/ShaderStore.cpp)
target_include_directories(d3d12_x_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/store PRIVATE ${ANYBOX_LIBS_INCLUDE})
target_compile_definitions(d3d12_x_store PRIVATE ABX_LIBRARY_NAME="d3d12_x")
target_link_libraries(d3d12_x_store PUBLIC abx_runtime bcrypt)

anybox_add_xdll(d3d12_x SOURCES Inventory.cpp RootSignature.cpp LINK d3d12_x_store)
