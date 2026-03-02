# Vulkan 3D Vision Stereo – GeForce / Consumer GPU
### `imageArrayLayers=2` quad-buffer demo  ·  Driver ≤ 426.06

---

## What this does

Renders a stereoscopic frame pair using the NVIDIA 3D Vision **beta-driver
stereo path** that is available on **consumer GeForce GPUs** (not just Quadro):

| Eye   | Colour | Vulkan layer |
|-------|--------|--------------|
| Left  | Cyan   | layer 0      |
| Right | Red    | layer 1      |

With 3D Vision glasses active you see cyan in the left eye and red in the
right.  Without glasses the two colours flash alternately at the display's
stereoscopic refresh rate.

---

## How it works

NVIDIA's 3D Vision beta driver 426.06 (and predecessors) exposes a
**Vulkan quad-buffer** by reporting `maxImageArrayLayers ≥ 2` on the
`VkSurfaceKHR`.  Creating the swapchain with

```cpp
VkSwapchainCreateInfoKHR ci{};
ci.imageArrayLayers = 2;   // layer 0 = left, layer 1 = right
```

is the sole "trick."  No proprietary extension (`VK_NVX_*`, D3D stereo API,
OpenGL `WGL_STEREO`, etc.) is needed.  The driver intercepts `vkQueuePresentKHR`
and drives the LCD shutter glasses accordingly.

The app then:
1. Creates a separate `VkImageView` per layer per swapchain image.
2. Creates two `VkFramebuffer`s per swapchain image (one per layer).
3. Records two sequential render-pass instances per command buffer —
   the first clears layer 0 to **cyan**, the second clears layer 1 to **red**.
4. Presents normally.

---

## Prerequisites

| Requirement | Detail |
|-------------|--------|
| **OS** | Windows 10 (or 8.1 / 7 with the Vulkan runtime manually installed) |
| **GPU** | NVIDIA GeForce with 3D Vision hardware support |
| **Driver** | NVIDIA 3D Vision **beta** driver **≤ 426.06** |
| **3D Vision hardware** | 3D Vision kit (IR emitter + glasses) **or** 3D Vision Ready display |
| **Vulkan SDK** | 1.1.114 or newer (only `vulkan/vulkan.h` and `vulkan-1.lib` are used) |

> **Why ≤ 426.06?**  
> NVIDIA discontinued the 3D Vision beta driver branch after 426.06.  Later
> drivers removed the `imageArrayLayers=2` surface capability.

---

## Enabling 3D Vision in the NVIDIA Control Panel

1. Open **NVIDIA Control Panel**.
2. Navigate to **Set up stereoscopic 3D**.
3. Check **Enable stereoscopic 3D**.
4. Set the display to a supported 3D Vision / 3D Vision Ready mode (typically
   120 Hz or 60 Hz per eye).

If this is done correctly, the Vulkan surface reports `maxImageArrayLayers = 2`
and the app prints `STEREO ENABLED – imageArrayLayers = 2` on the console.

---

## Build

### Option A – Visual Studio Developer Command Prompt

```bat
cl /std:c++17 /W3 /O2 main.cpp /I%VULKAN_SDK%\Include ^
   /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib user32.lib ^
   /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup
```

### Option B – CMake + MSVC

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The `VULKAN_SDK` environment variable must point to your Vulkan SDK root
(e.g. `C:\VulkanSDK\1.1.114.0`).

---

## Runtime output

The app allocates a console window alongside the main window and prints:

```
Vulkan instance created (validation ON)
GPU: NVIDIA GeForce RTX 2080 Ti | surface maxImageArrayLayers = 2
STEREO ENABLED  – imageArrayLayers = 2
Init complete. Swapchain 1280x720, 3 images, imageArrayLayers=2
```

If `maxImageArrayLayers` is reported as 1 (stereo not active):

```
GPU: NVIDIA GeForce RTX 2080 Ti | surface maxImageArrayLayers = 1
WARNING: maxImageArrayLayers < 2 on this surface.
  -> Make sure 3D Vision is ENABLED in NVIDIA Control Panel
     and you are running driver <= 426.06.
  -> Falling back to single-layer swapchain (no stereo).
```

---

## File layout

```
vulkan_stereo/
├── main.cpp          ← entire application (~350 lines, no shaders required)
├── CMakeLists.txt    ← CMake build script
└── README.md         ← this file
```

No GLSL/SPIR-V shaders are needed — the demo fills each eye exclusively via
`VK_ATTACHMENT_LOAD_OP_CLEAR` in the render pass, so vertex and fragment
shaders are unnecessary.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `maxImageArrayLayers = 1` | 3D Vision not enabled in Control Panel, or driver > 426.06 |
| Crash in `vkCreateSwapchainKHR` | Driver does not support `imageArrayLayers=2` (see above) |
| Only one colour visible | 3D Vision glasses not syncing — check IR emitter / display mode |
| Both colours flicker equally | Normal behaviour without glasses at 120 Hz stereo mode |
| Black screen | Swap to fullscreen exclusive (Alt+Enter if implemented) for some setups |
