#include "TraderMocker.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"

#include <boost/bind.hpp>
#include <filesystem>
#include <cmath>
#include <stdexcept>
namespace fs = std::filesystem;

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	const char* buffer = fmtutil::format(format, args...);

	sink->handleTraderLog(ll, buffer);
}

extern "C"
{
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderMocker *instance = new TraderMocker();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			delete trader;
			trader = NULL;
		}
	}
}

std::vector<uint32_t> splitVolume(uint32_t vol, uint32_t minQty = 1, uint32_t maxQty = 100)
{
	std::vector<uint32_t> ret;
	if (minQty == 0 || maxQty < minQty)
		throw std::invalid_argument("Invalid mocker fill quantity bounds");
	// Stable maximum-sized whole-lot fills, with the final residual preserved.
	while (vol > 0)
	{
		uint32_t curVol = (std::min)(vol, maxQty);
		ret.emplace_back(curVol);
		vol -= curVol;
	}

	return ret;
}
TraderMocker::TraderMocker()
	: _terminated(false)
	, _listener(NULL)
	, _ticks(NULL)
	, _orders(NULL)
	, _awaits(NULL)
	, _trades(NULL)
	, _b_socket(NULL)
	, _max_tick_time(0)
	, _last_match_time(0)
{
	_auto_order_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 100);
	_auto_trade_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 300);
	_auto_entrust_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 100);
}


TraderMocker::~TraderMocker()
{
	if (_awaits)
		_awaits->release();
	if (_orders)
		_orders->release();

	if (_trades)
		_trades->release();

	if (_ticks)
		_ticks->release();
}

uint32_t TraderMocker::makeTradeID()
{
	return ++_auto_trade_id;
}

uint32_t TraderMocker::makeOrderID()
{
	return ++_auto_order_id;
}

bool TraderMocker::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	try
	{
		fmtutil::format_to(buffer, "me.{}.{}.{}", _paper ? _paper->rules().trading_day : TimeUtils::getCurDate(), _mocker_id, _auto_entrust_id++);
		return true;
	}
	catch (...)
	{

	}

	return false;
}

int TraderMocker::orderInsert(WTSEntrust* entrust)
{
	if (_paper) controlled_owner();
	if (entrust == NULL)
	{
		return 0;
	}

	entrust->retain();
	_io_service.post([this, entrust](){
		StdUniqueLock ordersLock(_mtx_awaits, std::defer_lock); if (!_paper) ordersLock.lock();
		StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();

		WTSContractInfo* ct = entrust->getContractInfo();
		if(ct == NULL) 
			ct = _bd_mgr->getContract(entrust->getCode(), entrust->getExchg());

		/*
		 *	1、开仓无需检查
		 *	2、平仓要先检查可平
		 *	3、检查通过了,平仓还要冻结持仓
		 *	4、还要考虑国际期货的问题
		 */

		bool bPass = false;
		std::string msg;
		do 
		{
			if (ct == NULL)
			{
				bPass = false;
				msg = "品种不存在";
				break;
			}
			WTSCommodityInfo* commInfo = ct->getCommInfo();
			if (_paper)
			{
				try
				{
					if (!_controlled_connected || _paper->rules().contract != ct->getFullCode() || entrust->getPriceType() != WPT_LIMITPRICE
						|| entrust->getOrderFlag() != WOF_NOR || entrust->isNet()
						|| (entrust->getDirection() != WDT_LONG && entrust->getDirection() != WDT_SHORT)
						|| !std::isfinite(entrust->getVolume()) || entrust->getVolume() <= 0
						|| entrust->getVolume() > INT32_MAX || std::floor(entrust->getVolume()) != entrust->getVolume())
						throw std::invalid_argument("Controlled paper requires a whole-lot limit order on its contract");
					PaperAccount::Offset offset;
					switch (entrust->getOffsetType())
					{
					case WOT_OPEN: offset = PaperAccount::Open; break;
					case WOT_CLOSETODAY: offset = PaperAccount::Today; break;
					case WOT_CLOSEYESTERDAY: offset = PaperAccount::Yesterday; break;
					default: throw std::invalid_argument("Explicit today/yesterday offset required");
					}
					bool known = _paper->orders().count(entrust->getEntrustID()) != 0;
					_paper->reserve(entrust->getEntrustID(), entrust->getDirection() == WDT_SHORT,
						offset, static_cast<int64_t>(entrust->getVolume()), paper_price(entrust->getPrice()));
					if (known) { entrust->release(); return; }
					bPass = true;
				}
				catch (const std::exception& error) { msg = error.what(); }
				break;
			}

			//检查价格类型的合法性
			if (entrust->getPriceType() == WPT_ANYPRICE && commInfo->getPriceMode() == PM_Limit)
			{
				bPass = false;
				msg = "价格类型不合法";
				break;
			}

			//检查数量的合法性
			if ((commInfo->getCategoty() == CC_Stock) && (entrust->getOffsetType() == WOT_OPEN) && !decimal::eq(decimal::mod(entrust->getVolume(), 100), 0))
			{
				bPass = false;
				msg = "股票买入数量必须为100的整数倍";
				break;
			}

			//检查方向的合法性
			if(!commInfo->canShort() && entrust->getDirection() == WDT_SHORT)
			{
				bPass = false;
				msg = "股票不能做空";
				break;
			}

			//检查价格的合法性
			if(!decimal::eq(entrust->getPrice(), 0))
			{
				double pricetick = commInfo->getPriceTick();
				double v = entrust->getPrice() / pricetick;

				if (!decimal::eq(decimal::mod(entrust->getPrice(), pricetick), 0))	//整除的检查方式,先小数相除得到商,然后商取整以后,再跟原来的商相减,如果等于0,则是整除,否则是
				{
					bPass = false;
					msg = "委托价格不合法";
					break;
				}
			}


			//开仓直接通过,不检查资金
			if (entrust->getOffsetType() == WOT_OPEN)
			{
				bPass = true;
				break;
			}

			//如果不需要开平,则直接通过,主要针对国际期货
			if (commInfo->getCoverMode() == CM_None)
			{
				bPass = true;
				break;
			}
			
			//如果区分平昨平今,而委托的是平昨,则直接拒绝,因为mocker为了简化处理,不考虑昨仓
			if (commInfo->getCoverMode() == CM_CoverToday && (entrust->getOffsetType() == WOT_CLOSE || entrust->getOffsetType() == WOT_CLOSEYESTERDAY))
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			//如果没有持仓或者持仓不够,也要
			auto it = _positions.find(ct->getFullCode());
			if(it == _positions.end())
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			PosItem& pItem = (PosItem&)it->second;
			bool isLong = entrust->getDirection() == WDT_LONG;

			double validQty = isLong ? (pItem._long._volume - pItem._long._frozen) : (pItem._short._volume - pItem._short._frozen);
			if(decimal::lt(validQty, entrust->getVolume()))
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			//冻结持仓
			if(isLong)
			{
				pItem._long._frozen += entrust->getVolume();
			}
			else
			{
				pItem._short._frozen += entrust->getVolume();
			}

			bPass = true;
			msg = "下单成功";

		} while (false);
		
		if(bPass)
		{
			WTSOrderInfo* ordInfo = WTSOrderInfo::create();
			ordInfo->setContractInfo(ct);
			ordInfo->setCode(entrust->getCode());
			ordInfo->setExchange(entrust->getExchg());
			ordInfo->setDirection(entrust->getDirection());
			ordInfo->setOffsetType(entrust->getOffsetType());
			ordInfo->setUserTag(entrust->getUserTag());
			ordInfo->setEntrustID(entrust->getEntrustID());
			ordInfo->setPrice(entrust->getPrice());
			thread_local static char str[64];
			fmtutil::format_to(str, "mo.{}.{}", _mocker_id, makeOrderID());
			ordInfo->setOrderID(str);
			ordInfo->setStateMsg(msg.c_str());
			ordInfo->setOrderState(WOS_NotTraded_Queuing);
			ordInfo->setOrderTime(_paper ? _event_ms : TimeUtils::getLocalTimeNow());
			if (_paper) ordInfo->setOrderDate(_paper->rules().trading_day);
			ordInfo->setVolume(entrust->getVolume());
			ordInfo->setVolLeft(entrust->getVolume());
			ordInfo->setPriceType(entrust->getPriceType());
			ordInfo->setOrderFlag(entrust->getOrderFlag());

			if (_listener != NULL)
			{
				_listener->onRspEntrust(entrust, NULL);
				_listener->onPushOrder(ordInfo);
			}

			_codes.insert(ct->getFullCode());

			if(_listener)
			{
				write_log(_listener,LL_INFO, "共有{}个品种有待撮合订单", _codes.size());
			}

			if (_orders == NULL)
				_orders = WTSArray::create();
			_orders->append(ordInfo, false);

			if (_awaits == NULL)
				_awaits = OrderCache::create();

			_awaits->add(ordInfo->getOrderID(), ordInfo, true);

			save_positions();
		}
		else
		{
			WTSError* err = WTSError::create(WEC_ORDERINSERT, msg.c_str());
			if (_listener != NULL)
			{
				_listener->onRspEntrust(entrust, err);
			}
			err->release();
		}
		entrust->release();
	});

	return 0;
}

int32_t TraderMocker::match_once()
{
	StdUniqueLock ordersLock(_mtx_awaits, std::defer_lock); if (!_paper) ordersLock.lock();
	if (_terminated || _orders == NULL || _orders->size() == 0 || _ticks == NULL)
		return 0;
	
	int32_t count = 0;
	for (const std::string& fullcode : _codes)
	{
		if (_terminated)
			return count;

		std::string code, exchg;
		auto pos = fullcode.find(".");
		exchg = fullcode.substr(0, pos);
		code = fullcode.substr(pos + 1);

		WTSContractInfo* ct = _bd_mgr->getContract(code.c_str(), exchg.c_str());
		if (ct == NULL)
			continue;

		WTSCommodityInfo* commInfo = ct->getCommInfo();

		WTSTickData* curTick = (WTSTickData*)_ticks->grab(fullcode);
		if (curTick && strcmp(curTick->code(), ct->getCode())==0)
		{
			uint64_t tickTime = (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime();
			if (decimal::gt(curTick->price(), 0) /*&& tickTime >= _last_match_time*/)
			{
				if (_paper) _paper->mark(paper_price(curTick->price()));
				//开始处理订单
				//处理记录
				std::vector<std::string> to_erase;

				double askLeft = curTick->askqty(0), bidLeft = curTick->bidqty(0);
				// _orders is append-only in acceptance order; hash traversal is not FIFO.
				for (uint32_t index = 0; index < _orders->size(); ++index)
				{
					WTSOrderInfo* ordInfo = (WTSOrderInfo*)_orders->at(index);
					if (_awaits->get(ordInfo->getOrderID()) == NULL || ordInfo->getVolLeft() == 0
						|| strcmp(ordInfo->getCode(), ct->getCode()) != 0
						|| strcmp(ordInfo->getExchg(), ct->getExchg()) != 0)
						continue;

					bool isBuy = (ordInfo->getDirection() == WDT_LONG && ordInfo->getOffsetType() == WOT_OPEN) || (ordInfo->getDirection() != WDT_LONG && ordInfo->getOffsetType() != WOT_OPEN);

					double uPrice, uVolume;
					if (isBuy)
					{
						uPrice = curTick->askprice(0);
						uVolume = askLeft;
					}
					else
					{
						uPrice = curTick->bidprice(0);
						uVolume = bidLeft;
					}

					if (decimal::eq(uVolume, 0))
						continue;					

					if (_use_newpx)
					{
						uPrice = curTick->price();
					}

					if (decimal::eq(uPrice, 0))
						continue;

					double target = ordInfo->getPrice();
					//买入的时候,委托价格小于最新价则不成交,卖出的时候,委托价大于最新价则不成交
					if (ordInfo->getPriceType() == WPT_LIMITPRICE && ((isBuy && decimal::lt(target, uPrice)) || (!isBuy && decimal::gt(target, uPrice))))
						continue;

					count++;

					double maxVolume = min(uVolume, ordInfo->getVolLeft());
					std::vector<uint32_t> ayVol = splitVolume((uint32_t)maxVolume, (uint32_t)_min_qty, (uint32_t)_max_qty);
					for (uint32_t curVol : ayVol)
					{
						int64_t fillFee = 0;
						if (_paper)
						{
							auto beforeFee = _paper->orders().at(ordInfo->getEntrustID()).charged_fee;
							_paper->fill(ordInfo->getEntrustID(), curVol, paper_price(uPrice));
							fillFee = _paper->orders().at(ordInfo->getEntrustID()).charged_fee - beforeFee;
							_paper->mark(paper_price(curTick->price()));
						}

						WTSTradeInfo* trade = WTSTradeInfo::create(curTick->code(), curTick->exchg());
						trade->setDirection(ordInfo->getDirection());
						trade->setOffsetType(ordInfo->getOffsetType());
						trade->setContractInfo(ct);

						trade->setPrice(uPrice);
						trade->setVolume(curVol);

						trade->setRefOrder(ordInfo->getOrderID());

						char str[64];
						fmtutil::format_to(str, "mt.{}.{}", _mocker_id, makeTradeID());
						trade->setTradeID(str);
						if (_paper) _trade_fees.emplace(str, fillFee);

						trade->setTradeTime(_paper ? _event_ms : TimeUtils::getLocalTimeNow());
						if (_paper) trade->setTradeDate(_paper->rules().trading_day);
						trade->setUserTag(ordInfo->getUserTag());

						//更新订单数据
						ordInfo->setVolLeft(ordInfo->getVolLeft() - curVol);
						ordInfo->setVolTraded(ordInfo->getVolTraded() + curVol);
						(isBuy ? askLeft : bidLeft) -= curVol;
						if (decimal::eq(ordInfo->getVolLeft(), 0))
						{
							ordInfo->setOrderState(WOS_AllTraded);
							ordInfo->setStateMsg("AllTrd");
							to_erase.emplace_back(ordInfo->getOrderID());
						}
						else
						{
							ordInfo->setOrderState(WOS_PartTraded_Queuing);
							ordInfo->setStateMsg("PartTrd");
						}

						PosItem& pItem = _positions[ct->getFullCode()];
						//第一次的话要给代码和交易所赋值
						if(strlen(pItem._code) == 0)
						{
							strcpy(pItem._code, ct->getCode());
							strcpy(pItem._exchg, ct->getExchg());
						}

						if(_paper || commInfo->getCoverMode() == CM_None)
						{

						}
						else
						{
							if (ordInfo->getDirection() == WDT_LONG)
							{
								if (ordInfo->getOffsetType() == WOT_OPEN)
								{
									pItem._long._volume += curVol;
								}
								else
								{
									pItem._long._volume -= curVol;
									pItem._long._frozen -= curVol;
								}
							}
							else
							{
								if (ordInfo->getOffsetType() == WOT_OPEN)
								{
									pItem._short._volume += curVol;
								}
								else
								{
									pItem._short._volume -= curVol;
									pItem._short._frozen -= curVol;
								}
							}
						}
						

						if (_listener)
						{
							StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();
							_listener->onPushOrder(ordInfo);
							_listener->onPushTrade(trade);
						}

						if (_trades == NULL)
							_trades = WTSArray::create();

						_trades->append(trade, false);
					}
				}

				if (count > 0)
				{
					//write_log(_listener,LL_INFO, "[TraderMocker]触发 %s.%s 开多 %u 条,价格:%u", tick->exchg(), tick->code(), iCount, uPrice);
					for (const std::string& oid : to_erase)
					{
						_awaits->remove(oid);
					}
				}
			}
		}

		if(curTick)
			curTick->release();
	}

	_last_match_time = _max_tick_time;

	_ticks->clear();

	if(count > 0)
		save_positions();

	return count;
}

//////////////////////////////////////////////////////////////////////////

bool TraderMocker::init(WTSVariant *params)
{
	// Controlled account configuration is explicit; legacy smoke keeps its old model.
	if (auto account = params->get("account"))
	{
		try
		{
			auto rules = paper_rules([&](const char* name) { return std::string(account->getString(name)); }, account->getUInt32("trading_day"));
			auto exact = [&](const char* name, unsigned places) {
				return PaperAccount::scaled_decimal(account->getString(name), places);
			};
			_paper.reset(new PaperAccount(rules, exact("initial_cash", 2), exact("mark", 6)));
			_auto_order_id = _auto_trade_id = _auto_entrust_id = 0;
		}
		catch (const std::exception& error) { write_log(_listener, LL_ERROR, "{}", error.what()); return false; }
	}
	_millisecs = params->getUInt32("span");
	_use_newpx = params->getBoolean("newpx");
	if (_paper && _use_newpx) return false; // Controlled fills use the executable counterparty quote.
	_mocker_id = params->getUInt32("mockerid");
	_max_qty = params->getDouble("maxqty");
	_min_qty = params->getDouble("minqty");

	_udp_port = params->getInt32("udp_port");

	boost::asio::ip::address addr = boost::asio::ip::address::from_string("0.0.0.0");
	_broad_ep = boost::asio::ip::udp::endpoint(addr, _udp_port);

	if (decimal::eq(_max_qty, 0))
		_max_qty = 100;

	if (decimal::eq(_min_qty, 0))
		_min_qty = 1;
	if (!std::isfinite(_min_qty) || !std::isfinite(_max_qty)
		|| _min_qty < 1 || _max_qty < _min_qty || _max_qty > UINT32_MAX
		|| std::floor(_min_qty) != _min_qty || std::floor(_max_qty) != _max_qty)
		return false;

	//加载持仓数据
	std::stringstream ss;
	ss << "./mocker_" << _mocker_id << "/";
	std::string path = ss.str();
	fs::create_directories(path.c_str());

	_pos_file = path;
	_pos_file += "positions.json";

	return true;
}

void TraderMocker::load_positions()
{
	if (_paper) return; // A legacy positions file cannot restore a controlled account.
	if (!fs::exists(_pos_file.c_str()))
		return;

	std::string json;
	StdFile::read_file_content(_pos_file.c_str(), json);

	rj::Document root;
	root.Parse(json.c_str());
	if (root.HasParseError())
		return;

	if(root.HasMember("positions"))
	{//读取仓位
		double total_profit = 0;
		double total_dynprofit = 0;
		const rj::Value& jPos = root["positions"];
		if (!jPos.IsNull() && jPos.IsArray())
		{
			for (const rj::Value& pItem : jPos.GetArray())
			{
				const char* exchg = pItem["exchg"].GetString();
				const char* code = pItem["code"].GetString();
				WTSContractInfo* ct = _bd_mgr->getContract(code, exchg);
				if (ct == NULL)
					continue;

				PosItem& pInfo = _positions[ct->getFullCode()];
				strcpy(pInfo._code, ct->getCode());
				strcpy(pInfo._exchg, ct->getExchg());
				
				pInfo._long._volume = pItem["long"]["volume"].GetDouble();

				pInfo._short._volume = pItem["short"]["volume"].GetDouble();
			}
		}
	}

	if (_listener)
		write_log(_listener, LL_INFO, "[TraderMocker]共加载{}条持仓数据", _positions.size());
}

void TraderMocker::save_positions()
{
	if (_paper) return; // Full controlled state is captured at the event barrier.
	rj::Document root(rj::kObjectType);

	{//持仓数据保存
		rj::Value jPos(rj::kArrayType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto& v : _positions)
		{
			const char* fullcode = v.first.c_str();
			const PosItem& pInfo = v.second;

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("exchg", rj::Value(pInfo._exchg, allocator), allocator);
			pItem.AddMember("code", rj::Value(pInfo._code, allocator), allocator);

			{
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("volume", pInfo._long._volume, allocator);

				pItem.AddMember("long", dItem, allocator);
			}

			{
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("volume", pInfo._short._volume, allocator);

				pItem.AddMember("short", dItem, allocator);
			}


			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	{
        rj::StringBuffer sb;
        rj::PrettyWriter<rj::StringBuffer> writer(sb);
        root.Accept(writer);
        StdFile::write_file_content(_pos_file.c_str(), sb.GetString());

	}
}

void TraderMocker::release()
{
	if (_terminated)
		return;

	_terminated = true;

	if (_thrd_match)
	{
		_thrd_match->join();
	}

	if (_thrd_worker)
	{
		_io_service.stop();
		_thrd_worker->join();
	}
}

void TraderMocker::registerSpi(ITraderSpi *listener)
{
	_listener = listener;

	_bd_mgr = listener->getBaseDataMgr();
}

void TraderMocker::reconn_udp()
{
	if (_b_socket != NULL)
	{
		_b_socket->close();
		delete _b_socket;
		_b_socket = NULL;
	}

	_b_socket = new boost::asio::ip::udp::socket(_io_service);

	_b_socket->open(_broad_ep.protocol());
	_b_socket->set_option(boost::asio::ip::udp::socket::reuse_address(true));
	_b_socket->set_option(boost::asio::ip::udp::socket::broadcast(true));
	_b_socket->bind(_broad_ep);


	_b_socket->async_receive_from(boost::asio::buffer(_b_buffer), _broad_ep,
		boost::bind(&TraderMocker::handle_read, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred, true));
}

void TraderMocker::connect()
{
	if (_paper)
	{
		controlled_owner();
		_controlled_connected = true;
		_io_service.post([this]() { if (_listener) _listener->handleEvent(WTE_Connect, 0); });
		return;
	}
	reconn_udp();

	_thrd_worker.reset(new StdThread(boost::bind(&boost::asio::io_service::run, &_io_service)));

	_io_service.post([this](){
		StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();

		load_positions();

		if (_listener)
			_listener->handleEvent(WTE_Connect, 0);
	});
}

void TraderMocker::disconnect()
{
	if (_paper) { controlled_owner(); _controlled_connected = false; return; }
	if (_terminated)
		return;

	_terminated = true;

	if (_thrd_match)
	{
		_thrd_match->join();
	}
}

bool TraderMocker::isConnected()
{
	if (_paper) return _controlled_connected;
	return _thrd_match != NULL;
}

int TraderMocker::login(const char* user, const char* pass, const char* productInfo)
{
	if (_paper)
	{
		controlled_owner();
		if (!_controlled_connected) return -1;
		_io_service.post([this]() { if (_listener) _listener->onLoginResult(true, "", _paper->rules().trading_day); });
		return 0;
	}
	_thrd_match.reset(new StdThread([this]() {
		while (!_terminated)
		{
			match_once();

			//等待5毫秒
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}));

	_io_service.post([this](){
		StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();

		if (_listener)
			_listener->onLoginResult(true, "", TimeUtils::getCurDate());
	});

	return 0;
}

PaperAccount::Rules TraderMocker::paper_rules(const std::function<std::string(const char*)>& value, uint32_t day)
{
	PaperAccount::Rules rules;
	rules.contract = value("contract"); rules.source = value("source"); rules.trading_day = day;
	auto exact = [&](const char* name, unsigned places) { return PaperAccount::scaled_decimal(value(name), places); };
	rules.multiplier = exact("multiplier", 0); rules.tick = exact("tick", 6);
	rules.upper_limit = exact("upper_limit", 6); rules.lower_limit = exact("lower_limit", 6);
	rules.margin_rate = {exact("long_margin_rate", 8), exact("short_margin_rate", 8)};
	const char* names[] = {"open", "today", "yesterday"};
	for (size_t index = 0; index != 3; ++index)
	{
		std::string prefix = names[index];
		rules.fees[index] = {exact((prefix + "_fee_per_lot").c_str(), 6), exact((prefix + "_fee_rate").c_str(), 8)};
	}
	return rules;
}

int TraderMocker::logout()
{
	return 0;
}

int TraderMocker::orderAction(WTSEntrustAction* action)
{
	if (_paper) controlled_owner();
	action->retain();
	
	_io_service.post([this, action](){
		StdUniqueLock lck(_mtx_awaits, std::defer_lock); if (!_paper) lck.lock();	//一定要把awaits锁起来,不然可能会导致一边撮合一边撤单
		WTSOrderInfo* ordInfo = _awaits ? (WTSOrderInfo*)_awaits->grab(action->getOrderID()) : nullptr;

		/*
		 *	撤单也要考虑几个问题
		 *	1、是否处于可以撤销的状态
		 *	2、如果是开仓,则直接撤销
		 *	3、如果是平仓,要释放冻结
		 */
		if(ordInfo == NULL)
		{
			write_log(_listener,LL_ERROR, "订单{}不存在或者已完成", action->getOrderID());
			WTSError* err = WTSError::create(WEC_ORDERCANCEL, "订单不存在或者处于不可撤销状态");
			if (_listener)
				_listener->onTraderError(err);
			err->release();
			action->release();
			return;
		}

		WTSContractInfo* ct = ordInfo->getContractInfo();
		WTSCommodityInfo* commInfo = ct->getCommInfo();

		bool bPass = false;
		do 
		{
			if (_paper)
			{
				_paper->cancel(ordInfo->getEntrustID());
				bPass = true;
				break;
			}
			//开仓委托直接撤单
			if (ordInfo->getOffsetType() == WOT_OPEN)
			{
				bPass = true;
				break;
			}
			
			//不区分开平的,也直接撤销
			if (commInfo->getCoverMode() == CM_None)
			{
				bPass = true;
				break;
			}

			//释放冻结持仓
			PosItem& pItem = _positions[ct->getFullCode()];
			bool isLong = ordInfo->getDirection() == WDT_LONG;
			if(isLong)
			{
				pItem._long._frozen -= ordInfo->getVolLeft();
			}
			else
			{
				pItem._short._frozen -= ordInfo->getVolLeft();
			}
			bPass = true;

		} while (false);

		ordInfo->setStateMsg("撤单成功");
		ordInfo->setOrderState(WOS_Canceled);
		//ordInfo->setVolLeft(0);	

		if (_listener)
		{
			StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();
			_listener->onPushOrder(ordInfo);
		}

		_awaits->remove(action->getOrderID());
		ordInfo->release();
		action->release();

		save_positions();
	});

	return 0;
}

int TraderMocker::queryAccount()
{
	if (_paper) controlled_owner();
	_io_service.post([this](){
		WTSArray* ay = WTSArray::create();
		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setCurrency("CNY");
		accountInfo->setBalance(0);
		accountInfo->setPreBalance(0);
		accountInfo->setCloseProfit(0);
		accountInfo->setMargin(0);
		accountInfo->setAvailable(0);
		accountInfo->setCommission(0);
		accountInfo->setFrozenMargin(0);
		accountInfo->setFrozenCommission(0);
		accountInfo->setDeposit(0);
		accountInfo->setWithdraw(0);
		accountInfo->setDynProfit(0);
		if (_paper)
		{
			auto balance = _paper->balance();
			accountInfo->setBalance(balance.cash / 100.0);
			accountInfo->setCloseProfit(balance.day_realized / 100.0);
			accountInfo->setPreBalance(balance.pre_balance / 100.0);
			accountInfo->setDeposit(balance.deposit / 100.0);
			accountInfo->setMargin(balance.margin / 100.0);
			accountInfo->setAvailable(balance.available / 100.0);
			accountInfo->setCommission(balance.day_fees / 100.0);
			accountInfo->setFrozenMargin(balance.frozen_margin / 100.0);
			accountInfo->setFrozenCommission(balance.frozen_fee / 100.0);
			accountInfo->setDynProfit(balance.unrealized / 100.0);
		}

		ay->append(accountInfo, false);

		if (_listener)
		{
			StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();
			_listener->onRspAccount(ay);
		}

		ay->release();

	});
	return 0;
}

int TraderMocker::queryPositions()
{
	if (_paper) controlled_owner();
	_io_service.post([this](){
		WTSArray* ayPos = WTSArray::create();
		if (_paper)
		{
			const auto& code = _paper->rules().contract;
			auto dot = code.find('.');
			for (bool shortSide : {false, true})
			{
				auto position = WTSPositionItem::create(code.substr(dot + 1).c_str(), "CNY", code.substr(0, dot).c_str());
				position->setDirection(shortSide ? WDT_SHORT : WDT_LONG);
                position->setContractInfo(_bd_mgr->getContract(code.substr(dot + 1).c_str(), code.substr(0, dot).c_str()));
                auto amounts = _paper->position_amounts(shortSide);
                position->setPositionCost(amounts.cost / 100.0);
                position->setMargin(amounts.margin / 100.0);
                position->setDynProfit(amounts.unrealized / 100.0);
                position->setAvgPrice(amounts.quantity ? amounts.cost / 100.0 / amounts.quantity / _paper->rules().multiplier : 0);

				position->setNewPosition(static_cast<double>(_paper->position(shortSide, PaperAccount::Today, false)));
				position->setPrePosition(static_cast<double>(_paper->position(shortSide, PaperAccount::Yesterday, false)));
				position->setAvailNewPos(static_cast<double>(_paper->position(shortSide, PaperAccount::Today, true)));
				position->setAvailPrePos(static_cast<double>(_paper->position(shortSide, PaperAccount::Yesterday, true)));
				ayPos->append(position, false);
			}
		}

		for(auto& v : _positions)
		{
			const PosItem& pItem = v.second;

			WTSContractInfo* ct = _bd_mgr->getContract(pItem._code, pItem._exchg);
			if(ct == NULL)
				continue;

			WTSCommodityInfo* commInfo = ct->getCommInfo();

			if(pItem._long._volume > 0)
			{
				WTSPositionItem* pInfo = WTSPositionItem::create(pItem._code, commInfo->getCurrency(), pItem._exchg);
				pInfo->setContractInfo(ct);
				pInfo->setDirection(WDT_LONG);
				pInfo->setNewPosition(pItem._long._volume);
				pInfo->setAvailNewPos(pItem._long._volume - pItem._long._frozen);

				ayPos->append(pInfo, false);
			}

			if (pItem._short._volume > 0)
			{
				WTSPositionItem* pInfo = WTSPositionItem::create(pItem._code, commInfo->getCurrency(), pItem._exchg);
				pInfo->setContractInfo(ct);
				pInfo->setDirection(WDT_SHORT);
				pInfo->setNewPosition(pItem._short._volume);
				pInfo->setAvailNewPos(pItem._short._volume - pItem._short._frozen);

				ayPos->append(pInfo, false);
			}
		}

		if (_listener)
		{
			StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();
			_listener->onRspPosition(ayPos);
		}
		ayPos->release();
	});

	return 0;
}

int64_t TraderMocker::paper_price(double price)
{
	if (!std::isfinite(price) || price <= 0 || price > static_cast<double>(INT64_MAX / 1000000))
		throw std::invalid_argument("Invalid native paper price");
	return static_cast<int64_t>(std::round(price * 1000000));
}

void TraderMocker::controlled_owner() const
{
	if (!_paper || std::this_thread::get_id() != _owner)
		throw std::logic_error("Controlled paper must run on its owning event thread");
}

void TraderMocker::controlled_barrier()
{
	controlled_owner();
	if (_draining) throw std::logic_error("Reentrant controlled event barrier");
	_draining = true;
	try { _io_service.reset(); _io_service.poll(); }
	catch (...) { _draining = false; throw; }
	_draining = false;
}

void TraderMocker::controlled_step(uint64_t input_seq, uint64_t event_ms, WTSTickData* tick)
{
	controlled_owner();
	if (_draining || !_controlled_connected || input_seq != _input_seq + 1 || event_ms == 0 || event_ms < _event_ms)
		throw std::invalid_argument("Invalid controlled event sequence, time or connection");
	if (tick)
	{
		if (_paper->settled() || tick->tradingdate() != _paper->rules().trading_day
			|| std::string(tick->exchg()) + "." + tick->code() != _paper->rules().contract)
			throw std::invalid_argument("Controlled tick contract or trading day mismatch");
		auto validatePrice = [&](double value) {
			auto fixed = paper_price(value);
			const auto& rule = _paper->rules();
			if (fixed < rule.lower_limit || fixed > rule.upper_limit || fixed % rule.tick)
				throw std::invalid_argument("Controlled quote outside price rules");
		};
		validatePrice(tick->price());
		for (auto pair : {std::make_pair(tick->askqty(0), tick->askprice(0)), std::make_pair(tick->bidqty(0), tick->bidprice(0))})
		{
			if (!std::isfinite(pair.first) || pair.first < 0 || pair.first > UINT32_MAX || std::floor(pair.first) != pair.first)
				throw std::invalid_argument("Controlled liquidity must be nonnegative whole lots");
			if (pair.first > 0) validatePrice(pair.second);
		}
	}
	_event_ms = event_ms;
	controlled_barrier(); // Orders from the preceding event enter before this quote.
	_draining = true;
	try
	{
		if (tick)
		{
			_paper->mark(paper_price(tick->price()));
			if (!_ticks) _ticks = TickCache::create();
			_ticks->add(_paper->rules().contract, tick);
			match_once();
			_ticks->clear(); // A quote is consumed even when there were no orders.
		}
		_input_seq = input_seq;
	}
	catch (...) { _draining = false; throw; }
	_draining = false;
}

void TraderMocker::controlled_settle(uint32_t trading_day, int64_t official_price)
{
	controlled_barrier();
	_paper->settle(trading_day, official_price);
	if (!_orders) return;
	_draining = true;
	try
	{
	for (uint32_t index = 0; index != _orders->size(); ++index)
	{
		auto order = static_cast<WTSOrderInfo*>(_orders->at(index));
		if (!_awaits->get(order->getOrderID())) continue;
		order->setOrderState(WOS_Canceled); order->setStateMsg("DAY expired at settlement");
		_awaits->remove(order->getOrderID());
		if (_listener) _listener->onPushOrder(order);
	}
	}
	catch (...) { _draining = false; throw; }
	_draining = false;
}

void TraderMocker::controlled_begin_day(PaperAccount::Rules rules, int64_t mark)
{
	controlled_barrier();
	_paper->begin_day(std::move(rules), mark);
}

int TraderMocker::queryOrders()
{
	if (_paper) controlled_owner();
	_io_service.post([this](){
		StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();

		if (_listener)
			_listener->onRspOrders(_orders);
	});

	return 0;
}

int TraderMocker::queryTrades()
{
	if (_paper) controlled_owner();
	_io_service.post([this](){
		StdUniqueLock lock(_mutex_api, std::defer_lock); if (!_paper) lock.lock();

		if (_listener)
			_listener->onRspTrades(_trades);
	});

	return 0;
}

void TraderMocker::handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad /* = true */)
{
	if (e)
	{
		if (_listener)
			write_log(_listener,LL_ERROR, "[TraderMocker]UDP行情接收出错:{}({})", e.message().c_str(), e.value());

		if (!_terminated)
		{
			std::this_thread::sleep_for(std::chrono::seconds(2));
			reconn_udp();
			return;
		}
	}

	if (_terminated || bytes_transferred <= 0)
		return;

	extract_buffer(bytes_transferred, isBroad);

	if (isBroad && _b_socket)
	{
		_b_socket->async_receive_from(boost::asio::buffer(_b_buffer), _broad_ep,
			boost::bind(&TraderMocker::handle_read, this,
			boost::asio::placeholders::error,
			boost::asio::placeholders::bytes_transferred, true));
	}
}

#define UDP_MSG_PUSHTICK	0x200
#pragma pack(push,1)

typedef struct UDPPacketHead
{
	uint32_t		_type;
} UDPPacketHead;
//UDP请求包
typedef struct _UDPReqPacket : UDPPacketHead
{
	char			_data[1020];
} UDPReqPacket;

//UDPTick数据包
template <typename T>
struct UDPDataPacket : UDPPacketHead
{
	T			_data;
};
#pragma pack(pop)
typedef UDPDataPacket<WTSTickStruct>	UDPTickPacket;
void TraderMocker::extract_buffer(uint32_t length, bool isBroad /* = true */)
{
	UDPPacketHead* header = (UDPTickPacket*)_b_buffer.data();

	if (header->_type == UDP_MSG_PUSHTICK)
	{
		UDPTickPacket* packet = (UDPTickPacket*)header;
		thread_local static char fullcode[64] = { 0 };
		fmtutil::format_to(fullcode, "{}.{}", packet->_data.exchg, packet->_data.code);
		auto it = _codes.find(fullcode);
		if (it == _codes.end())
			return;

		WTSTickData* curTick = WTSTickData::create(packet->_data);
		
		if (_ticks == NULL)
			_ticks = TickCache::create();

		_ticks->add(fullcode, curTick, false);

		_max_tick_time = max(_max_tick_time, (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime());
	}
}
