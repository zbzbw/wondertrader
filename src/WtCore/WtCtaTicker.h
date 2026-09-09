/*!
 * \file WtCtaTicker.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include <stdint.h>
#include <atomic>
#include <string>

#include "../Includes/WTSMarcos.h"
#include "../Share/StdUtils.hpp"

NS_WTP_BEGIN
class WTSSessionInfo;
class IDataReader;
class WTSTickData;

class WtCtaEngine;
//////////////////////////////////////////////////////////////////////////
//生产时间步进器
class WtCtaRtTicker
{
public:
	WtCtaRtTicker(WtCtaEngine* engine) 
		: _engine(engine)
		, _stopped(false)
		, _date(0)
		, _time(UINT_MAX)
		, _next_check_time(0)
		, _last_emit_pos(0)
		, _cur_pos(0){}
	~WtCtaRtTicker(){}

public:
	void	init(IDataReader* store, const char* sessionID);
	//void	set_time(uint32_t uDate, uint32_t uTime);
	void	on_tick(WTSTickData* curTick);
	void initControlled(IDataReader* store, const char* sessionID);
	void startControlled(uint32_t date, uint32_t time, uint32_t tradingDay, uint64_t event_ms, bool initialize = true);
	void stepControlled(uint32_t date, uint32_t time, uint64_t event_ms);
	std::string liveSnapshot() const;
	void restoreLive(const std::string& state);

	void	run();
	void	stop();

	bool		is_in_trading() const;
	uint32_t	time_to_mins(uint32_t uTime) const;

private:
	void	trigger_price(WTSTickData* curTick);

private:
	WTSSessionInfo*	_s_info;
	bool _controlled = false;
	bool _session_ended = false;
	WtCtaEngine*	_engine;
	IDataReader*	_store;

	uint32_t	_date;
	uint32_t	_time;

	uint32_t	_cur_pos;

	StdUniqueMutex	_mtx;
	std::atomic<uint64_t>	_next_check_time;
	std::atomic<uint32_t>	_last_emit_pos;

	bool			_stopped;
	StdThreadPtr	_thrd;

};
NS_WTP_END