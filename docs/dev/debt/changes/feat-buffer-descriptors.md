# d3d12_x

## add Functional

- [BufferDescriptor](../../../core/libs/xdll/d3d12_x/resources/BufferDescriptor.cpp) (d3d12_x) - builds the four-word buffer descriptors (V#) the translated shaders read from descriptor heaps and root arguments, with the fields where the translator reads them: base address (48 bits), stride, records, component selects, format and bounds mode, plus resource level 1. Constant buffer and raw views use byte bounds, structured views element-and-stride bounds and typed views index bounds, the modes whose behaviour the translator documents from hardware measurements. The field values a console driver writes for each view type are not known; these are AnyBox's choice, consistent with how the shaders read them. Typed formats without an encoding (and typed loads the translator decodes to 0) are refused or read as 0, views with a counter are refused, and nothing writes these words until the device's view methods and descriptor heaps exist
  Closes when: a title's buffer views are written through it and its shaders read the data they expect
