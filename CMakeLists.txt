cmake_minimum_required(VERSION 3.15)
project(VulkanStereo3DVision CXX)

set(CMAKE_CXX_STANDARD 17)

# ── Vulkan ───────────────────────────────────────────────────────────────────
find_package(Vulkan REQUIRED)

# ── executable ───────────────────────────────────────────────────────────────
add_executable(VulkanStereo WIN32 main.cpp)

target_include_directories(VulkanStereo PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(VulkanStereo PRIVATE ${Vulkan_LIBRARIES} user32)

# ── MSVC flags ───────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(VulkanStereo PRIVATE /W3 /O2)
endif()
