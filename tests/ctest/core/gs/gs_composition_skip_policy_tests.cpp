// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Pins the "this frame will never be presented, so skip composing it" decision GSRenderer::VSync
// makes before Merge(). The policy is constexpr and static_asserts its key cases at its definition;
// this suite names them so a regression reads as a failure rather than a compile error.

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
	// EXTWRITE feeds the merge result back into GS memory: emulated state, never optional.
	EXPECT_FALSE(ShouldSkipUnpresentedComposition(true, true, true, false, true));
}

TEST(GSCompositionSkipPolicy, ImageConsumerKeepsFullMerge)
{
	// Screenshot, GS dump or video capture read the composed frame even when it isn't shown.
	EXPECT_FALSE(ShouldSkipUnpresentedComposition(true, true, false, true, true));
}

TEST(GSCompositionSkipPolicy, NoPriorOutputKeepsFullMerge)
{
	// Without a previous frame there is nothing for a pause/resize re-present to show.
	EXPECT_FALSE(ShouldSkipUnpresentedComposition(true, true, false, false, false));
}
