#include "gtest/gtest/gtest.h"

#include "../WtDtCore/UDPCaster.h"
#include "../Includes/WTSDataDef.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <future>
#include <utility>

class UDPCasterTestPeer
{
public:
	static void set_before_wait(UDPCaster& caster, std::function<void()> hook)
	{
		StdUniqueLock lock(caster.m_mtxCast);
		caster.m_beforeCastWait = std::move(hook);
	}
};

namespace
{
#pragma pack(push, 1)
struct RawTickPacket
{
	uint32_t type;
	WTSTickStruct tick;
};
#pragma pack(pop)

void require_ready(std::future<void>& future)
{
	if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready)
		std::abort();
}
}

TEST(test_udp_caster_stop, idle_wait_and_stop_share_one_predicate_boundary)
{
	boost::asio::io_service receiver_service;
	boost::asio::ip::udp::socket receiver(
		receiver_service,
		boost::asio::ip::udp::endpoint(
			boost::asio::ip::address_v4::loopback(),
			0));

	UDPCaster caster;
	ASSERT_TRUE(caster.addBRecver(
		"127.0.0.1",
		receiver.local_endpoint().port(),
		2));
	caster.start(0);

	std::promise<void> empty_predicate_checked;
	std::future<void> checked = empty_predicate_checked.get_future();
	std::promise<void> release_predicate;
	std::shared_future<void> release = release_predicate.get_future().share();
	std::atomic<bool> announced(false);
	UDPCasterTestPeer::set_before_wait(caster, [&]() {
		if (!announced.exchange(true))
		{
			empty_predicate_checked.set_value();
			release.wait();
		}
	});

	WTSTickStruct value = {};
	std::strcpy(value.exchg, "SHFE");
	std::strcpy(value.code, "cu2610");
	value.price = 81234.5;
	WTSTickData* tick = WTSTickData::create(value);
	caster.broadcast(tick);
	tick->release();

	require_ready(checked);
	RawTickPacket packet = {};
	boost::asio::ip::udp::endpoint sender;
	ASSERT_EQ(receiver.receive_from(boost::asio::buffer(&packet, sizeof(packet)), sender), sizeof(packet));
	EXPECT_DOUBLE_EQ(packet.tick.price, value.price);

	std::promise<void> stop_started;
	std::future<void> started = stop_started.get_future();
	std::future<void> stopped = std::async(std::launch::async, [&]() {
		stop_started.set_value();
		caster.stop();
	});
	require_ready(started);
	release_predicate.set_value();
	require_ready(stopped);
}
