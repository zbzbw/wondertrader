#pragma once
#include <atomic>
#include <memory>
#include <thread>
#include "PaperAccount.h"

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/asio/io_service.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/StdUtils.hpp"
#include "../Includes/WTSCollection.hpp"


NS_WTP_BEGIN
	class WTSTickData;
NS_WTP_END

USING_NS_WTP;

/*
 *	仿真交易器
 */
class TraderMocker : public ITraderApi
{
	friend class TraderMockerMatchingTest;
	friend class TraderMockerAccountTest;
public:
	TraderMocker();
	~TraderMocker();

	// Invoked by the controlled WtPorter driver, never by a timer/UDP worker.
	void controlled_step(uint64_t input_seq, uint64_t event_ms, WTSTickData* tick, bool shared_liquidity = false);
	void controlled_barrier();
	void controlled_settle(uint32_t trading_day, int64_t official_price);
	void controlled_expire_day(uint32_t trading_day);
	void controlled_begin_day(PaperAccount::Rules rules, int64_t mark);
	std::string controlled_snapshot();
	void controlled_restore(const std::string& state);
	std::string controlled_request(const std::string& request);

private:
	/*
	*	撮合 定时器
	*/
	int32_t		match_once();

	uint32_t	makeTradeID();
	uint32_t	makeOrderID();

	void		load_positions();
	void		save_positions();
	std::unique_ptr<PaperAccount> _paper;
	static int64_t paper_price(double price);
	static PaperAccount::Rules paper_rules(const std::function<std::string(const char*)>& value, uint32_t day);
	void controlled_owner() const;
	std::thread::id _owner = std::this_thread::get_id();
	uint64_t _input_seq = 0, _event_ms = 0;
	bool _controlled_connected = false, _draining = false;
	bool _shared_liquidity = false;
	std::map<std::string, int64_t> _trade_fees;


private:
	StdThreadPtr	_thrd_match;
	bool			_terminated;

	StdUniqueMutex		_mutex_api;

	std::atomic<uint32_t>	_auto_trade_id;
	std::atomic<uint32_t>	_auto_order_id;
	std::atomic<uint32_t>	_auto_entrust_id;

	ITraderSpi* _listener;
	IBaseDataMgr*		_bd_mgr = nullptr;

	StdThreadPtr		_thrd_worker;

	uint32_t		_millisecs;
	uint32_t		_mocker_id;
	bool			_use_newpx;
	double			_max_qty;
	double			_min_qty;

	WTSArray*		_orders;
	WTSArray*		_trades;
	typedef WTSHashMap<std::string> TickCache;
	TickCache*		_ticks;

	typedef WTSHashMap<std::string> OrderCache;
	OrderCache*			_awaits;
	StdUniqueMutex	_mtx_awaits;

	wt_hashset<std::string>	_codes;

	uint64_t		_max_tick_time;
	uint64_t		_last_match_time;

	typedef struct _PosUnit
	{
		double	_volume;
		double	_frozen;
	} PosUnit;

	typedef struct _PosItem
	{
		char	_exchg[MAX_INSTRUMENT_LENGTH];
		char	_code[MAX_INSTRUMENT_LENGTH];

		PosUnit		_long;
		PosUnit		_short;

		_PosItem()
		{
			memset(this, 0, sizeof(_PosItem));
		}
	} PosItem;

	wt_hashmap<std::string, PosItem> _positions;
	std::string		_pos_file;

private:
	int			_udp_port;

	boost::asio::ip::udp::endpoint	_broad_ep;
	boost::asio::io_service			_io_service;

	boost::asio::ip::udp::socket*	_b_socket;

	boost::array<char, 1024> _b_buffer;

	void	handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad);

	void	extract_buffer(uint32_t length, bool isBroad);

	void	reconn_udp();

//////////////////////////////////////////////////////////////////////////
//ITraderApi
public:
	virtual bool init(WTSVariant *params) override;

	virtual void release() override;

	virtual void registerSpi(ITraderSpi *listener) override;

	virtual void connect() override;

	virtual void disconnect() override;

	virtual bool isConnected() override;

	virtual bool makeEntrustID(char* buffer, int length) override;

	virtual int login(const char* user, const char* pass, const char* productInfo) override;

	virtual int logout() override;

	virtual int orderInsert(WTSEntrust* eutrust) override;

	virtual int orderAction(WTSEntrustAction* action) override;

	virtual int queryAccount() override;

	virtual int queryPositions() override;

	virtual int queryOrders() override;

	virtual int queryTrades() override;
};
