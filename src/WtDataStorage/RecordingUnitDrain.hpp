#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct RecordingUnitProgress
{
	std::string session_id;
	std::string fullcode;
	uint32_t trading_date;
	uint64_t accepted;
	uint64_t completed;
	uint64_t persisted;
	bool failed;

	RecordingUnitProgress()
		: trading_date(0), accepted(0), completed(0), persisted(0), failed(false)
	{
	}
};

// Called while WtDataWriter::_task_mtx is held. This makes accepting a tick
// and freezing a session one ordered boundary without stopping other sessions.
class RecordingUnitDrain
{
public:
	static std::string key(
		const std::string& session_id,
		const std::string& fullcode,
		uint32_t trading_date)
	{
		return session_id + "|" + fullcode + "|" + std::to_string(trading_date);
	}

	bool accept(
		const std::string& session_id,
		const std::string& fullcode,
		uint32_t trading_date,
		std::string& unit_key)
	{
		if (_closing_sessions.count(session_id) != 0)
			return false;
		unit_key = key(session_id, fullcode, trading_date);
		RecordingUnitProgress& unit = _units[unit_key];
		unit.session_id = session_id;
		unit.fullcode = fullcode;
		unit.trading_date = trading_date;
		unit.accepted++;
		return true;
	}

	void complete(const std::string& unit_key, bool persisted)
	{
		auto it = _units.find(unit_key);
		if (it == _units.end())
			return;
		it->second.completed++;
		if (persisted)
			it->second.persisted++;
		else
			it->second.failed = true;
	}

	void closeSession(const std::string& session_id)
	{
		_closing_sessions.insert(session_id);
	}

	void openSession(const std::string& session_id)
	{
		_closing_sessions.erase(session_id);
		for (auto it = _units.begin(); it != _units.end();)
		{
			if (it->second.session_id == session_id)
				it = _units.erase(it);
			else
				++it;
		}
	}

	bool isClosing(const std::string& session_id) const
	{
		return _closing_sessions.count(session_id) != 0;
	}

	bool isDrained(const std::string& session_id) const
	{
		for (const auto& item : _units)
		{
			const RecordingUnitProgress& unit = item.second;
			if (unit.session_id == session_id && unit.completed != unit.accepted)
				return false;
		}
		return true;
	}

	std::vector<RecordingUnitProgress> units(const std::string& session_id) const
	{
		std::vector<RecordingUnitProgress> result;
		for (const auto& item : _units)
		{
			if (item.second.session_id == session_id)
				result.emplace_back(item.second);
		}
		return result;
	}

private:
	std::set<std::string> _closing_sessions;
	std::map<std::string, RecordingUnitProgress> _units;
};
