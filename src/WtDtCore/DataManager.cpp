/*!
 * \file DataManager.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "DataManager.h"
#include "StateMonitor.h"
#include "UDPCaster.h"
#include "WtHelper.h"
#include "IDataCaster.h"
#include "LiveBarCalendar.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/DLLHelper.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/decimal.h"
#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include <boost/filesystem.hpp>
#include <ctime>

#include "../WTSTools/WTSBaseDataMgr.h"
#include "../WTSTools/WTSLogger.h"


DataManager::DataManager()
	: _writer_params(NULL), _creator(NULL), _segment(0), _received(0), _last_receive_log_second(0)
	, _recording(false), _recording_failed(false), _accepted_total(0), _persisted_total(0)
	, _recording_filter_set(false)
	, _recording_limit_enabled(false), _recording_capacity_exhausted(false), _recording_limit(0)
	, _writer(NULL)
	, _writer_ready(false)
	, _bd_mgr(NULL)
	, _state_mon(NULL)
{
}


DataManager::~DataManager()
{
	if (_writer_params) _writer_params->release();
}

bool DataManager::isSessionProceeded(const char* sid)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (!_recording || _recording_failed || _writer == NULL)
		return true;
	uint64_t accepted, persisted;
	_writer->recordingOffsets(accepted, persisted, _recording_failed);
	if (_recording_failed) return true;

	return _writer->isSessionProceeded(sid);
}

bool DataManager::init(WTSVariant* params, WTSBaseDataMgr* bdMgr, StateMonitor* stMonitor)
{
	_bd_mgr = bdMgr;
	_state_mon = stMonitor;
	_last_drain.writer_drained = _last_drain.checkpoint_persisted = true;
	if (!params) return true;
	_writer_params = params;
	params->retain();
	_base_path = StrUtil::standardisePath(params->getCString("path"));
	if (boost::filesystem::exists(_base_path + "segments")
		|| boost::filesystem::is_regular_file(_base_path + "recording/closed.json"))
	{
		std::string next_path;
		do {
			next_path = _base_path + "segments/" + std::to_string(++_segment) + "/";
		} while (boost::filesystem::exists(next_path));
		params->append("path", next_path.c_str());
	}
	if (params->has("enabled") && !params->getBoolean("enabled")) return true;
	if (!loadWriter()) return false;
	_writer_ready = _writer->init(params, this);
	_recording = _writer_ready;
	_recording_failed = !_writer_ready;
	if (_writer_ready)
	{
		// Retain the existing restart baseline without relying on future disk writes.
		WTSArray* contracts = _bd_mgr->getContracts();
		for (auto it = contracts->begin(); it != contracts->end(); ++it)
		{
			WTSContractInfo* contract = static_cast<WTSContractInfo*>(*it);
			WTSTickData* cached = _writer->getCurTick(contract->getCode(), contract->getExchg());
			if (cached)
			{
				_latest_ticks[contract->getFullCode()] = cached->getTickStruct();
				cached->release();
			}
		}
		contracts->release();
	}
	return _writer_ready;
}

bool DataManager::loadWriter()
{
	std::string module = _writer_params->getCString("module");
	if (module.empty())
		module = WtHelper::get_module_dir() + DLLHelper::wrap_module("WtDataStorage");
	else
		module = WtHelper::get_module_dir() + DLLHelper::wrap_module(module.c_str());
	
	DllHandle libWriter = DLLHelper::load_library(module.c_str());
	if (libWriter)
	{
		FuncCreateWriter pFuncCreateWriter = (FuncCreateWriter)DLLHelper::get_symbol(libWriter, "createWriter");
		if (pFuncCreateWriter == NULL)
		{
			WTSLogger::error("Initializing of data writer failed: function createWriter not found...");
		}

		FuncDeleteWriter pFuncDeleteWriter = (FuncDeleteWriter)DLLHelper::get_symbol(libWriter, "deleteWriter");
		if (pFuncDeleteWriter == NULL)
		{
			WTSLogger::error("Initializing of data writer failed: function deleteWriter not found...");
		}

		if (pFuncCreateWriter && pFuncDeleteWriter)
		{
			_creator = pFuncCreateWriter;
			_writer = pFuncCreateWriter();
			_remover = pFuncDeleteWriter;
		}
		WTSLogger::info("Data storage module {} loaded", module);
	}
	else
	{
		WTSLogger::error("Initializing of data writer failed: loading module {} failed...", module.c_str());
		return false;
	}

	if (_writer == NULL)
		return false;
	uint64_t accepted, persisted;
	bool failed;
	if (!_writer->recordingOffsets(accepted, persisted, failed)) {
		WTSLogger::error("Data writer does not support independent recording controls; use WtDataStorage");
		_recording_failed = true;
		_remover(_writer);
		_writer = NULL;
		return false;
	}
	return true;
}

void DataManager::add_ext_dumper(const char* id, IHisDataDumper* dumper)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	_ext_dumpers[id] = dumper;
	if (_writer == NULL)
		return;

	_writer->add_ext_dumper(id, dumper);
}

void DataManager::release()
{
	if (_writer)
	{
		_writer->release();
		_remover(_writer);
		_writer = NULL;
		_writer_ready = false;
	}
}

bool DataManager::stopAndFlush(DataWriterStopResult& result)
{
	return stopRecording(result);
}

bool DataManager::stopRecording(DataWriterStopResult& result)
{
	IDataWriter* closing = NULL;
	{
		std::lock_guard<std::mutex> lock(_writer_mtx);
		if (_writer == NULL)
		{
			result = _last_drain;
			return !_recording_failed && result.writer_drained && result.checkpoint_persisted;
		}
		_recording = false;
		closing = _writer;
		_writer = NULL;
	}
	// The reception path can continue while the detached writer drains.
	DataWriterStopResult drained;
	bool success = closing->stopAndDrain(drained);
	success = success && _writer_ready;
	_remover(closing);
	std::lock_guard<std::mutex> lock(_writer_mtx);
	_writer_ready = false;
	_accepted_total += drained.received_offset;
	_persisted_total += drained.persisted_offset;
	_recording_failed = !success;
	_last_drain = drained;
	_last_drain.received_offset = _accepted_total;
	_last_drain.persisted_offset = _persisted_total;
	result = _last_drain;
	return success;
}

bool DataManager::setRecordingInstruments(const std::set<std::string>& instruments)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_recording || _writer != NULL || _recording_failed) return false;
	_recording_instruments = instruments;
	_recording_filter_set = true;
	return true;
}

bool DataManager::startRecording()
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_recording) return true;
	if (_recording_failed || !_writer_params) return false;
	if (!_creator && !loadWriter()) return false;
	// loadWriter creates the first writer; subsequent starts create a new one.
	// A new directory keeps the deliberate recording gap out of old bars/files.
	std::string segment_path;
	do {
		++_segment;
		segment_path = _base_path + "segments/" + std::to_string(_segment) + "/";
	} while (boost::filesystem::exists(segment_path));
	_writer_params->append("path", segment_path.c_str());
	if (!_writer) _writer = _creator();
	_writer_ready = _writer->init(_writer_params, this);
	for (const auto& dumper : _ext_dumpers)
		_writer->add_ext_dumper(dumper.first.c_str(), dumper.second);
	_recording = _writer_ready;
	_recording_failed = !_writer_ready;
	return _writer_ready;
}

bool DataManager::setRecordingLimit(uint64_t accepted_limit)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_recording_failed) return false;
	// Exhaustion is sticky until a completed stop and an explicit start.
	if (!_recording) _recording_capacity_exhausted = false;
	_recording_limit_enabled = true;
	_recording_limit = accepted_limit;
	return true;
}

void DataManager::recordingStatus(bool& recording, bool& failed, uint64_t& received,
	uint64_t& accepted, uint64_t& persisted, uint64_t& segment, bool& capacity_exhausted)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	recording = _recording;
	failed = _recording_failed;
	received = _received.load();
	accepted = persisted = 0;
	if (_writer) _writer->recordingOffsets(accepted, persisted, failed);
	accepted += _accepted_total;
	persisted += _persisted_total;
	segment = _segment;
	capacity_exhausted = _recording_capacity_exhausted;
}

bool DataManager::writeTick(WTSTickData* curTick, uint32_t procFlag, uint64_t received_ns)
{
	if (!received_ns) received_ns = LiveBarCalendar::now();
	if (curTick == NULL || !updateLatest(curTick, procFlag, received_ns)) return false;
	uint64_t received = _received.fetch_add(1) + 1;
	broadcastTick(curTick);
	uint64_t now = static_cast<uint64_t>(std::time(NULL));
	uint64_t previous = _last_receive_log_second.load();
	if (now != previous && _last_receive_log_second.compare_exchange_strong(previous, now))
		WTSLogger::info("{} shared market data ticks received", received);
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (!_recording || _recording_failed || !_writer) return true;
	if (_recording_filter_set
		&& _recording_instruments.count(curTick->getContractInfo()->getFullCode()) == 0)
		return true;
	uint64_t accepted, persisted;
	_writer->recordingOffsets(accepted, persisted, _recording_failed);
	if (_recording_failed) return true;
	if (_recording_limit_enabled && (_recording_capacity_exhausted
		|| accepted + _accepted_total >= _recording_limit))
	{
		_recording_capacity_exhausted = true;
		return true;
	}
	return _writer->writeTick(curTick, 0);
}

bool DataManager::writeOrderQueue(WTSOrdQueData* curOrdQue)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_writer == NULL)
		return false;

	return _writer->writeOrderQueue(curOrdQue);
}

bool DataManager::writeOrderDetail(WTSOrdDtlData* curOrdDtl)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_writer == NULL)
		return false;

	return _writer->writeOrderDetail(curOrdDtl);
}

bool DataManager::writeTransaction(WTSTransData* curTrans)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_writer == NULL)
		return false;

	return _writer->writeTransaction(curTrans);
}

WTSTickData* DataManager::getCurTick(const char* code, const char* exchg)
{
	WTSContractInfo* ct = _bd_mgr->getContract(code, exchg);
	if (ct == NULL) return NULL;
	std::lock_guard<std::mutex> lock(_latest_mtx);
	auto found = _latest_ticks.find(ct->getFullCode());
	return found == _latest_ticks.end() ? NULL : WTSTickData::create(found->second);
}

bool DataManager::updateLatest(WTSTickData* curTick, uint32_t procFlag, uint64_t received)
{
	auto ct = curTick->getContractInfo();
	if (!ct) return false;
	uint64_t event = 0;
	try { event = LiveBarCalendar::timestamp(curTick->actiondate(), curTick->actiontime()); }
	catch (const std::invalid_argument&) {
		rejectTick(curTick->code(), curTick->exchg(), received);
		return false;
	}
	std::lock_guard<std::mutex> lock(_latest_mtx);
	const std::string code = ct->getFullCode();
	if (!canSessionReceive(ct->getCommInfo()->getSession())) {
		markRejected(code, BarRejected, received);
		return false;
	}
	auto& cache = _latest_ticks[code];
	auto session = ct->getCommInfo()->getSessionInfo();
	if (!curTick->tradingdate() || curTick->tradingdate() < cache.trading_date ||
		!session || session->getOffsetDate(curTick->actiondate(), curTick->actiontime() / 100000) > curTick->tradingdate()) {
		markRejected(code, BarRejected, received);
		return false;
	}
	WTSTickStruct normalized{};
	auto result = _tick_states[code].normalize(curTick->getTickStruct(), event, normalized, procFlag != 0);
	if (result == LiveTickState::Duplicate) return false;
	if (result == LiveTickState::Invalid || result == LiveTickState::Late || result == LiveTickState::CumulativeReset) {
		markRejected(code, result == LiveTickState::CumulativeReset ? BarCumulativeReset : BarRejected, received);
		return false;
	}
	uint32_t flags = 0;
	if (!cache.trading_date || normalized.trading_date != cache.trading_date) flags = 3;
	else {
		if (decimal::gt(normalized.high, cache.high)) flags |= 1;
		if (decimal::lt(normalized.low, cache.low)) flags |= 2;
	}
	curTick->setLimitFlag(flags);
	curTick->getTickStruct() = cache = normalized;
	auto found = _bar_inputs.find(code);
	if (found != _bar_inputs.end()) {
		auto& input = found->second;
		// An observed Tick confirms this instrument even for extended parsers
		// without a subscription-ack callback. Silence never confirms it.
		markInput(input, received);
		input.connected = input.subscribed = true;
		input.status_since = received;
		auto release = [](WTSTickData* value) { value->release(); };
		std::unique_ptr<WTSTickData, decltype(release)> barTick(WTSTickData::create(normalized), release);
		barTick->setCode(_bar_names[input.series.front()].first.c_str());
		barTick->setContractInfo(ct);
		for (auto index : input.series) {
			auto& entry = _bar_series[index];
			advanceSeries(entry, received);
			if (result == LiveTickState::Baseline)
				entry.value->mark(true, false, BarJoined, event, event);
			if (!entry.value->tick(barTick.get(), event, received))
				entry.value->mark(false, true, BarRejected, received, received);
		}
	}
	return true;
}

void DataManager::transHisData(const char* sid)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_writer)
		_writer->transHisData(sid);
}

void DataManager::beginSessionClose(const char* sid)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_writer)
		_writer->beginSessionClose(sid);
}

void DataManager::beginSessionOpen(const char* sid)
{
	std::lock_guard<std::mutex> lock(_writer_mtx);
	if (_writer)
		_writer->beginSessionOpen(sid);
}

//////////////////////////////////////////////////////////////////////////
#pragma region "IDataWriterSink"
IBaseDataMgr* DataManager::getBDMgr()
{
	return _bd_mgr;
}

bool DataManager::canSessionReceive(const char* sid)
{
	//By Wesley @ 2021.12.27
	//如果状态机为NULL，说明是全天候模式，直接返回true即可
	if (_state_mon == NULL)
		return true;

	return _state_mon->isInState(sid, SS_RECEIVING);
}

void DataManager::broadcastTick(WTSTickData* curTick)
{
	for(IDataCaster* caster : _casters)
		caster->broadcast(curTick);
}

void DataManager::broadcastOrdDtl(WTSOrdDtlData* curOrdDtl)
{
	for (IDataCaster* caster : _casters)
		caster->broadcast(curOrdDtl);
}

void DataManager::broadcastOrdQue(WTSOrdQueData* curOrdQue)
{
	for (IDataCaster* caster : _casters)
		caster->broadcast(curOrdQue);
}

void DataManager::broadcastTrans(WTSTransData* curTrans)
{
	for (IDataCaster* caster : _casters)
		caster->broadcast(curTrans);
}

CodeSet* DataManager::getSessionComms(const char* sid)
{
	return  _bd_mgr->getSessionComms(sid);
}

uint32_t DataManager::getTradingDate(const char* pid)
{
	return  _bd_mgr->getTradingDate(pid);
}

void DataManager::outputLog(WTSLogLevel ll, const char* message)
{
	WTSLogger::log_raw(ll, message);
}

#pragma endregion "IDataWriterSink"
