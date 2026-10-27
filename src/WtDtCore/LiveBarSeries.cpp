#include "LiveBarSeries.h"
#include <algorithm>
#include <stdexcept>

void LiveBarCoverage::add(bool p, bool g, uint32_t reason, uint64_t first, uint64_t last)
{
    partial |= p;
    gap |= g;
    reasons |= reason;
    first_ns = first_ns ? std::min(first_ns, first) : first;
    last_ns = std::max(last_ns, last);
    for (size_t i = 0; i < _observations.size(); ++i) {
        if (!(reason & (1u << i))) continue;
        auto& o = _observations[i];
        o.first = o.present ? std::min(o.first, first) : first;
        o.last = std::max(o.last, last);
        o.partial |= p; o.gap |= g; o.present = true;
    }
}

LiveBarCoverage LiveBarCoverage::until(uint64_t through) const
{
    LiveBarCoverage result;
    for (size_t i = 0; i < _observations.size(); ++i) {
        const auto& o = _observations[i];
        if (o.present && o.first <= through)
            result.add(o.partial, o.gap, 1u << i, o.first, std::min(o.last, through));
    }
    return result;
}

LiveBarSeries::LiveBarSeries(const std::string& code, wtp::WTSSessionInfo* session,
    uint32_t minutes, size_t history_count, uint64_t delay)
    : _code(code), _session(session), _history_count(history_count), _delay(delay)
{
    if (!session || code.empty() || code.size() > 31 || !history_count || delay > 60000000000ULL)
        throw std::invalid_argument("invalid live bar configuration");
    if (minutes != 0 && minutes != 1 && minutes != 5 && minutes != 15 &&
        minutes != 30 && minutes != 60 && minutes != 120)
        throw std::invalid_argument("unsupported live bar frequency");
    _session->retain();
    _kline = wtp::WTSKlineData::create(code.c_str(), 0);
    _kline->setPeriod(minutes ? wtp::KP_Minute1 : wtp::KP_DAY, minutes ? minutes : 1);
}

LiveBarSeries::~LiveBarSeries()
{
    _kline->release();
    _session->release();
}

void LiveBarSeries::setWindows(std::vector<LiveBarWindow> windows, uint64_t joined)
{
    if (_close_index != _windows.size() || !_pending.empty())
        throw std::logic_error("previous live bar calendar is not drained");
    uint64_t previous = _windows.empty() ? 0 : _windows.back().end_ns;
    _previous_calendar_end = previous;
    for (const auto& window : windows) {
        if (window.start_ns < previous || window.end_ns <= window.start_ns)
            throw std::invalid_argument("live bar windows must be ordered and disjoint");
        previous = window.end_ns;
    }
    _windows = std::move(windows);
    _close_index = 0;
    _joined = joined;
    // Starting partway through an interval cannot prove earlier coverage.
    for (size_t i = 0; i < _windows.size(); ++i)
        if (_windows[i].start_ns < joined && joined < _windows[i].end_ns)
            pending(i).coverage.add(true, false, BarJoined, _windows[i].start_ns, joined);
}

LiveBarInterval& LiveBarSeries::pending(size_t index)
{
    auto found = _pending.find(index);
    if (found != _pending.end()) return found->second;
    LiveBarInterval value;
    static_cast<LiveBarWindow&>(value) = _windows.at(index);
    value.preceding_start_ns = index ? _windows[index - 1].end_ns :
        (_previous_calendar_end ? _previous_calendar_end : std::min(_joined, value.start_ns));
    return _pending.emplace(index, value).first->second;
}

bool LiveBarSeries::tick(wtp::WTSTickData* tick, uint64_t event, uint64_t received)
{
    if (!_session->isInTradingTime(tick->actiontime() / 100000)) return false;
    // WTS assigns a section-end tick (including its millisecond fraction) to
    // the last interval. Ordinary minute boundaries start the following bar.
    auto it = std::upper_bound(_windows.begin(), _windows.end(), event,
        [](uint64_t at, const LiveBarWindow& w) { return at < w.start_ns; });
    if (it == _windows.begin()) return false;
    --it;
    size_t index = static_cast<size_t>(it - _windows.begin());
    const bool section_end = _session->isLastOfSection(tick->actiontime() / 100000);
    if (event >= it->end_ns && !(section_end && event < it->end_ns + 60000000000ULL))
        return false;
    if (index < _close_index || it->trading_day != tick->tradingdate()) return false;

    auto& interval = pending(index);
    // Only a single WTS bar is retained for each update. Pending closed-label
    // intervals are independent copies until the shared delay expires.
    auto& data = _kline->getDataRef();
    data.clear();
    if (interval.bar) data.push_back(*interval.bar);
    auto added = _factory.updateKlineData(_kline, tick, _session, true);
    // WTS's daily Tick entry returns an owned new bar, unlike its minute entry.
    if (_kline->period() == wtp::KP_DAY && added) {
        data.push_back(*added);
        delete added;
    }
    if (data.empty()) return false;
    interval.bar = data.back();
    interval.event_ns = event;
    interval.received_ns = received;
    return true;
}

void LiveBarSeries::mark(bool partial, bool gap, uint32_t reason, uint64_t first, uint64_t last)
{
    if (last < first) throw std::invalid_argument("reversed live bar coverage range");
    for (size_t i = _close_index; i < _windows.size(); ++i) {
        const auto& w = _windows[i];
        const auto preceding = i ? _windows[i - 1].end_ns :
            (_previous_calendar_end ? _previous_calendar_end : std::min(_joined, w.start_ns));
        if (preceding > last) break;
        if (first < w.start_ns && last >= preceding)
            pending(i).preceding_coverage.add(partial, gap, reason,
                std::max(first, preceding), std::min(last, w.start_ns));
        if (w.start_ns > last) break;
        if (w.end_ns > first)
            pending(i).coverage.add(partial, gap, reason,
                std::max(first, w.start_ns), std::min(last, w.end_ns));
    }
}

bool LiveBarSeries::advance(uint64_t now, size_t budget)
{
    const uint64_t through = now > _delay ? now - _delay : 0;
    while (_close_index < _windows.size() && _windows[_close_index].end_ns <= through) {
        if (!budget--) return false;
        auto value = pending(_close_index);
        if (value.end_ns > _joined) {
            value.sequence = ++_sequence;
            _history.push_back(value);
            if (_history.size() > _history_count) _history.pop_front();
        }
        _processed = std::max(_processed, value.end_ns);
        _pending.erase(_close_index++);
    }
    _processed = std::max(_processed, through);
    return true;
}

LiveBarPage LiveBarSeries::read(uint64_t after, uint64_t through) const
{
    LiveBarPage page;
    page.earliest = _history.empty() ? _sequence + 1 : _history.front().sequence;
    page.next = after;
    page.processed_through_ns = _processed;
    page.expired = after < page.earliest - 1 || after > _sequence;
    if (page.expired) return page;
    for (const auto& interval : _history) {
        if (interval.sequence <= after) continue;
        if (interval.end_ns > through) break;
        if (page.interval) { page.has_more = true; break; }
        page.interval = interval;
        page.next = interval.sequence;
    }
    // State at T belongs to the same bounded interval history as OHLC. This
    // also covers a lower frequency whose bar has not closed yet, and a rest
    // period before the next interval. Later gaps never contaminate an old T.
    auto at = [&](const LiveBarInterval& value) {
        if (value.start_ns < through && through <= value.end_ns)
            page.coverage = value.coverage.until(through);
        else if (value.preceding_start_ns <= through && through < value.start_ns)
            page.coverage = value.preceding_coverage.until(through);
        else return false;
        return true;
    };
    for (const auto& value : _history) if (at(value)) return page;
    for (const auto& item : _pending) if (at(item.second)) return page;
    return page;
}

std::optional<LiveBarInterval> LiveBarSeries::current() const
{
    if (_pending.empty()) return std::nullopt;
    return _pending.rbegin()->second;
}
