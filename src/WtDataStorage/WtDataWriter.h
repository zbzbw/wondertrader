#pragma once
#include "DataDefine.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/IDataWriter.h"
#include "../Share/StdUtils.hpp"
#include "../Share/BoostMappingFile.hpp"
#include "../Share/SpinMutex.hpp"
#include "WriterDrainSync.hpp"
#include "RecordingUnitDrain.hpp"

#include <queue>
#include <map>
#include <atomic>

typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

NS_WTP_BEGIN
class WTSObject;
class WTSContractInfo;
NS_WTP_END

USING_NS_WTP;

class WtDataWriter : public IDataWriter
{
public:
	WtDataWriter();
	~WtDataWriter();	

private:
	template<typename HeaderType, typename T>
	void* resizeRTBlock(BoostMFPtr& mfPtr, uint32_t nCount);

	void  proc_loop();

	void  check_loop();

	uint32_t  dump_bars_to_file(WTSContractInfo* ct);

	uint32_t  dump_bars_via_dumper(WTSContractInfo* ct);

private:
	bool	dump_day_data(WTSContractInfo* ct, WTSBarStruct* newBar);

	bool	proc_block_data(const char* tag, std::string& content, bool isBar, bool bKeepHead = true);

	bool	procTick(WTSTickData* curTick, uint32_t procFlag);
	bool	procQueue(WTSOrdQueData* curOrdQue);
	bool	procOrder(WTSOrdDtlData* curOrdDetail);
	bool	procTrans(WTSTransData* curTrans);

public:
	virtual bool init(WTSVariant* params, IDataWriterSink* sink) override;
	virtual void release() override;
	virtual bool stopAndDrain(DataWriterStopResult& result) override;

	virtual bool writeTick(WTSTickData* curTick, uint32_t procFlag) override;

	virtual bool writeOrderQueue(WTSOrdQueData* curOrdQue) override;

	virtual bool writeOrderDetail(WTSOrdDtlData* curOrdDetail) override;

	virtual bool writeTransaction(WTSTransData* curTrans) override;

	virtual void transHisData(const char* sid) override;
	virtual void beginSessionClose(const char* sid) override;
	virtual void beginSessionOpen(const char* sid) override;
	
	virtual bool isSessionProceeded(const char* sid) override;

	virtual WTSTickData* getCurTick(const char* code, const char* exchg = "") override;

private:
	IBaseDataMgr*		_bd_mgr;

	typedef struct _KBlockPair
	{
		RTKlineBlock*	_block;
		BoostMFPtr		_file;
		SpinMutex		_mutex;
		uint64_t		_lasttime;

		_KBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_lasttime = 0;
		}

	} KBlockPair;
	typedef wt_hashmap<std::string, KBlockPair*>	KBlockFilesMap;

	typedef struct _TickBlockPair
	{
		RTTickBlock*	_block;
		BoostMFPtr		_file;
		SpinMutex		_mutex;
		uint64_t		_lasttime;

		std::shared_ptr< std::ofstream>	_fstream;

		_TickBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_fstream = NULL;
			_lasttime = 0;
		}
	} TickBlockPair;
	typedef wt_hashmap<std::string, TickBlockPair*>	TickBlockFilesMap;

	typedef struct _TransBlockPair
	{
		RTTransBlock*	_block;
		BoostMFPtr		_file;
		SpinMutex		_mutex;
		uint64_t		_lasttime;

		_TransBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_lasttime = 0;
		}
	} TransBlockPair;
	typedef wt_hashmap<std::string, TransBlockPair*>	TransBlockFilesMap;

	typedef struct _OdeDtlBlockPair
	{
		RTOrdDtlBlock*	_block;
		BoostMFPtr		_file;
		SpinMutex		_mutex;
		uint64_t		_lasttime;

		_OdeDtlBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_lasttime = 0;
		}
	} OrdDtlBlockPair;
	typedef wt_hashmap<std::string, OrdDtlBlockPair*>	OrdDtlBlockFilesMap;

	typedef struct _OdeQueBlockPair
	{
		RTOrdQueBlock*	_block;
		BoostMFPtr		_file;
		SpinMutex		_mutex;
		uint64_t		_lasttime;

		_OdeQueBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_lasttime = 0;
		}
	} OrdQueBlockPair;
	typedef wt_hashmap<std::string, OrdQueBlockPair*>	OrdQueBlockFilesMap;
	

	KBlockFilesMap	_rt_min1_blocks;
	KBlockFilesMap	_rt_min5_blocks;

	TickBlockFilesMap	_rt_ticks_blocks;
	TransBlockFilesMap	_rt_trans_blocks;
	OrdDtlBlockFilesMap _rt_orddtl_blocks;
	OrdQueBlockFilesMap _rt_ordque_blocks;

	SpinMutex		_lck_tick_cache;
	wt_hashmap<std::string, uint32_t> _tick_cache_idx;
	BoostMFPtr		_tick_cache_file;
	RTTickCache*	_tick_cache_block;

	//typedef std::function<void()> TaskInfo;
	typedef struct alignas(64) _TaskInfo
	{
		WTSObject*	_obj;
		uint64_t	_type;
		uint32_t	_flag;
		std::string _session_id;
		std::string _fullcode;
		uint32_t _trading_date;
		std::string _unit_key;

		_TaskInfo(WTSObject* data, uint64_t dtype, uint32_t flag = 0);

		_TaskInfo(const _TaskInfo& rhs);

		~_TaskInfo();

	} TaskInfo;
	std::queue<TaskInfo>	_tasks;
	StdThreadPtr			_task_thrd;
	StdUniqueMutex			_task_mtx;
	StdCondVariable			_task_cond;
	std::atomic<uint64_t>	_received_offset;
	std::atomic<uint64_t>	_completed_offset;
	std::atomic<uint64_t>	_persisted_offset;
	std::atomic<bool>		_processing_failed;
	bool					_accepting;
	WriterDrainSync			_drain_sync;
	RecordingUnitDrain		_unit_drain;

	std::string		_base_dir;
	std::string		_cache_file;
	uint32_t		_log_group_size;
	bool			_async_proc;

	StdCondVariable	_proc_cond;
	StdUniqueMutex	_proc_mtx;
	typedef struct _ClosingTask
	{
		std::string command;
		std::string session_id;
		std::string fullcode;
		uint32_t trading_date;

		_ClosingTask(const std::string& value = "")
			: command(value), trading_date(0)
		{
		}
	} ClosingTask;
	typedef struct _UnitCloseResult
	{
		bool persisted;
		uint64_t row_count;
		uint64_t file_size;
		std::string dmb_path;
		std::string dsb_path;
		std::string dsb_fingerprint;

		_UnitCloseResult()
			: persisted(false), row_count(0), file_size(0)
		{
		}
	} UnitCloseResult;
	typedef struct _DmbBoundary
	{
		uint32_t trading_date;
		uint64_t row_count;
		uint64_t file_size;
		std::string relative_path;
		std::string fingerprint;

		_DmbBoundary()
			: trading_date(0), row_count(0), file_size(0)
		{
		}
	} DmbBoundary;
	std::queue<ClosingTask> _proc_que;
	std::map<std::string, std::map<std::string, UnitCloseResult>> _session_close_results;
	StdThreadPtr	_proc_thrd;
	StdThreadPtr	_proc_chk;
	StdCondVariable	_check_cond;
	StdUniqueMutex	_check_mtx;
	bool			_released;

	bool			_save_tick_log;
	bool			_skip_notrade_tick;
	bool			_skip_notrade_bar;
	bool			_disable_his;

	bool			_disable_tick;
	bool			_disable_min1;
	bool			_disable_min5;
	bool			_disable_day;

	bool			_disable_trans;
	bool			_disable_ordque;
	bool			_disable_orddtl;

	/*
	 *	by Wesley @ 2023.05.04
	 *	分钟线价格模式，0-常规模式，1-将买卖价也记录下来，这个设计时只针对期权这种不活跃的品种
	 */
	uint32_t		_min_price_mode;
	
	std::map<std::string, uint32_t> _proc_date;

private:
	void loadCache();
	bool pushTask(const TaskInfo& task);
	bool beginSyncTask(TaskInfo& task);
	void completeTask(const TaskInfo& task, bool persisted);
	bool flushFiles();
	void releaseFiles();
	bool inspectTickDmb(
		const std::string& fullcode,
		DmbBoundary& boundary) const;
	bool persistPendingRecordingUnits();
	void restorePendingRecordingUnits();
	bool validateSessionDmbBoundaries(
		const char* sid,
		const std::vector<RecordingUnitProgress>& units) const;
	bool persistRecordingUnitFact(
		const RecordingUnitProgress& unit,
		const UnitCloseResult& close_result,
		const std::string& marker_path,
		uint32_t marker_value);

	bool updateCache(WTSContractInfo* ct, WTSTickData* curTick, uint32_t procFlag);

	bool pipeToTicks(WTSContractInfo* ct, WTSTickData* curTick);

	void pipeToKlines(WTSContractInfo* ct, WTSTickData* curTick);

	KBlockPair* getKlineBlock(WTSContractInfo* ct, WTSKlinePeriod period, bool bAutoCreate = true);

	TickBlockPair* getTickBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true);
	TransBlockPair* getTransBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true);
	OrdDtlBlockPair* getOrdDtlBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true);
	OrdQueBlockPair* getOrdQueBlock(WTSContractInfo* ct, uint32_t curDate, bool bAutoCreate = true);

	template<typename T>
	void	releaseBlock(T* block);

};

