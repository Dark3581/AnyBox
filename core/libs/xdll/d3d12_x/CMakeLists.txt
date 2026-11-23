add_library(d3d12_x_store STATIC store/ShaderStore.cpp)
target_include_directories(d3d12_x_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/store PRIVATE ${ANYBOX_LIBS_INCLUDE})
target_compile_definitions(d3d12_x_store PRIVATE ABX_LIBRARY_NAME="d3d12_x")
target_link_libraries(d3d12_x_store PUBLIC abx_runtime bcrypt)

add_library(d3d12_x_vulkan STATIC vulkan/VulkanBackend.cpp vulkan/VulkanDevice.cpp vulkan/TitleLayout.cpp vulkan/PipelineCompiler.cpp vulkan/DescriptorTable.cpp vulkan/BindlessSet.cpp vulkan/TitleBufferWindow.cpp vulkan/PipelineLibrary.cpp vulkan/GraphicsPipeline.cpp vulkan/BufferArena.cpp)
target_include_directories(d3d12_x_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vulkan)
target_link_libraries(d3d12_x_vulkan PUBLIC Vulkan::Headers)

add_library(d3d12_x_pso STATIC pso/PipelineState.cpp pso/GraphicsState.cpp pso/VertexInput.cpp pso/RootSignatureLayout.cpp commands/ResourceState.cpp commands/CopyLayout.cpp commands/CommandRecorder.cpp commands/IndirectCommands.cpp resources/TextureDesc.cpp resources/ViewDesc.cpp resources/BufferDescriptor.cpp objects/Fence.cpp objects/CommandQueue.cpp objects/Resource.cpp objects/CommandAllocator.cpp objects/CommandList.cpp)
target_include_directories(d3d12_x_pso PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pso ${CMAKE_CURRENT_SOURCE_DIR}/commands ${CMAKE_CURRENT_SOURCE_DIR}/resources ${CMAKE_CURRENT_SOURCE_DIR}/objects PRIVATE ${ANYBOX_LIBS_INCLUDE})
target_compile_definitions(d3d12_x_pso PRIVATE ABX_LIBRARY_NAME="d3d12_x" PUBLIC $<$<CXX_COMPILER_ID:GNU>:WIDL_EXPLICIT_AGGREGATE_RETURNS>)
target_link_libraries(d3d12_x_pso PUBLIC d3d12_x_store d3d12_x_vulkan)

anybox_add_xdll(d3d12_x SOURCES Inventory.cpp RootSignature.cpp LINK d3d12_x_pso d3d12_x_store d3d12_x_vulkan)
