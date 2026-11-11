# Vulkan headers

Unmodified C headers from Khronos Vulkan-Headers 1.4.363 (`VK_HEADER_VERSION 363`), copied from
the Arch `vulkan-headers 1:1.4.363.0-1` package. `include/vulkan/*.h` and `include/vk_video/*.h`
only; the C++ `vulkan.hpp` bindings are unused and omitted. Licensed `Apache-2.0 OR MIT`, see
`LICENSE.txt`.

Vendored because an OS update removed the package while keeping `libvulkan.so`, which broke the
build. The top-level `CMakeLists.txt` points `Vulkan_INCLUDE_DIR` here. The loader still comes
from the system. To update, replace these files from a newer release no newer than the installed
loader.
