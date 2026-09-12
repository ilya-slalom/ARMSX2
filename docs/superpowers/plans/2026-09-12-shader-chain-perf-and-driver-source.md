# Shader-Chain Post-Process Performance + Balemuni Driver Source Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the emulator-side waste around the librashader post-process chain on Android (unpresented frames, per-frame texture churn, mid-frame submits, per-frame framebuffer creation) without changing any presented pixel, and list Balemuni's Aurora Turnip packs in the in-app GPU driver downloader.

**Architecture:** GSRenderer::VSync already knows before Merge() whether a frame will be presented; the skip decision is lifted into a constexpr policy header and used to route every unpresented frame through the existing cheap Merge modes. GSDevice gains two dedicated chain textures so m_merge/m_target_tmp stop being resized every frame. GSDeviceVK tracks which command buffer recorded the last chain frame and submits mid-frame only when a second chain frame would land in the same buffer, and enables VK_KHR_dynamic_rendering so librashader can skip per-pass framebuffer objects. The Kotlin driver list gains one more GitHub-releases source.

**Tech Stack:** C++20 (PCSX2 GS core), Vulkan, librashader C ABI (`filter_chain_vk_opt_t`, API version 5), Kotlin (Android app), GoogleTest via `tests/ctest/core/GS`.

**Spec:** The analysis delivered in-session on 2026-09-12 (optimizations 1-4) and the user's request to add `https://github.com/Balemuni/Balemunis-Aurora/releases` as a driver source.

## Global Constraints

- Presented pixels must not change: no shader edits, no filter changes, no resolution changes.
- Android arm64-v8a is the only target that must build and run; desktop builds must still compile (the changed code is shared).
- librashader shader `FrameCount` parity must be preserved when the chain is skipped on unpresented frames: the counter advances once per VSync, not once per chain run.
- Keep `GSGetPresentCapRenderSkip()` / `AgePoolAfterPresentCapSkip()` semantics untouched (out of scope).
- Follow existing file conventions: policy headers in `pcsx2/GS/Renderers/Common/*Policy.h` with `static_assert` cases, gtest files under `tests/ctest/core/GS/`.
- Commit after each task; commit message text is shown in chat before committing (user preference).

---

### Task 1: Composition-skip policy header + test (optimization 1, part A)

**Files:**
- Create: `pcsx2/GS/Renderers/Common/GSCompositionSkipPolicy.h`
- Create: `tests/ctest/core/GS/gs_composition_skip_policy_tests.cpp`
- Modify: `tests/ctest/core/GS/CMakeLists.txt` (add the test file to `gs_vertex_tests`)

**Interfaces:**
- Produces: `constexpr bool ShouldSkipUnpresentedComposition(bool frame_will_not_be_presented, bool hardware_renderer, bool feedback_write_active, bool image_consumer_active, bool has_current_output)`

- [ ] **Step 1: Write the failing test**

```cpp
// tests/ctest/core/GS/gs_composition_skip_policy_tests.cpp
#include "GS/Renderers/Common/GSCompositionSkipPolicy.h"
#include <gtest/gtest.h>

TEST(GSCompositionSkipPolicy, PresentedFrameNeverSkips)
{
	EXPECT_FALSE(ShouldSkipUnpresentedComposition(false, true, false, false, true));
}

TEST(GSCompositionSkipPolicy, UnpresentedHardwareFrameSkips)
{
	EXPECT_TRUE(ShouldSkipUnpresentedComposition(true, true, false, false, true));
}

TEST(GSCompositionSkipPolicy, SoftwareRendererKeepsFullMerge)
{
	EXPECT_FALSE(ShouldSkipUnpresentedComposition(true, false, false, false, true));
}

TEST(GSCompositionSkipPolicy, FeedbackWriteKeepsFullMerge)
{
	EXPECT_FALSE(ShouldSkipUnpresentedComposition(true, true, true, false, true));
}

TEST(GSCompositionSkipPolicy, ImageConsumerKeepsFullMerge)
{
	EXPECT_FALSE(ShouldSkipUnpresentedComposition(true, true, false, true, true));
}

TEST(GSCompositionSkipPolicy, NoPriorOutputKeepsFullMerge)
{
	EXPECT_FALSE(ShouldSkipUnpresentedComposition(true, true, false, false, false));
}
```

- [ ] **Step 2: Verify it fails** — `clang++ -std=c++20 -fsyntax-only -Ipcsx2 tests/ctest/core/GS/gs_composition_skip_policy_tests.cpp` fails with "file not found".

- [ ] **Step 3: Write the header**

```cpp
// pcsx2/GS/Renderers/Common/GSCompositionSkipPolicy.h
#pragma once

// A frame VSync has already decided not to present (duplicate-frame skip, manual frame skip, or
// the presentation cap) still runs the full display merge, deinterlace, ShadeBoost, FXAA and the
// RetroArch shader chain — 8-10 ms of GPU for an 18-pass CRT preset on an Adreno 740 — for an
// image nobody sees. Those passes can be omitted when nothing else consumes the merged frame:
//  * hardware renderer only (the software renderer's Merge does its own readback bookkeeping),
//  * no EXTWRITE feedback (the merge circuit writes back into GS memory, which is emulated state),
//  * no screenshot / GS dump / video capture pending (they read the composed frame),
//  * a previous presented frame exists, so pause/resize re-presents something sensible.
// Interlaced frames must still update deinterlace history; the caller picks InterlaceHistoryOnly
// versus SkipFinalComposition for that.
constexpr bool ShouldSkipUnpresentedComposition(bool frame_will_not_be_presented,
	bool hardware_renderer, bool feedback_write_active, bool image_consumer_active,
	bool has_current_output)
{
	return frame_will_not_be_presented && hardware_renderer && !feedback_write_active &&
	       !image_consumer_active && has_current_output;
}

static_assert(ShouldSkipUnpresentedComposition(true, true, false, false, true));
static_assert(!ShouldSkipUnpresentedComposition(false, true, false, false, true));
static_assert(!ShouldSkipUnpresentedComposition(true, false, false, false, true));
static_assert(!ShouldSkipUnpresentedComposition(true, true, true, false, true));
static_assert(!ShouldSkipUnpresentedComposition(true, true, false, true, true));
static_assert(!ShouldSkipUnpresentedComposition(true, true, false, false, false));
```

- [ ] **Step 4: Add to CMake** — insert `gs_composition_skip_policy_tests.cpp` after `gs_snapshot_policy_tests.cpp` in `tests/ctest/core/GS/CMakeLists.txt`.

- [ ] **Step 5: Verify** — `clang++ -std=c++20 -fsyntax-only -Ipcsx2 -x c++ pcsx2/GS/Renderers/Common/GSCompositionSkipPolicy.h` passes (static_asserts hold). The gtest binary needs a desktop build; not run locally, noted in the commit.

- [ ] **Step 6: Commit** — `GS: composition-skip policy for unpresented frames`

---

### Task 2: Route unpresented frames through the cheap Merge modes and keep the chain frame counter ticking (optimization 1, part B)

**Files:**
- Modify: `pcsx2/GS/Renderers/Common/GSRenderer.cpp:930-960` (VSync merge-mode selection)
- Modify: `pcsx2/GS/Renderers/Common/GSDevice.h` (declare `m_shader_chain_frame_count`, `NoteShaderChainFrameSkipped()`, new `DoApplyShaderChain(sTex, dTex, frame_count)` signature)
- Modify: `pcsx2/GS/Renderers/Common/GSDevice.cpp:1335-1410` (pass and advance the counter)
- Modify: `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h/.cpp`, `pcsx2/GS/Renderers/OpenGL/GSDeviceOGL.h/.cpp` (drop `m_shader_frame_count`, take `frame_count` parameter)

**Interfaces:**
- Consumes: `ShouldSkipUnpresentedComposition(...)` from Task 1.
- Produces: `void GSDevice::NoteShaderChainFrameSkipped()`; `virtual bool DoApplyShaderChain(GSTexture* sTex, GSTexture* dTex, size_t frame_count)`.

- [ ] **Step 1: In VSync replace the `request_skipped_final_render` expression**

```cpp
	// Any frame that will not be presented may omit display composition (see the policy header).
	// fps_cap_present_skip used to be the only trigger, gated on GSGetPresentCapRenderSkip(); that
	// gate stays on the pool-aging choice below but no longer decides composition.
	const bool request_skipped_final_render = ShouldSkipUnpresentedComposition(
		skip_frame,
		GSIsHardwareRenderer(),
		m_regs->EXTWRITE.WRITE != 0,
		!m_snapshot.empty() || m_dump || m_dump_frames != 0 || GSCapture::IsCapturingVideo() ||
			GSConfig.ShouldDump(s_n, g_perfmon.GetFrame()),
		g_gs_device->GetCurrent() != nullptr);
```

- [ ] **Step 2: After the Merge<...> selection, advance the chain counter on skipped composition**

```cpp
	if (skipped_final_render && GSConfig.ShaderChainEnabled && !GSConfig.ShaderChainPreset.empty())
		g_gs_device->NoteShaderChainFrameSkipped();
```

- [ ] **Step 3: GSDevice — counter plumbing**

Header, next to `m_shader_chain_loaded`:
```cpp
	/// FrameCount handed to the chain. Advances once per VSync — including frames whose
	/// composition was skipped (NoteShaderChainFrameSkipped) — so parity-driven effects
	/// (NTSC phase alternation) see exactly the cadence RetroArch gives them.
	size_t m_shader_chain_frame_count = 0;
```
Public: `void NoteShaderChainFrameSkipped() { m_shader_chain_frame_count++; }`
Virtual: `virtual bool DoApplyShaderChain(GSTexture* sTex, GSTexture* dTex, size_t frame_count) { return false; }`

In `ApplyShaderChain`, replace the final block:
```cpp
	const size_t frame_count = m_shader_chain_frame_count++;
	if (!DoApplyShaderChain(sTex, dTex, frame_count))
		return false;
```

- [ ] **Step 4: Backends** — remove `size_t m_shader_frame_count` from both headers, remove the `m_shader_frame_count = 0;` and `m_shader_frame_count++;` lines, change the override signatures, and pass `frame_count` to `libra_vk_filter_chain_frame` / `libra_gl_filter_chain_frame`.

- [ ] **Step 5: Build check** — `cd platforms/android && ./gradlew :app:assembleGithubRelease` (compiles emucore for arm64).

- [ ] **Step 6: Commit** — `GS: skip display composition and the shader chain on frames that will not be presented`

---

### Task 3: Dedicated shader-chain textures (optimization 2)

**Files:**
- Modify: `pcsx2/GS/Renderers/Common/GSDevice.h` (two members)
- Modify: `pcsx2/GS/Renderers/Common/GSDevice.cpp` (`ApplyShaderChain`, `ClearCurrent`)

- [ ] **Step 1: Members**
```cpp
	/// Chain source (native-res downscale) and target (on-screen size). Dedicated rather than
	/// ping-ponged over m_merge/m_target_tmp: those are resized back to internal size by the
	/// next Merge, so sharing them deleted and re-created two full-size VkImages every frame.
	GSTexture* m_shader_chain_source = nullptr;
	GSTexture* m_shader_chain_target = nullptr;
```

- [ ] **Step 2: ApplyShaderChain** — on the release edge also `delete` both and null them. Downscale into `m_shader_chain_source` (via `ResizeRenderTarget(&m_shader_chain_source, ...)`); when no downscale is needed and `m_shader_chain_source` exists, delete it. Chain output into `m_shader_chain_target`. Remove the `nTex`/`dTex` reference-ping-pong lines.

- [ ] **Step 3: ClearCurrent** — delete and null both.

- [ ] **Step 4: Build check + commit** — `GS: give the shader chain its own source/target textures`

---

### Task 4: Conditional mid-frame submit + safe chain teardown (optimization 3)

**Files:**
- Modify: `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h` (replace `m_shader_frame_count` slot with `u64 m_shader_chain_fence_counter = 0`)
- Modify: `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp` (`DoApplyShaderChain`, `DestroyShaderChain`)

- [ ] **Step 1: In DoApplyShaderChain, after `EndRenderPass()`**
```cpp
	// librashader recycles frame N-3's views/framebuffers when frame N is recorded. That is
	// safe as long as no two chain frames share one submit: our NUM_COMMAND_BUFFERS ring waits
	// for the buffer submitted three submits ago before reusing it. A second chain frame in the
	// SAME command buffer (present skipped by the FIFO throttle or a blank) breaks that, so
	// kick the buffer first — and only then. Steady state is one submit per frame again.
	if (m_shader_chain_fence_counter == GetCurrentFenceCounter())
		ExecuteCommandBuffer(false);
	m_shader_chain_fence_counter = GetCurrentFenceCounter();
```
Delete the trailing `ExecuteCommandBuffer(false);` and its comment block.

- [ ] **Step 2: DestroyShaderChain** — before `libra_vk_filter_chain_free`, if `GetCurrentCommandBuffer() != VK_NULL_HANDLE && !m_last_submit_failed`, call `ExecuteCommandBuffer(true)` so nothing in flight still references the chain's images. Reset `m_shader_chain_fence_counter = 0`.

- [ ] **Step 3: Build check + commit** — `GS/VK: submit mid-frame for the shader chain only when a buffer would hold two chain frames`

---

### Task 5: Dynamic rendering for librashader (optimization 4)

**Files:**
- Modify: `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.h` (`OptionalExtensions::vk_khr_dynamic_rendering`)
- Modify: `pcsx2/GS/Renderers/Vulkan/GSDeviceVK.cpp` (`SelectDeviceExtensions`, `CreateDevice` probe + enable, `DoApplyShaderChain` options)

- [ ] **Step 1: Extension** — `m_optional_extensions.vk_khr_dynamic_rendering = SupportsExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, false);`
- [ ] **Step 2: Feature probe/enable** — `VkPhysicalDeviceDynamicRenderingFeaturesKHR probe_dr/ dynamic_rendering_feature`; `keep("VK_KHR_dynamic_rendering", ..., probe_dr.dynamicRendering == VK_TRUE)`; chain into `device_info` with `dynamicRendering = VK_TRUE`.
- [ ] **Step 3: Chain options**
```cpp
		filter_chain_vk_opt_t opt = {};
		opt.version = LIBRASHADER_CURRENT_VERSION;
		opt.frames_in_flight = 0; // default 3, matches NUM_COMMAND_BUFFERS
		opt.force_no_mipmaps = false;
		// librashader resolves the CORE vkCmdBeginRendering name through vkGetDeviceProcAddr and
		// silently falls back to render passes when it is null; check the same thing it does so the
		// log says which path a device actually took.
		opt.use_dynamic_rendering = m_optional_extensions.vk_khr_dynamic_rendering &&
			vkGetDeviceProcAddr(m_device, "vkCmdBeginRendering") != nullptr;
		opt.disable_cache = false;
		... libra_vk_filter_chain_create(&preset, vk, &opt, &chain)
		Console.WriteLn("(GS) librashader: loaded preset '%s' (dynamic rendering %s)", ..., opt.use_dynamic_rendering ? "on" : "off");
```
- [ ] **Step 4: Build check + commit** — `GS/VK: enable VK_KHR_dynamic_rendering and let librashader use it`

---

### Task 6: Balemuni Aurora driver source

**Files:**
- Modify: `platforms/android/app/src/main/java/com/armsx2/CustomDriver.kt:75-98` (append a `DriverSource`)

- [ ] **Step 1: Add**
```kotlin
        // Balemuni/Balemunis-Aurora — "Apex" Mesa 26.3-devel Turnip builds tuned for Snapdragon
        // 8 Gen 2 / Adreno 740 (AYN Thor) plus a universal a7xx pack. Standard adrenotools zips
        // (meta.json + vulkan.freedreno.so at the root); libc needs stop at LIBC_R (API 30).
        DriverSource(
            "Balemuni · Aurora (Apex)",
            "https://api.github.com/repos/Balemuni/Balemunis-Aurora/releases",
            "balemuni",
        ),
```
- [ ] **Step 2: Build check + commit** — `Android: add Balemuni Aurora to the driver downloader`

---

### Task 7: On-device verification

- [ ] Install the built APK on the AYN Thor (`adb install -r`), launch Ace Combat 04, confirm in logcat: `librashader: loaded preset ... (dynamic rendering on|off)`, no validation/crash, PerfLog fps and GS% at 4x chain-on versus the 2026-09-12 baseline (51-53 fps, GS 65-71%).
- [ ] Open the driver downloader and confirm the Balemuni source lists three releases.
