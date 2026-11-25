add_library(d3d12_x_store STATIC store/ShaderStore.cpp)
target_include_directories(d3d12_x_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/store PRIVATE ${ANYBOX_LIBS_INCLUDE})
target_compile_definitions(d3d12_x_store PRIVATE ABX_LIBRARY_NAME="d3d12_x")
target_link_libraries(d3d12_x_store PUBLIC abx_runtime bcrypt)

add_library(d3d12_x_vulkan STATIC vulkan/VulkanBackend.cpp vulkan/VulkanDevice.cpp vulkan/TitleLayout.cpp vulkan/PipelineCompiler.cpp vulkan/DescriptorTable.cpp vulkan/BindlessSet.cpp vulkan/TitleBufferWindow.cpp vulkan/TitleAddressSpace.cpp vulkan/AddressTranslation.cpp vulkan/PipelineLibrary.cpp vulkan/UserDataRebase.cpp vulkan/GraphicsPipeline.cpp vulkan/BufferArena.cpp vulkan/Presenter.cpp)
target_include_directories(d3d12_x_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vulkan)
target_link_libraries(d3d12_x_vulkan PUBLIC Vulkan::Headers)

add_library(gfx10_tiling STATIC resources/Gfx10Tiling.cpp)
target_include_directories(gfx10_tiling PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/resources)
target_compile_definitions(gfx10_tiling PUBLIC $<$<CXX_COMPILER_ID:GNU>:WIDL_EXPLICIT_AGGREGATE_RETURNS>)

add_library(d3d12_x_pso STATIC pso/PipelineState.cpp pso/PipelineStream.cpp pso/GraphicsState.cpp pso/VertexInput.cpp pso/RootSignatureLayout.cpp commands/ResourceState.cpp commands/CopyLayout.cpp commands/CommandRecorder.cpp commands/IndirectCommands.cpp commands/IndirectClamp.cpp resources/TextureDesc.cpp resources/ViewDesc.cpp resources/BufferDescriptor.cpp resources/ImageDescriptor.cpp resources/SamplerDescriptor.cpp resources/DescriptorSizes.cpp objects/Fence.cpp objects/CommandQueue.cpp objects/Resource.cpp objects/CommandAllocator.cpp objects/CommandList.cpp objects/ConsoleFacade.cpp objects/DescriptorHeap.cpp objects/DescriptorWriter.cpp objects/RootSignature.cpp objects/QueryHeap.cpp objects/TitleTextures.cpp objects/CommandSignature.cpp objects/PipelineObject.cpp objects/LiveStats.cpp objects/DiscordPresence.cpp objects/FrameDump.cpp)
target_include_directories(d3d12_x_pso PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/pso ${CMAKE_CURRENT_SOURCE_DIR}/commands ${CMAKE_CURRENT_SOURCE_DIR}/resources ${CMAKE_CURRENT_SOURCE_DIR}/objects PRIVATE ${ANYBOX_LIBS_INCLUDE})
target_compile_definitions(d3d12_x_pso PRIVATE ABX_LIBRARY_NAME="d3d12_x" PUBLIC $<$<CXX_COMPILER_ID:GNU>:WIDL_EXPLICIT_AGGREGATE_RETURNS>)
target_link_libraries(d3d12_x_pso PUBLIC d3d12_x_store d3d12_x_vulkan gfx10_tiling)

anybox_add_xdll(d3d12_x SOURCES Inventory.cpp RootSignature.cpp VirtualMemory.cpp ConsoleDevice.cpp ConsoleObjects.cpp LINK d3d12_x_pso d3d12_x_store d3d12_x_vulkan)
