#include "gtest/gtest/gtest.h"
#include "../WtDataStorage/RecordingUnitDrain.hpp"

TEST(test_recording_unit_drain, close_freezes_only_target_session)
{
	RecordingUnitDrain drain;
	std::string first;
	std::string other;
	EXPECT_TRUE(drain.accept("SHFE", "SHFE.cu2609", 20260915, first));
	drain.closeSession("SHFE");

	std::string rejected;
	EXPECT_FALSE(drain.accept("SHFE", "SHFE.cu2609", 20260915, rejected));
	EXPECT_TRUE(drain.accept("CFFEX", "CFFEX.IF2609", 20260915, other));
	EXPECT_FALSE(drain.isDrained("SHFE"));

	drain.complete(first, true);
	EXPECT_TRUE(drain.isDrained("SHFE"));
	EXPECT_FALSE(drain.isDrained("CFFEX"));
}

TEST(test_recording_unit_drain, rejected_processing_never_becomes_persisted)
{
	RecordingUnitDrain drain;
	std::string key;
	ASSERT_TRUE(drain.accept("SHFE", "SHFE.cu2609", 20260915, key));
	drain.complete(key, false);
	drain.closeSession("SHFE");

	auto units = drain.units("SHFE");
	ASSERT_EQ(units.size(), 1U);
	EXPECT_EQ(units[0].accepted, 1U);
	EXPECT_EQ(units[0].completed, 1U);
	EXPECT_EQ(units[0].persisted, 0U);
	EXPECT_TRUE(units[0].failed);
}

TEST(test_recording_unit_drain, next_cycle_reopens_without_reusing_old_counts)
{
	RecordingUnitDrain drain;
	std::string old_key;
	ASSERT_TRUE(drain.accept("SHFE", "SHFE.cu2609", 20260915, old_key));
	drain.complete(old_key, true);
	drain.closeSession("SHFE");
	ASSERT_TRUE(drain.isDrained("SHFE"));

	drain.openSession("SHFE");
	std::string new_key;
	EXPECT_TRUE(drain.accept("SHFE", "SHFE.cu2609", 20260916, new_key));
	EXPECT_NE(old_key, new_key);
	auto units = drain.units("SHFE");
	ASSERT_EQ(units.size(), 1U);
	EXPECT_EQ(units[0].trading_date, 20260916U);
}
