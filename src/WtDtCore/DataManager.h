/*!
 * \file DataManager.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据管理器定义
 */
#pragma once

#include "../Includes/IDataWriter.h"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostMappingFile.hpp"
#include "../Includes/WTSDataDef.hpp"
#include <map>
#include <mutex>
#include <atomic>
#include "LiveTickState.h"
#include "LiveBarSeries.h"
#include <set>

NS_WTP_BEGIN
class WTSTickData;
class WTSOrdQueData;
class WTSOrdDtlData;
class WTSTransData;
class WTSVariant;
class IDataCaster;
NS_WTP_END

USING_NS_WTP;

class WTSBaseDataMgr;
class StateMonitor;
class UDPCaster;

class DataManager : public IDataWriterSink
{
public:
	DataManager();
	~DataManager();

public:
	bool init(WTSVariant* params, WTSBaseDataMgr* bdMgr, StateMonitor* stMonitor);
	bool initBars(WTSVariant* config, uint64_t now_ns);
	void advanceBars(uint64_t now_ns);
	std::string queryBars(const char* packet, size_t length);
	void parserSubscriptions(const std::string& id, const CodeSet& codes, uint64_t now_ns);
	void parserEvent(const std::string& id, WTSParserEvent event, int32_t error, uint64_t now_ns);
	void parserSubscription(const std::string& id, const char* code, bool accepted, uint64_t now_ns);
	void rejectTick(const char* code, const char* exchange, uint64_t now_ns);
	void rejectParserTick(const std::string& id, const char* code, const char* exchange, uint64_t now_ns);

	void add_ext_dumper(const char* id, IHisDataDumper* dumper);

	inline void add_caster(IDataCaster* caster)
	{
		if (caster == NULL)
			return;

		_casters.emplace_back(caster);
	}

	void release();
	bool stopAndFlush(DataWriterStopResult& result);
	bool startRecording();
	bool setRecordingInstruments(const std::set<std::string>& instruments);
	bool stopRecording(DataWriterStopResult& result);
	void recordingStatus(bool& recording, bool& failed, uint64_t& received,
		uint64_t& accepted, uint64_t& persisted, uint64_t& segment);


	bool writeTick(WTSTickData* curTick, uint32_t procFlag, uint64_t received_ns = 0);

	bool writeOrderQueue(WTSOrdQueData* curOrdQue);

	bool writeOrderDetail(WTSOrdDtlData* curOrdDetail);

	bool writeTransaction(WTSTransData* curTrans);

	void transHisData(const char* sid);
	void beginSessionClose(const char* sid);
	void beginSessionOpen(const char* sid);
	
	bool isSessionProceeded(const char* sid);

	WTSTickData* getCurTick(const char* code, const char* exchg = "");

public:
	//////////////////////////////////////////////////////////////////////////
	//IDataWriterSink
	virtual IBaseDataMgr* getBDMgr() override;

	virtual bool canSessionReceive(const char* sid) override;

	virtual void broadcastTick(WTSTickData* curTick) override;

	virtual void broadcastOrdQue(WTSOrdQueData* curOrdQue) override;

	virtual void broadcastOrdDtl(WTSOrdDtlData* curOrdDtl) override;

	virtual void broadcastTrans(WTSTransData* curTrans) override;

	virtual CodeSet* getSessionComms(const char* sid) override;

	virtual uint32_t getTradingDate(const char* pid) override;

	/*
	*	处理解析模块的日志
	*	@ll			日志级别
	*	@message	日志内容
	*/
	virtual void outputLog(WTSLogLevel ll, const char* message) override;

private:
	bool loadWriter();
	struct BarInput {
		WTSContractInfo* contract = nullptr;
		std::string parser;
		bool connected = false, subscribed = false;
		uint64_t status_since = 0;
		std::vector<size_t> series;
	};
	struct BarEntry {
		std::unique_ptr<LiveBarSeries> value;
		BarInput* input = nullptr;
		uint32_t day = 0, minutes = 0;
	};
	void markInput(BarInput& input, uint64_t now_ns);
	void markRejected(const std::string& code, uint32_t reason, uint64_t now_ns);
	void advanceSeries(BarEntry& entry, uint64_t now_ns);
	std::map<std::string, LiveTickState> _tick_states;
	std::map<std::string, BarInput> _bar_inputs;
	std::map<std::pair<std::string, std::string>, BarInput*> _bar_subscriptions;
	std::vector<BarEntry> _bar_series;
	std::vector<std::pair<std::string, std::string>> _bar_names;
	std::map<std::pair<std::string, std::string>, size_t> _bar_index;
	std::string _bar_stream;
	size_t _bar_clock_index = 0;
	uint64_t _bar_delay = 2000000000ULL, _bar_joined = 0, _bar_clock_ns = 0;
	bool updateLatest(WTSTickData* tick, uint32_t procFlag, uint64_t received_ns);
	std::mutex _latest_mtx;
	std::map<std::string, WTSTickStruct> _latest_ticks;
	std::mutex _writer_mtx;
	WTSVariant* _writer_params;
	FuncCreateWriter _creator;
	std::string _base_path;
	uint64_t _segment;
	std::atomic<uint64_t> _received;
	std::atomic<uint64_t> _last_receive_log_second;
	bool _recording;
	bool _recording_failed;
	bool _recording_filter_set;
	std::set<std::string> _recording_instruments;
	DataWriterStopResult _last_drain;
	uint64_t _accepted_total;
	uint64_t _persisted_total;
	ExtDumpers _ext_dumpers;
	IDataWriter*		_writer;
	FuncDeleteWriter	_remover;
	bool				_writer_ready;
	WTSBaseDataMgr*		_bd_mgr;
	StateMonitor*		_state_mon;
	std::vector<IDataCaster*>	_casters;
};

