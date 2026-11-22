# d3d12_x

## add Functional

- [DescriptorTable](../../../core/libs/xdll/d3d12_x/vulkan/DescriptorTable.cpp) (d3d12_x) - builds the descriptor hash table the translated shaders search (word 0 the mask, then ten words per slot: an eight-word T# or S# key, the bindless index, padding; FNV-1a over the key words; eight slots probed from the hash; a miss is index 0). The format is the one the translator's generated lookup reads, checked on a GPU by `anybox-pipeline-probe bindless`, not against a title. The slot comes from the low bits of the hash, which depend only on the low bits of each key word, so keys that differ only in high bits share slots and grow the table. The all-zero key cannot be stored and resolves to index 0
  Closes when: a title's descriptors are entered from its own descriptor writes and its shaders find them
- [BindlessSet](../../../core/libs/xdll/d3d12_x/vulkan/BindlessSet.cpp) (d3d12_x) - allocates the title layout's descriptor set and puts fallbacks at index 0 of every binding: zeroed 1x1 images (`R32_UINT` for the two bindings that declare it, 32-bit four-component float, uint or sint otherwise) and a nearest, clamp-to-edge sampler. What a title expects from a missing descriptor is unknown. The table buffer is host-visible and rewritten in place, and growing it replaces the buffer and rewrites binding 2, which has no update-after-bind flag, so neither may happen while the GPU uses the set
  Closes when: descriptor writes come from a title's calls and updates are ordered against GPU work
