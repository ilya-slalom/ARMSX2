// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// A frame VSync has already decided not to present (duplicate-frame skip, manual frame skip, or
// the presentation cap) still ran the full display merge, deinterlace, ShadeBoost, FXAA and the
// RetroArch shader chain — 8-10 ms of GPU for an 18-pass CRT preset on an Adreno 740 — for an
// image nobody sees. For a 30 fps title with SkipDuplicateFrames on, that is the whole chain
// twice per displayed frame.
//
// Those passes can be omitted when nothing else consumes the merged frame:
//  * hardware renderer only (the software renderer's Merge does its own readback bookkeeping),
//  * no EXTWRITE feedback (the merge circuit writes back into GS memory, which is emulated state),
//  * no screenshot / GS dump / video capture pending (they read the composed frame),
//  * a previously presented frame exists, so a pause or resize re-presents something sensible.
// Interlaced frames must still update deinterlace history; the caller picks InterlaceHistoryOnly
// versus SkipFinalComposition for that. GetOutput() still runs in both modes, so the texture
// cache sees exactly the display reads it would have seen.
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
