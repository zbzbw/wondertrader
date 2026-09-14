#include "gtest/gtest/gtest.h"
#include "../WtDataStorage/WriterDrainSync.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <thread>

namespace
{
void require_ready(std::future<void>& future)
{
	if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
		std::abort();
}
}

TEST(test_writer_drain_sync, idle_waiter_observes_stop_after_false_predicate)
{
	WriterDrainSync drain;
	StdUniqueMutex mutex;
	StdCondVariable condition;
	std::promise<void> predicate_checked;
	std::future<void> checked = predicate_checked.get_future();
	std::promise<void> waiter_done;
	std::future<void> done = waiter_done.get_future();
	std::atomic<bool> release_predicate(false);
	std::atomic<bool> announced(false);

	std::thread waiter([&]() {
		StdUniqueLock lock(mutex);
		condition.wait(lock, [&]() {
			if (!drain.terminated() && !announced.exchange(true))
			{
				predicate_checked.set_value();
				while (!release_predicate.load())
					std::this_thread::yield();
			}
			return drain.terminated();
		});
		waiter_done.set_value();
	});

	require_ready(checked);
	std::thread stopper([&]() { drain.terminate(mutex, condition); });
	release_predicate.store(true);
	stopper.join();
	require_ready(done);
	waiter.join();
	EXPECT_TRUE(drain.terminated());
}

TEST(test_writer_drain_sync, final_synchronous_completion_wakes_drain_waiter)
{
	WriterDrainSync drain;
	StdUniqueMutex mutex;
	StdCondVariable condition;
	std::atomic<uint64_t> received_offset(1);
	std::atomic<uint64_t> completed_offset(0);
	std::atomic<uint64_t> persisted_offset(0);
	std::atomic<bool> processing_failed(false);
	std::promise<void> predicate_checked;
	std::future<void> checked = predicate_checked.get_future();
	std::promise<void> waiter_done;
	std::future<void> done = waiter_done.get_future();
	std::atomic<bool> release_predicate(false);
	std::atomic<bool> announced(false);

	std::thread waiter([&]() {
		StdUniqueLock lock(mutex);
		condition.wait(lock, [&]() {
			if (completed_offset.load() != received_offset.load()
				&& !announced.exchange(true))
			{
				predicate_checked.set_value();
				while (!release_predicate.load())
					std::this_thread::yield();
			}
			return completed_offset.load() == received_offset.load();
		});
		waiter_done.set_value();
	});

	require_ready(checked);
	std::thread completer([&]() {
		drain.complete(
			mutex,
			condition,
			completed_offset,
			persisted_offset,
			processing_failed,
			true);
	});
	release_predicate.store(true);
	completer.join();
	require_ready(done);
	waiter.join();
	EXPECT_EQ(completed_offset.load(), 1U);
	EXPECT_EQ(persisted_offset.load(), 1U);
	EXPECT_FALSE(processing_failed.load());
}

TEST(test_writer_drain_sync, rejected_completion_is_not_persisted)
{
	WriterDrainSync drain;
	StdUniqueMutex mutex;
	StdCondVariable condition;
	std::atomic<uint64_t> completed_offset(0);
	std::atomic<uint64_t> persisted_offset(0);
	std::atomic<bool> processing_failed(false);

	drain.complete(
		mutex,
		condition,
		completed_offset,
		persisted_offset,
		processing_failed,
		false);

	EXPECT_EQ(completed_offset.load(), 1U);
	EXPECT_EQ(persisted_offset.load(), 0U);
	EXPECT_TRUE(processing_failed.load());
}
