#include "DataManager.h"
#include "LiveBarCalendar.h"
#include "LiveBarWire.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/CodeHelper.hpp"
#include "../WTSTools/WTSLogger.h"
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <set>

bool DataManager::initBars(WTSVariant* config, uint64_t now)
{
    if (!config) return true;
    std::lock_guard<std::mutex> lock(_latest_mtx);
    try {
        if (!_bar_series.empty()) throw std::invalid_argument("bars already initialized");
        auto instruments = config->get("instruments");
        auto frequencies = config->get("frequencies");
        if (!instruments || !frequencies || frequencies->type() != WTSVariant::VT_Array)
            throw std::invalid_argument("missing bar instruments or frequencies");
        const bool all = instruments->type() == WTSVariant::VT_String && instruments->asString() == "all-active";
        if (!all && instruments->type() != WTSVariant::VT_Array)
            throw std::invalid_argument("invalid bar instrument selection");
        std::set<std::string> selected, periods;
        if (!all) for (uint32_t i = 0; i < instruments->size(); ++i) selected.insert(instruments->get(i)->asString());
        for (uint32_t i = 0; i < frequencies->size(); ++i) {
            const auto frequency = frequencies->get(i)->asString();
            LiveBarWire::minutes(frequency);
            periods.insert(frequency);
        }
        const auto history = config->getUInt32("history_count");
        if (periods.empty() || !history || history > 100000) throw std::invalid_argument("invalid bar history size");
        if (config->has("close_delay_ns")) {
            rapidjson::Value delay(config->getCString("close_delay_ns"),
                static_cast<rapidjson::SizeType>(strlen(config->getCString("close_delay_ns"))));
            _bar_delay = LiveBarWire::number(delay);
        }
        if (_bar_delay > 60000000000ULL) throw std::invalid_argument("invalid bar closing delay");
        _bar_joined = now;
        _bar_stream = boost::uuids::to_string(boost::uuids::random_generator()());
        std::map<std::string, WTSContractInfo*> contracts;
        auto all_contracts = _bd_mgr->getContracts();
        for (auto i = all_contracts->begin(); i != all_contracts->end(); ++i) {
            auto ct = static_cast<WTSContractInfo*>(*i);
            auto comm = ct->getCommInfo();
            if (!comm || !comm->isFuture()) continue;
            const std::string raw = ct->getCode(), product = comm->getProduct();
            if (raw.compare(0, product.size(), product) != 0 || raw.size() <= product.size()) continue;
            const auto month = raw.substr(product.size());
            if ((month.size() != 3 && month.size() != 4) || month.find_first_not_of("0123456789") != std::string::npos) continue;
            auto code = CodeHelper::rawMonthCodeToStdCode(ct->getCode(), ct->getExchg());
            // CZCE's three-digit raw month uses the contract expiry year, not the host decade.
            if (strlen(ct->getCode()) == strlen(comm->getProduct()) + 3) {
                code = std::string(comm->getFullPid()) + "." +
                    std::to_string(ct->getExpireDate() / 10000 % 100) + raw.substr(raw.size() - 2);
            }
            if (!all && !selected.count(code)) continue;
            const auto day = _bd_mgr->calcTradingDate(comm->getFullPid(), LiveBarCalendar::date(now), LiveBarCalendar::hhmm(now));
            if (ct->getOpenDate() > day || ct->getExpireDate() < day) continue;
            contracts.emplace(code, ct);
        }
        all_contracts->release();
        if (all && contracts.empty()) throw std::invalid_argument("no current physical futures contracts for bars");
        if (!all && contracts.size() != selected.size()) throw std::invalid_argument("bar selection has missing or expired contracts");
        for (const auto& item : contracts) {
            const auto& code = item.first;
            auto ct = item.second;
            if (code.size() > 31) throw std::invalid_argument("bar code exceeds wire limit");
            auto comm = ct->getCommInfo();
            auto& input = _bar_inputs[ct->getFullCode()];
            input.contract = ct;
            input.status_since = now;
            uint32_t day = _bd_mgr->calcTradingDate(comm->getFullPid(), LiveBarCalendar::date(now), LiveBarCalendar::hhmm(now));
            if (!_bd_mgr->isTradingDate(comm->getFullPid(), day)) day = _bd_mgr->getNextTDate(comm->getFullPid(), day);
            for (const auto& period : periods) {
                BarEntry entry;
                entry.input = &input;
                entry.day = day;
                entry.minutes = LiveBarWire::minutes(period);
                entry.value = std::make_unique<LiveBarSeries>(code, comm->getSessionInfo(), entry.minutes, history, _bar_delay);
                entry.value->setWindows(LiveBarCalendar::windows(*_bd_mgr, *ct, day, entry.minutes), now);
                input.series.push_back(_bar_series.size());
                _bar_index[{code, period}] = _bar_series.size();
                _bar_names.emplace_back(code, period);
                _bar_series.push_back(std::move(entry));
            }
        }
        WTSLogger::info("Shared bars configured: {} contracts, {} series", contracts.size(), _bar_series.size());
        return true;
    } catch (const std::exception& error) {
        WTSLogger::error("Shared bars initialization failed: {}", error.what());
        return false;
    }
}

void DataManager::markInput(BarInput& input, uint64_t now)
{
    uint32_t reason = (!input.connected ? BarDisconnected : 0) | (!input.subscribed ? BarSubscription : 0);
    if (reason && now >= input.status_since)
        for (auto index : input.series)
            _bar_series[index].value->mark(false, true, reason, input.status_since, now);
}

void DataManager::markRejected(const std::string& code, uint32_t reason, uint64_t now)
{
    auto found = _bar_inputs.find(code);
    if (found != _bar_inputs.end())
        for (auto index : found->second.series)
            _bar_series[index].value->mark(false, true, reason, now, now);
}

void DataManager::advanceSeries(BarEntry& entry, uint64_t now)
{
    auto& series = *entry.value;
    auto& input = *entry.input;
    // Never claim processing past a calendar that has not been installed yet.
    const uint64_t close = series.calendarEnd() + _bar_delay;
    markInput(input, std::min(now, close));
    if (!series.advance(std::min(now, close))) return;
    if (series.drained() && now >= close) {
        entry.day = _bd_mgr->getNextTDate(input.contract->getFullPid(), entry.day);
        series.setWindows(LiveBarCalendar::windows(*_bd_mgr, *input.contract, entry.day, entry.minutes), _bar_joined);
        markInput(input, now);
        series.advance(std::min(now, series.calendarEnd() + _bar_delay));
    }
}

void DataManager::advanceBars(uint64_t now)
{
    std::lock_guard<std::mutex> lock(_latest_mtx);
    if (now < _bar_clock_ns) {
        for (auto& item : _bar_inputs) markRejected(item.first, BarRejected, _bar_clock_ns);
        return;
    }
    _bar_clock_ns = now;
    const size_t count = std::min<size_t>(256, _bar_series.size());
    for (size_t i = 0; i < count; ++i) {
        auto& entry = _bar_series[_bar_clock_index];
        advanceSeries(entry, now);
        _bar_clock_index = (_bar_clock_index + 1) % _bar_series.size();
    }
}

std::string DataManager::queryBars(const char* packet, size_t length)
{
    LiveBarWire::Request request;
    if (length >= 4)
        for (int i = 0; i < 4; ++i) request.type |= uint32_t(static_cast<unsigned char>(packet[i])) << (i * 8);
    try { request = LiveBarWire::parse(packet, length); }
    catch (const std::invalid_argument&) { return LiveBarWire::error(request, _bar_stream, "request_invalid"); }
    LiveBarPage page;
    std::optional<LiveBarInterval> current;
    std::vector<std::pair<std::string, std::string>> names;
    WTSContractInfo* contract = nullptr;
    std::string stream;
    const char* error = nullptr;
    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lock(_latest_mtx);
        stream = _bar_stream;
        if (stream.empty()) error = "bars_disabled";
        else if (!request.stream.empty() && request.stream != stream) error = "stream_changed";
        else if (request.type == LiveBarWire::Enumerate) {
            total = _bar_names.size();
            if (request.index > total) error = "index_invalid";
            else names.assign(_bar_names.begin() + request.index, _bar_names.begin() + std::min(total, request.index + 8));
        } else {
            const auto found = _bar_index.find({request.code, request.frequency});
            if (found == _bar_index.end()) error = "series_missing";
            else {
                const auto& entry = _bar_series[found->second];
                page = entry.value->read(request.after, request.through_ns);
                current = entry.value->current();
                contract = entry.input->contract;
                page.connected = entry.input->connected;
                page.subscribed = entry.input->subscribed;
                page.observed_ns = entry.input->status_since;
            }
        }
    }
    // Only immutable local copies are encoded/sent after releasing market state.
    if (error) return LiveBarWire::error(request, stream, error);
    if (request.type == LiveBarWire::Enumerate) return LiveBarWire::enumeratePage(request, stream, names, total);
    page.session_closed = true;
    const uint64_t at = request.type == LiveBarWire::Current ? LiveBarCalendar::now() : request.through_ns;
    try {
        auto pid = contract->getFullPid();
        auto day = _bd_mgr->calcTradingDate(pid, LiveBarCalendar::date(at), LiveBarCalendar::hhmm(at));
        if (at >= 86400 * LiveBarCalendar::Second && _bd_mgr->isTradingDate(pid, day))
            for (const auto& section : LiveBarCalendar::windows(*_bd_mgr, *contract, day, UINT32_MAX))
                if (section.start_ns <= at && at <= section.end_ns) page.session_closed = false;
    } catch (const std::invalid_argument&) {
        return LiveBarWire::error(request, stream, "request_invalid");
    }
    if (request.type == LiveBarWire::Current)
        return LiveBarWire::current(request, stream, current, page.processed_through_ns, page);
    return LiveBarWire::page(request, stream, page);
}

void DataManager::parserSubscriptions(const std::string& id, const CodeSet& codes, uint64_t now)
{
    std::lock_guard<std::mutex> lock(_latest_mtx);
    for (auto& item : _bar_inputs) {
        auto& input = item.second;
        if (!codes.count(input.contract->getFullCode()) && !codes.count(input.contract->getFullAltCode())) continue;
        if (!input.parser.empty() && input.parser != id)
            throw std::invalid_argument("shared bar instrument has multiple parsers");
        input.parser = id;
        input.status_since = now;
        for (const std::string code : {std::string(input.contract->getCode()),
            std::string(input.contract->getFullCode()), std::string(input.contract->getFullAltCode())})
            _bar_subscriptions[{id, code}] = &input;
    }
}

void DataManager::parserEvent(const std::string& id, WTSParserEvent event, int32_t error, uint64_t now)
{
    std::lock_guard<std::mutex> lock(_latest_mtx);
    for (auto& item : _bar_inputs) {
        auto& input = item.second;
        if (input.parser != id) continue;
        markInput(input, now);
        input.connected = event == WPE_Login && error == 0;
        input.subscribed = false;
        input.status_since = now;
        _tick_states[item.first].disconnected();
    }
}

void DataManager::parserSubscription(const std::string& id, const char* code, bool accepted, uint64_t now)
{
    std::lock_guard<std::mutex> lock(_latest_mtx);
    if (!*code && !accepted) {
        for (auto& item : _bar_inputs) {
            auto& input = item.second;
            if (input.parser != id) continue;
            markInput(input, now);
            input.subscribed = false;
            input.status_since = now;
            _tick_states[item.first].disconnected();
        }
        return;
    }
    const auto found = _bar_subscriptions.find({id, code});
    if (found == _bar_subscriptions.end()) return;
    auto& input = *found->second;
    if (input.subscribed == accepted && input.connected) return;
    markInput(input, now);
    input.subscribed = accepted;
    if (accepted) input.connected = true;
    input.status_since = now;
    if (!accepted) _tick_states[input.contract->getFullCode()].disconnected();
}

void DataManager::rejectTick(const char* code, const char* exchange, uint64_t now)
{
    std::lock_guard<std::mutex> lock(_latest_mtx);
    markRejected(std::string(exchange) + "." + code, BarRejected, now);
}

void DataManager::rejectParserTick(const std::string& id, const char* code, const char* exchange, uint64_t now)
{
    std::lock_guard<std::mutex> lock(_latest_mtx);
    const std::string fullcode = std::string(exchange) + "." + code;
    if (_bar_inputs.count(fullcode)) markRejected(fullcode, BarRejected, now);
    else for (auto& item : _bar_inputs)
        if (item.second.parser == id) markRejected(item.first, BarRejected, now);
}
