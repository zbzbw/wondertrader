#include "TraderAdapter.h"
#include "../Includes/WTSTradeDef.hpp"
#include <cmath>
#include <stdexcept>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

USING_NS_WTP;
namespace {
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
void number(Writer& w, double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite native trader report field");
    w.Double(value);
}
void writeRecord(Writer& w, const WTSObject* value) {
    if (!value) { w.Null(); return; }
    if (auto array = dynamic_cast<const WTSArray*>(value)) {
        w.StartArray(); for (auto item : *array) writeRecord(w, item); w.EndArray(); return;
    }
    w.StartObject();
    // Preserve every native record field before legacy normalization. Member
    // names deliberately follow WT's ABI so no broker fact is inferred here.
#define S(member) w.Key(#member); w.String(p->member)
#define D(member) w.Key(#member); number(w, p->member)
#define I(member) w.Key(#member); w.Int(static_cast<int>(p->member))
#define U(member) w.Key(#member); w.Uint64(p->member)
#define B(member) w.Key(#member); w.Bool(p->member)
    if (auto p = dynamic_cast<const WTSEntrust*>(value)) {
        S(m_strExchg); S(m_strCode); D(m_dVolume); D(m_iPrice); B(m_bIsNet); B(m_bIsBuy);
        I(m_direction); I(m_priceType); I(m_orderFlag); I(m_offsetType);
        S(m_strEntrustID); S(m_strUserTag); I(m_businessType); S(m_extras);
    } else if (auto p = dynamic_cast<const WTSOrderInfo*>(value)) {
        S(m_strExchg); S(m_strCode); D(m_dVolume); D(m_iPrice); B(m_bIsNet); B(m_bIsBuy);
        I(m_direction); I(m_priceType); I(m_orderFlag); I(m_offsetType);
        S(m_strEntrustID); S(m_strUserTag); I(m_businessType); I(m_uErrorFlag);
        U(m_uInsertDate); U(m_uInsertTime); D(m_dVolTraded); D(m_dVolLeft);
        I(m_orderState); I(m_orderType); S(m_strOrderID); S(m_strStateMsg);
    } else if (auto p = dynamic_cast<const WTSTradeInfo*>(value)) {
        S(m_strExchg); S(m_strCode); S(m_strTradeID); S(m_strRefOrder); S(m_strUserTag);
        U(m_uTradeDate); U(m_uTradeTime); D(m_dVolume); D(m_dPrice); B(m_bIsNet); B(m_bIsBuy);
        I(m_direction); I(m_offsetType); I(m_orderType); I(m_tradeType); D(m_uAmount); I(m_businessType);
    } else if (auto p = dynamic_cast<const WTSPositionItem*>(value)) {
        S(m_strExchg); S(m_strCode); S(m_strCurrency); I(m_direction);
        D(m_dInitPosition); D(m_dPrePosition); D(m_dNewPosition); D(m_dApplyPosition);
        D(m_dAvailPrePos); D(m_dAvailNewPos); D(m_dTotalPosCost); D(m_dMargin);
        D(m_dAvgPrice); D(m_dDynProfit); I(m_businessType);
    } else if (auto p = dynamic_cast<const WTSAccountInfo*>(value)) {
        S(m_strCurrency); D(m_dBalance); D(m_dPreBalance); D(m_uMargin); D(m_dCommission);
        D(m_dFrozenMargin); D(m_dFrozenCommission); D(m_dCloseProfit); D(m_dDynProfit);
        D(m_dDeposit); D(m_dWithdraw); D(m_dAvailable);
    } else if (auto error = dynamic_cast<const WTSError*>(value)) {
        w.Key("code"); w.Int(error->getErrorCode()); w.Key("message"); w.String(error->getMessage());
    } else throw std::invalid_argument("Unsupported native trader report");
#undef S
#undef D
#undef I
#undef U
#undef B
    w.EndObject();
}
}

void TraderAdapter::recordLiveReport(const char* kind, const WTSObject* payload, const WTSObject* error) {
    liveIdentity(_live_run, _live_generation);
    if (_live_report_seq == UINT64_MAX) throw std::overflow_error("Native report sequence exhausted");
    rapidjson::StringBuffer buffer; Writer w(buffer);
    w.StartObject(); w.Key("sequence"); w.Uint64(_live_report_seq + 1);
    w.Key("kind"); w.String(kind); w.Key("payload"); writeRecord(w, payload);
    w.Key("error"); writeRecord(w, error); w.EndObject();
    _live_reports.emplace(_live_report_seq + 1, buffer.GetString()); ++_live_report_seq;
    if (_live_report) _live_report(kind, payload, error);
}

std::string TraderAdapter::liveReports() const {
    liveIdentity(_live_run, _live_generation);
    rapidjson::StringBuffer buffer; Writer w(buffer);
    w.StartObject(); w.Key("source_session"); w.String(_live_source.c_str());
    w.Key("acknowledged"); w.Uint64(_live_report_ack); w.Key("last"); w.Uint64(_live_report_seq);
    w.Key("reports"); w.StartArray();
    for (const auto& entry : _live_reports) w.RawValue(entry.second.c_str(), entry.second.size(), rapidjson::kObjectType);
    w.EndArray(); w.EndObject(); return buffer.GetString();
}

void TraderAdapter::acknowledgeLiveReports(uint64_t sequence) {
    liveIdentity(_live_run, _live_generation);
    if (sequence < _live_report_ack || sequence > _live_report_seq)
        throw std::invalid_argument("Native report acknowledgement is outside the retained waterline");
    _live_reports.erase(_live_reports.begin(), _live_reports.upper_bound(sequence));
    _live_report_ack = sequence;
}
