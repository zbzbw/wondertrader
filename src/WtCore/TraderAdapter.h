/*!
 * \file TraderAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include <atomic>
#include <map>
#include <thread>
#include <memory>
#include <boost/noncopyable.hpp>

#include "../Includes/ExecuteDefs.h"
#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/BoostFile.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/SpinMutex.hpp"

NS_WTP_BEGIN
class WTSVariant;
struct WTSTickStruct;
class ActionPolicyMgr;
class WTSContractInfo;
class WTSCommodityInfo;
class WtLocalExecuter;
class EventNotifier;

class ITrdNotifySink;

typedef std::function<void(const char*, bool, double, double, double, double)> FuncEnumChnlPosCallBack;

class TraderAdapter : public ITraderSpi
{
	friend class TraderAdapterLiveTest;
public:
	TraderAdapter(EventNotifier* caster = NULL);
	~TraderAdapter();

	typedef enum tagAdapterState
	{
		AS_NOTLOGIN,		//未登录
		AS_LOGINING,		//正在登录
		AS_LOGINED,			//已登录
		AS_LOGINFAILED,		//登录失败
		AS_POSITION_QRYED,	//仓位已查
		AS_ORDERS_QRYED,	//订单已查
		AS_TRADES_QRYED,	//成交已查
		AS_ALLREADY			//全部就绪
	} AdapterState;

	typedef struct _PosItem
	{
		//多仓数据
		double	l_newvol;
		double	l_newavail;
		double	l_prevol;
		double	l_preavail;

		//空仓数据
		double	s_newvol;
		double	s_newavail;
		double	s_prevol;
		double	s_preavail;

		_PosItem()
		{
			memset(this, 0, sizeof(_PosItem));
		}

		double total_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newvol + l_prevol;
			else
				return s_newvol + s_prevol;
		}

		double avail_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newavail + l_preavail;
			else
				return s_newavail + s_preavail;
		}

	} PosItem;

	typedef struct _RiskParams
	{
		uint32_t	_order_times_boundary;
		uint32_t	_order_stat_timespan;
		uint32_t	_order_total_limits;

		uint32_t	_cancel_times_boundary;
		uint32_t	_cancel_stat_timespan;
		uint32_t	_cancel_total_limits;

		_RiskParams()
		{
			memset(this, 0, sizeof(_RiskParams));
		}
	} RiskParams;

public:
	bool init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);
	bool initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	void release();

	bool run();

	inline const char* id() const{ return _id.c_str(); }

	AdapterState state() const{ return _state; }

	void addSink(ITrdNotifySink* sink)
	{
		_sinks.insert(sink);
	}

	inline bool isReady() const { return _state == AS_ALLREADY; }

	void queryFund();

	struct LiveCommand
	{
		std::string contract, entrust_id, status = "prepared";
		WTSDirectionType direction = WDT_LONG;
		WTSOffsetType offset = WOT_OPEN;
		double price = 0, quantity = 0;
		uint32_t local_id = UINT32_MAX;
	};
	void configureLive(const std::string& run, uint64_t generation, const std::string& contract,
		std::function<void(CommonExecuter)> queue);
	void armLive(const std::string& run, uint64_t generation);
	void blockLive();
	uint32_t submitLive(const std::string& run, uint64_t generation, const std::string& command, WTSEntrust* entrust);
	bool cancelLive(const std::string& run, uint64_t generation, const std::string& command, const std::string& target);
	const std::map<std::string, LiveCommand>& liveCommands() const { return _live_commands; }
	std::string liveSnapshot() const;
	void restoreLive(const std::string& state);
	using LiveReportSink = std::function<void(const char*, const WTSObject*, const WTSObject*)>;
	void setLiveReportSink(LiveReportSink sink) { liveIdentity(_live_run, _live_generation); _live_report = std::move(sink); }
	bool liveConnected() const { return _live_connected; }
	uint32_t liveTradingDay() const { return supportsPaperControl() ? _live_trading_day : _trading_day; }
	bool supportsPaperControl() const { return _mocker_live && _mocker_step && _mocker_version == 1; }
	std::string paperControl(const std::string& request);
	std::string paperStep(uint64_t sequence, uint64_t event_ms, const WTSTickStruct* tick, bool shared_liquidity = false);
	int queryLiveFacts();
	std::string liveReports() const;
	void acknowledgeLiveReports(uint64_t sequence);
	void stampLiveInput(uint64_t sequence, uint64_t event_ms, const std::string& received_at, uint32_t trading_day);
	void recordLiveReport(const char* kind, const WTSObject* payload, const WTSObject* error);

private:
	bool liveSendAllowed(WTSEntrust* entrust) const;
	uint32_t liveOrderId(const WTSOrderInfo* order) const;
	void liveIdentity(const std::string& run, uint64_t generation) const;
	bool liveDeferred();
	void deferLive(CommonExecuter action);
	static std::shared_ptr<WTSObject> copyLive(const WTSObject* value);
	std::function<void(CommonExecuter)> _live_queue;
	bool _live_dispatching = false;
	LiveReportSink _live_report;
	std::map<uint64_t, std::string> _live_reports;
	std::string _live_source;
	uint64_t _live_report_seq = 0, _live_report_ack = 0;
	uint64_t _live_input_seq = 0, _live_event_ms = 0, _live_arrival_ms = 0;
	uint32_t _live_trading_day = 0;
	std::string _live_received_at;
	using MockerControl = const char* (*)(ITraderApi*, const char*);
	MockerControl _mocker_live = nullptr;
	using MockerStep = const char* (*)(ITraderApi*, uint64_t, uint64_t, const WTSTickStruct*, uint32_t);
	MockerStep _mocker_step = nullptr;
	uint32_t _mocker_version = 0;
	bool _live_controlled = false;
	std::atomic<bool> _live_enabled{false}, _live_connected{false};
	std::thread::id _live_owner;
	std::string _live_run, _live_contract;
	uint64_t _live_generation = 0;
	uint32_t _live_local_order = 0;
	WTSEntrust* _live_entrust = nullptr;
	LiveCommand* _live_command = nullptr;
	uint32_t _live_cancel_local = UINT32_MAX;
	std::map<std::string, LiveCommand> _live_commands;
	std::map<std::string, std::pair<std::string, bool>> _live_cancels;

	uint32_t doEntrust(WTSEntrust* entrust);
	bool	doCancel(WTSOrderInfo* ordInfo);

	inline void	printPosition(const char* stdCode, const PosItem& pItem);

	inline WTSContractInfo* getContract(const char* stdCode);
	inline WTSCommodityInfo* getCommodify(const char* stdCommID);

	const RiskParams* getRiskParams(const char* stdCode);

	void initSaveData();

	inline void	logTrade(uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo);
	inline void	logOrder(uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo);

	void	saveData(WTSArray* ayFunds = NULL);

	inline void updateUndone(const char* stdCode, double qty, bool bOuput = true);

public:
	double getPosition(const char* stdCode, bool bValidOnly, int32_t flag = 3);
	OrderMap* getOrders(const char* stdCode);
	double getUndoneQty(const char* stdCode)
	{
		auto it = _undone_qty.find(stdCode);
		if (it != _undone_qty.end())
			return it->second;

		return 0;
	}

	void enumPosition(FuncEnumChnlPosCallBack cb);

	uint32_t openLong(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL);
	uint32_t openShort(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL);
	uint32_t closeLong(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL);
	uint32_t closeShort(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL);
	
	OrderIDs buy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);
	OrderIDs sell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);
	bool	cancel(uint32_t localid);
	OrderIDs cancel(const char* stdCode, bool isBuy, double qty = 0);

	inline bool	isTradeEnabled(const char* stdCode) const;

	bool	checkCancelLimits(const char* stdCode);
	bool	checkOrderLimits(const char* stdCode);

	bool	checkSelfMatch(const char* stdCode, WTSTradeInfo* tInfo);

	inline	bool isSelfMatched(const char* stdCode)
	{
		//如果忽略自成交，则直接返回false
		if (_ignore_sefmatch)
			return false;

		auto it = _self_matches.find(stdCode);
		return it != _self_matches.end();
	}

public:
	//////////////////////////////////////////////////////////////////////////
	//ITraderSpi接口
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override;

	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override;

	virtual void onLogout() override;

	virtual void onRspEntrust(WTSEntrust* entrust, WTSError *err) override;

	virtual void onRspAccount(WTSArray* ayAccounts) override;

	virtual void onRspPosition(const WTSArray* ayPositions) override;

	virtual void onRspOrders(const WTSArray* ayOrders) override;

	virtual void onRspTrades(const WTSArray* ayTrades) override;

	virtual void onPushOrder(WTSOrderInfo* orderInfo) override;

	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override;

	virtual void onTraderError(WTSError* err, void* pData = NULL) override;

	virtual IBaseDataMgr* getBaseDataMgr() override;

	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override;

private:
	WTSVariant*			_cfg;
	std::string			_id;
	std::string			_order_pattern;

	uint32_t			_trading_day;

	ITraderApi*			_trader_api;
	FuncDeleteTrader	_remover;
	AdapterState		_state;

	EventNotifier*		_notifier;

	wt_hashset<ITrdNotifySink*>	_sinks;

	IBaseDataMgr*		_bd_mgr;
	ActionPolicyMgr*	_policy_mgr;

	wt_hashmap<std::string, PosItem> _positions;

	SpinMutex	_mtx_orders;
	OrderMap*	_orders;
	wt_hashset<std::string> _orderids;	//主要用于标记有没有处理过该订单

	wt_hashmap<std::string, std::string>		_trade_refs;	//用于记录成交单和订单的匹配
	wt_hashset<std::string>					_self_matches;	//自成交的合约

	/*
	 *	By Wesley @ 2023.03.16
	 *	加一个控制，这样自成交发生以后，还可以恢复交易
	 */
	bool			_ignore_sefmatch;		//忽略自成交限制

	wt_hashmap<std::string, double> _undone_qty;	//未完成数量

	typedef WTSHashMap<std::string>	TradeStatMap;
	TradeStatMap*	_stat_map;	//统计数据

	//这两个缓存时间内的容器,主要是为了控制瞬间流量而设置的
	typedef std::vector<uint64_t> TimeCacheList;
	typedef wt_hashmap<std::string, TimeCacheList> CodeTimeCacheMap;
	CodeTimeCacheMap	_order_time_cache;	//下单时间缓存
	CodeTimeCacheMap	_cancel_time_cache;	//撤单时间缓存

	//如果被风控了,就会进入到排除队列
	wt_hashset<std::string>	_exclude_codes;
	
	typedef wt_hashmap<std::string, RiskParams>	RiskParamsMap;
	RiskParamsMap	_risk_params_map;
	bool			_risk_mon_enabled;

	bool			_save_data;	//是否保存交易日志
	BoostFilePtr	_trades_log;		//交易数据日志
	BoostFilePtr	_orders_log;		//订单数据日志
	std::string		_rt_data_file;		//实时数据文件
};

typedef std::shared_ptr<TraderAdapter>				TraderAdapterPtr;
typedef wt_hashmap<std::string, TraderAdapterPtr>	TraderAdapterMap;


//////////////////////////////////////////////////////////////////////////
//TraderAdapterMgr
class TraderAdapterMgr : private boost::noncopyable
{
public:
	void	release();

	void	run();

	const TraderAdapterMap& getAdapters() const { return _adapters; }

	TraderAdapterPtr getAdapter(const char* tname);

	bool	addAdapter(const char* tname, TraderAdapterPtr& adapter);

	void	refresh_funds();

private:
	TraderAdapterMap	_adapters;
};

NS_WTP_END
