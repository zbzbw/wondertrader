#pragma once

#include "../Share/StdUtils.hpp"

#include <atomic>
#include <cstdint>

class WriterDrainSync
{
public:
	WriterDrainSync() : _terminated(false) {}

	bool terminated() const
	{
		return _terminated.load();
	}

	void terminate(StdUniqueMutex& mutex, StdCondVariable& condition)
	{
		{
			StdUniqueLock lock(mutex);
			_terminated.store(true);
		}
		condition.notify_all();
	}

	void complete(
		StdUniqueMutex& mutex,
		StdCondVariable& condition,
		std::atomic<uint64_t>& completed_offset,
		std::atomic<uint64_t>& persisted_offset,
		std::atomic<bool>& processing_failed,
		bool persisted)
	{
		{
			StdUniqueLock lock(mutex);
			completed_offset.fetch_add(1);
			if (persisted)
				persisted_offset.fetch_add(1);
			else
				processing_failed.store(true);
		}
		condition.notify_all();
	}

private:
	std::atomic<bool> _terminated;
};
