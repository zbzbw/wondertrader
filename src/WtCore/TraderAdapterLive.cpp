#include "TraderAdapter.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include <cmath>
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

USING_NS_WTP;

void TraderAdapter::configureLive(const std::string& run, uint64_t generation, const std::string& contract,
                                 std::function<void(CommonExecuter)> queue)
{
    if (_live_controlled || run.empty() || !generation || contract.empty() || !queue)
        throw std::invalid_argument("Controlled trader requires a fresh run, generation and physical contract");
    _live_controlled = true; _live_owner = std::this_thread::get_id();
    _live_run = run; _live_generation = generation; _live_contract = contract;
    _live_source = run;
    _live_queue = std::move(queue);
    _live_enabled = false;
    // Paper restore needs the immutable contract catalogue before connection.
    if (_trader_api) _trader_api->registerSpi(this);
}

bool TraderAdapter::liveDeferred()
{
    if (!_live_controlled) return false;
    if (std::this_thread::get_id() == _live_owner && _live_dispatching) {
        _live_dispatching = false;
        return false;
    }
    return true;
}

void TraderAdapter::deferLive(CommonExecuter action)
{
    _live_queue([this, action = std::move(action)]() {
        liveIdentity(_live_run, _live_generation);
        _live_dispatching = true;
        try { action(); }
        catch (...) { _live_dispatching = false; throw; }
        _live_dispatching = false;
    });
}

std::shared_ptr<WTSObject> TraderAdapter::copyLive(const WTSObject* value)
{
    if (!value) return {};
    auto wrap = [](WTSObject* item) { return std::shared_ptr<WTSObject>(item, [](WTSObject* p) { p->release(); }); };
    if (auto source = dynamic_cast<const WTSArray*>(value)) {
        auto array = WTSArray::create(); auto result = wrap(array);
        for (auto object : *source) { auto copy = copyLive(object); array->append(copy.get(), true); }
        return result;
    }
    if (auto source = dynamic_cast<const WTSOrderInfo*>(value)) {
        auto copy = WTSOrderInfo::create(); static_cast<WTSOrderStruct&>(*copy) = *source;
        copy->setContractInfo(source->getContractInfo()); return wrap(copy);
    }
    if (auto source = dynamic_cast<const WTSTradeInfo*>(value)) {
        auto copy = WTSTradeInfo::create(""); static_cast<WTSTradeStruct&>(*copy) = *source;
        copy->setContractInfo(source->getContractInfo()); return wrap(copy);
    }
    if (auto source = dynamic_cast<const WTSEntrust*>(value)) {
        auto copy = WTSEntrust::create(); static_cast<WTSEntrustStruct&>(*copy) = *source;
        copy->setContractInfo(source->getContractInfo()); return wrap(copy);
    }
    if (auto source = dynamic_cast<const WTSPositionItem*>(value)) {
        auto copy = WTSPositionItem::create(""); static_cast<WTSPositionStruct&>(*copy) = *source;
        copy->setContractInfo(source->getContractInfo()); return wrap(copy);
    }
    if (auto source = dynamic_cast<const WTSAccountInfo*>(value)) {
        auto copy = WTSAccountInfo::create(); static_cast<WTSAccountStruct&>(*copy) = *source; return wrap(copy);
    }
    if (auto source = dynamic_cast<const WTSError*>(value))
        return wrap(WTSError::create(source->getErrorCode(), source->getMessage()));
    throw std::invalid_argument("Unsupported controlled trader report record");
}

void TraderAdapter::liveIdentity(const std::string& run, uint64_t generation) const
{
    if (!_live_controlled || std::this_thread::get_id() != _live_owner
        || run != _live_run || generation != _live_generation)
        throw std::invalid_argument("Controlled trader run, generation or event thread mismatch");
}

void TraderAdapter::armLive(const std::string& run, uint64_t generation)
{
    liveIdentity(run, generation);
    if (!_live_connected || !isReady()) throw std::logic_error("Trader reconciliation is not ready");
    _live_enabled = true;
}

void TraderAdapter::blockLive()
{
    // Also called immediately by disconnect callbacks, before queued reporting.
    _live_enabled = false;
}

bool TraderAdapter::liveSendAllowed(WTSEntrust* entrust) const
{
    return _live_enabled && _live_connected && std::this_thread::get_id() == _live_owner
        && _live_entrust == entrust && _live_command && _live_command->status == "prepared";
}

uint32_t TraderAdapter::submitLive(const std::string& run, uint64_t generation,
                                 const std::string& command, WTSEntrust* entrust)
{
    liveIdentity(run, generation);
    if (!entrust || command.empty() || _live_entrust || _live_cancels.count(command))
        throw std::invalid_argument("Invalid or reentrant live command");
    auto contract = entrust->getContractInfo();
    if (!contract || _live_contract != contract->getFullCode()
        || entrust->getPriceType() != WPT_LIMITPRICE || entrust->getOrderFlag() != WOF_NOR || entrust->isNet()
        || (entrust->getDirection() != WDT_LONG && entrust->getDirection() != WDT_SHORT)
        || (entrust->getOffsetType() != WOT_OPEN && entrust->getOffsetType() != WOT_CLOSETODAY && entrust->getOffsetType() != WOT_CLOSEYESTERDAY)
        || !std::isfinite(entrust->getPrice()) || entrust->getPrice() <= 0
        || !std::isfinite(entrust->getVolume()) || entrust->getVolume() <= 0
        || entrust->getVolume() > INT32_MAX || std::floor(entrust->getVolume()) != entrust->getVolume())
        throw std::invalid_argument("Live command requires its physical contract and explicit whole-lot DAY limit intent");
    LiveCommand intent;
    intent.contract = contract->getFullCode(); intent.direction = entrust->getDirection();
    intent.offset = entrust->getOffsetType(); intent.price = entrust->getPrice(); intent.quantity = entrust->getVolume();
    auto existing = _live_commands.find(command);
    if (existing != _live_commands.end()) {
        const auto& old = existing->second;
        if (old.contract != intent.contract || old.direction != intent.direction || old.offset != intent.offset
            || old.price != intent.price || old.quantity != intent.quantity)
            throw std::invalid_argument("Live command identity conflict");
        return old.local_id; // Includes unknown outcomes: query/reconcile, never resend.
    }
    if (!_live_enabled || !_live_connected) throw std::logic_error("Live sends are blocked");
    auto& record = _live_commands.emplace(command, std::move(intent)).first->second;
    _live_entrust = entrust; _live_command = &record;
    try {
        auto result = doEntrust(entrust);
        if (record.status == "prepared") record.status = "rejected";
        _live_entrust = nullptr; _live_command = nullptr;
        return record.local_id == UINT32_MAX ? result : record.local_id;
    } catch (...) {
        if (record.status == "prepared") record.status = "rejected";
        _live_entrust = nullptr; _live_command = nullptr;
        throw;
    }
}

bool TraderAdapter::cancelLive(const std::string& run, uint64_t generation,
                              const std::string& command, const std::string& target)
{
    liveIdentity(run, generation);
    if (command.empty() || _live_cancel_local != UINT32_MAX || _live_commands.count(command))
        throw std::invalid_argument("Invalid or reentrant cancel command");
    auto old = _live_cancels.find(command);
    if (old != _live_cancels.end()) {
        if (old->second.first != target) throw std::invalid_argument("Cancel command identity conflict");
        return old->second.second;
    }
    auto owned = _live_commands.find(target);
    if (!_live_connected || owned == _live_commands.end() || owned->second.local_id == UINT32_MAX)
        throw std::invalid_argument("Cancel requires a connected trader and a known owned command");
    // Cancellation remains allowed after send authority is revoked for stop/hold.
    auto& result = _live_cancels.emplace(command, std::make_pair(target, false)).first->second;
    _live_cancel_local = owned->second.local_id;
    try { result.second = cancel(_live_cancel_local); }
    catch (...) { _live_cancel_local = UINT32_MAX; throw; }
    _live_cancel_local = UINT32_MAX;
    return result.second;
}

std::string TraderAdapter::liveSnapshot() const
{
    liveIdentity(_live_run, _live_generation);
    if (_live_entrust || _live_cancel_local != UINT32_MAX) throw std::logic_error("Live command is still in progress");
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
    auto text = [&](const char* key, const std::string& value) { out.Key(key); out.String(value.c_str()); };
    out.StartObject(); out.Key("version"); out.Uint(1);
    text("contract", _live_contract); text("order_pattern", _order_pattern);
    out.Key("local_counter"); out.Uint(_live_local_order);
    out.Key("commands"); out.StartArray();
    for (const auto& entry : _live_commands) {
        const auto& command = entry.second;
        out.StartObject(); text("id", entry.first); text("entrust_id", command.entrust_id); text("status", command.status);
        out.Key("direction"); out.Int(command.direction); out.Key("offset"); out.Int(command.offset);
        out.Key("price"); out.Double(command.price); out.Key("quantity"); out.Double(command.quantity);
        out.Key("local_id"); out.Uint(command.local_id); out.EndObject();
    }
    out.EndArray(); out.Key("cancels"); out.StartArray();
    for (const auto& entry : _live_cancels) {
        out.StartObject(); text("id", entry.first); text("target", entry.second.first);
        out.Key("accepted"); out.Bool(entry.second.second); out.EndObject();
    }
    out.EndArray();
    auto reports = liveReports(); out.Key("outbox"); out.RawValue(reports.c_str(), reports.size(), rapidjson::kObjectType);
    out.EndObject();
    return std::string(buffer.GetString(), buffer.GetSize());
}

void TraderAdapter::restoreLive(const std::string& state)
{
    liveIdentity(_live_run, _live_generation);
    if (_live_enabled || _live_connected || !_live_commands.empty() || !_live_cancels.empty())
        throw std::logic_error("Command restore requires a fresh disconnected blocked trader");
    auto require = [](bool valid) { if (!valid) throw std::invalid_argument("Invalid live command state"); };
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseFullPrecisionFlag>(state.data(), state.size());
    require(!doc.HasParseError() && doc.IsObject());
    auto field = [&](const rapidjson::Value& value, const char* key) -> const rapidjson::Value& {
        require(value.IsObject() && value.HasMember(key)); return value[key];
    };
    auto text = [&](const rapidjson::Value& value, const char* key) {
        const auto& item = field(value, key); require(item.IsString());
        return std::string(item.GetString(), item.GetStringLength());
    };
    auto number = [&](const rapidjson::Value& value, const char* key) {
        const auto& item = field(value, key); require(item.IsUint()); return item.GetUint();
    };
    require(number(doc, "version") == 1 && text(doc, "contract") == _live_contract
            && text(doc, "order_pattern") == _order_pattern);
    auto counter = number(doc, "local_counter"); require(counter < UINT32_MAX);
    const auto& commands = field(doc, "commands"); const auto& cancels = field(doc, "cancels");
    require(commands.IsArray() && cancels.IsArray());
    std::map<std::string, LiveCommand> restored;
    std::map<std::string, std::pair<std::string, bool>> restoredCancels;
    std::set<uint32_t> ids;
    std::set<std::string> entrustIds;
    for (const auto& value : commands.GetArray()) {
        auto id = text(value, "id"); require(!id.empty());
        LiveCommand command;
        command.contract = _live_contract; command.entrust_id = text(value, "entrust_id");
        command.status = text(value, "status"); command.local_id = number(value, "local_id");
        auto direction = number(value, "direction"), offset = number(value, "offset");
        require(direction == WDT_LONG || direction == WDT_SHORT);
        require(offset == WOT_OPEN || offset == WOT_CLOSETODAY || offset == WOT_CLOSEYESTERDAY);
        command.direction = static_cast<WTSDirectionType>(direction); command.offset = static_cast<WTSOffsetType>(offset);
        const auto& price = field(value, "price"); const auto& quantity = field(value, "quantity");
        require(price.IsNumber() && quantity.IsNumber());
        command.price = price.GetDouble(); command.quantity = quantity.GetDouble();
        require(std::isfinite(command.price) && command.price > 0 && std::isfinite(command.quantity)
                && command.quantity > 0 && command.quantity <= INT32_MAX && std::floor(command.quantity) == command.quantity);
        require(command.status == "submitted" || command.status == "unknown" || command.status == "rejected");
        if (command.local_id != UINT32_MAX)
            require(command.local_id > 0 && command.local_id <= counter && ids.insert(command.local_id).second
                    && !command.entrust_id.empty() && command.entrust_id.size() < 64 && entrustIds.insert(command.entrust_id).second);
        else require(command.status == "rejected" && command.entrust_id.empty());
        require(restored.emplace(id, std::move(command)).second);
    }
    for (const auto& value : cancels.GetArray()) {
        auto id = text(value, "id"), target = text(value, "target");
        const auto& accepted = field(value, "accepted"); require(accepted.IsBool());
        require(!id.empty() && !restored.count(id) && restored.count(target)
                && restored.at(target).local_id != UINT32_MAX);
        require(restoredCancels.emplace(id, std::make_pair(target, accepted.GetBool())).second);
    }
    const auto& outbox = field(doc, "outbox");
    auto source = text(outbox, "source_session"); require(!source.empty());
    const auto& last = field(outbox, "last"); const auto& ack = field(outbox, "acknowledged");
    require(last.IsUint64() && ack.IsUint64() && ack.GetUint64() <= last.GetUint64());
    const auto& reports = field(outbox, "reports"); require(reports.IsArray());
    std::map<uint64_t, std::string> restoredReports;
    auto next = ack.GetUint64();
    for (const auto& report : reports.GetArray()) {
        const auto& seq = field(report, "sequence");
        require(seq.IsUint64() && next < UINT64_MAX && seq.GetUint64() == ++next);
        require(field(report, "kind").IsString()); field(report, "payload"); field(report, "error");
        rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        report.Accept(writer); restoredReports.emplace(next, buffer.GetString());
    }
    require(next == last.GetUint64());
    _live_commands = std::move(restored); _live_cancels = std::move(restoredCancels); _live_local_order = counter;
    _live_source = std::move(source); _live_reports = std::move(restoredReports);
    _live_report_seq = last.GetUint64(); _live_report_ack = ack.GetUint64();
    // Run/generation and all write permissions come from this new process, never the snapshot.
}

std::string TraderAdapter::paperControl(const std::string& request)
{
    liveIdentity(_live_run, _live_generation);
    if (!supportsPaperControl()) throw std::logic_error("Controlled TraderMocker ABI 1 is required");
    return _mocker_live(_trader_api, request.c_str());
}

int TraderAdapter::queryLiveFacts()
{
    liveIdentity(_live_run, _live_generation);
    if (_live_enabled || !_live_connected) throw std::logic_error("Reconciliation requires blocked connected trader");
    _state = AS_LOGINED;
    return _trader_api->queryPositions(); // Existing end callbacks chain orders, trades and account.
}

std::string TraderAdapter::paperStep(uint64_t sequence, uint64_t event_ms, const WTSTickStruct* tick, bool shared_liquidity)
{
    liveIdentity(_live_run, _live_generation);
    if (!supportsPaperControl()) throw std::logic_error("Controlled TraderMocker step ABI 1 is required");
    return _mocker_step(_trader_api, sequence, event_ms, tick, shared_liquidity ? 1 : 0);
}
