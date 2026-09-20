#include "gtest/gtest/gtest.h"

#include "../Share/DLLHelper.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IDataWriter.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StdUtils.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

USING_NS_WTP;

namespace
{
class UnitFactBaseData : public IBaseDataMgr
{
public:
	UnitFactBaseData()
	{
		_shfe_session = WTSSessionInfo::create("SHFE", "SHFE");
		_shfe_session->addTradingSection(900, 1500);
		_shfe_comm = WTSCommodityInfo::create("cu", "copper", "SHFE", "SHFE", "CHINA");
		_shfe_comm->setSessionInfo(_shfe_session);
		_shfe_comm->addCode("cu2609");
		_shfe = WTSContractInfo::create("cu2609", "copper", "SHFE", "cu");
		_shfe->setCommInfo(_shfe_comm);
		_cffex_session = WTSSessionInfo::create("CFFEX", "CFFEX");
		_cffex_session->addTradingSection(900, 1500);
		_cffex_comm = WTSCommodityInfo::create("IF", "index", "CFFEX", "CFFEX", "CHINA");
		_cffex_comm->setSessionInfo(_cffex_session);
		_cffex_comm->addCode("IF2609");
		_cffex = WTSContractInfo::create("IF2609", "index", "CFFEX", "IF");
		_cffex->setCommInfo(_cffex_comm);
	}

	~UnitFactBaseData()
	{
		_shfe->release();
		_shfe_comm->release();
		_cffex->release();
		_cffex_comm->release();
		_shfe_session->release();
		_cffex_session->release();
	}

	WTSCommodityInfo* getCommodity(const char* value) override
	{
		return std::string(value) == "SHFE.cu" ? _shfe_comm : _cffex_comm;
	}
	WTSCommodityInfo* getCommodity(const char* exchg, const char*) override
	{
		return std::string(exchg) == "SHFE" ? _shfe_comm : _cffex_comm;
	}
	WTSContractInfo* getContract(const char* code, const char* exchg, uint32_t) override
	{
		return std::string(exchg) == "SHFE" && std::string(code) == "cu2609"
			? _shfe : _cffex;
	}
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return NULL; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return NULL; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t date, uint32_t, bool) override { return date; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool, bool) override { return 0; }
	uint32_t getContractSize(const char*, uint32_t) override { return 2; }

private:
	WTSCommodityInfo* _shfe_comm;
	WTSCommodityInfo* _cffex_comm;
	WTSSessionInfo* _shfe_session;
	WTSSessionInfo* _cffex_session;
	WTSContractInfo* _shfe;
	WTSContractInfo* _cffex;
};

class UnitFactSink : public IDataWriterSink
{
public:
	UnitFactSink()
	{
		_shfe.insert("SHFE.cu");
		_cffex.insert("CFFEX.IF");
	}

	IBaseDataMgr* getBDMgr() override { return &_base; }
	bool canSessionReceive(const char*) override { return true; }
	void broadcastTick(WTSTickData*) override {}
	void broadcastOrdQue(WTSOrdQueData*) override {}
	void broadcastOrdDtl(WTSOrdDtlData*) override {}
	void broadcastTrans(WTSTransData*) override {}
	CodeSet* getSessionComms(const char* sid) override
	{
		return std::string(sid) == "SHFE" ? &_shfe : &_cffex;
	}
	uint32_t getTradingDate(const char*) override { return 20260915; }
	void outputLog(WTSLogLevel, const char* message) override
	{
		if (message != NULL && strstr(message, "failed") != NULL)
			_failure_logged.store(true);
	}
	bool failureLogged() const { return _failure_logged.load(); }
	WTSContractInfo* contract(const char* exchg)
	{
		return _base.getContract(
			std::string(exchg) == "SHFE" ? "cu2609" : "IF2609", exchg, 0);
	}

private:
	UnitFactBaseData _base;
	CodeSet _shfe;
	CodeSet _cffex;
	std::atomic<bool> _failure_logged{false};
};

WTSTickData* tick(UnitFactSink& sink, const char* exchg, uint32_t sequence)
{
	WTSTickStruct value = {};
	strcpy(value.exchg, exchg);
	strcpy(value.code, std::string(exchg) == "SHFE" ? "cu2609" : "IF2609");
	value.trading_date = 20260915;
	value.action_date = 20260915;
	value.action_time = 90000000 + sequence;
	value.price = 1000.0 + sequence;
	value.total_volume = sequence + 1;
	WTSTickData* result = WTSTickData::create(value);
	result->setContractInfo(sink.contract(exchg));
	return result;
}

WTSVariant* writerConfig(const std::filesystem::path& root)
{
	WTSVariant* config = WTSVariant::createObject();
	config->append("path", (root.string() + "/").c_str());
	config->append("cache", "cache.dmb");
	config->append("async", true);
	config->append("groupsize", (uint32_t)1000);
	config->append("savelog", false);
	config->append("disablehis", false);
	config->append("disabletick", false);
	config->append("disablemin1", true);
	config->append("disablemin5", true);
	config->append("disableday", true);
	config->append("disabletrans", true);
	config->append("disableordque", true);
	config->append("disableorddtl", true);
	return config;
}
}

TEST(test_writer_unit_fact, real_dll_drains_one_session_without_stopping_another)
{
	const char* dll_path = std::getenv("BWT_TEST_WRITER_DLL");
	ASSERT_NE(dll_path, nullptr);
	DllHandle library = DLLHelper::load_library(dll_path);
	ASSERT_NE(library, nullptr);
	auto create = (FuncCreateWriter)DLLHelper::get_symbol(library, "createWriter");
	auto destroy = (FuncDeleteWriter)DLLHelper::get_symbol(library, "deleteWriter");
	ASSERT_NE(create, nullptr);
	ASSERT_NE(destroy, nullptr);

	std::filesystem::path root = std::filesystem::temp_directory_path()
		/ "zbw83-native-unit-fact";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
#ifdef _WIN32
	_putenv_s("BWT_DATAKIT_RUN_ID", "native-test-run");
	_putenv_s("BWT_DATAKIT_SUBSCRIPTION_IDENTITY", "73d8a3c48fd95e85433ce792a1a99bb9f138c64a74f9fd3270ce697cc8a32253");
#else
	setenv("BWT_DATAKIT_RUN_ID", "native-test-run", 1);
	setenv("BWT_DATAKIT_SUBSCRIPTION_IDENTITY", "73d8a3c48fd95e85433ce792a1a99bb9f138c64a74f9fd3270ce697cc8a32253", 1);
#endif

	UnitFactSink sink;
WTSVariant* config = writerConfig(root);

	IDataWriter* writer = create();
	ASSERT_TRUE(writer->init(config, &sink));
	config->release();
	const uint32_t count = 20000;
	for (uint32_t index = 0; index < count; index++)
	{
		WTSTickData* value = tick(sink, "SHFE", index);
		ASSERT_TRUE(writer->writeTick(value, 0));
		value->release();
	}
	writer->beginSessionClose("SHFE");
	WTSTickData* rejected = tick(sink, "SHFE", count + 1);
	EXPECT_FALSE(writer->writeTick(rejected, 0));
	rejected->release();
	WTSTickData* other = tick(sink, "CFFEX", 1);
	EXPECT_TRUE(writer->writeTick(other, 0));
	other->release();
	writer->transHisData("SHFE");

	std::filesystem::path fact = root / "recording" / "units"
		/ "native-test-run" / "20260915" / "SHFE" / "cu2609.json";
	for (uint32_t attempt = 0; attempt < 500 && !std::filesystem::exists(fact); attempt++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	ASSERT_TRUE(std::filesystem::exists(fact));
	std::string document;
	StdFile::read_file_content(fact.string().c_str(), document);
	EXPECT_NE(document.find("\"accepted_offset\":20000"), std::string::npos);
	EXPECT_NE(document.find("\"persisted_offset\":20000"), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(
		root / "rt" / "ticks" / "CFFEX" / "IF2609.dmb"));

	WTSTickData* filtered = tick(sink, "CFFEX", 0);
	EXPECT_TRUE(writer->writeTick(filtered, 0));
	filtered->release();
	writer->beginSessionClose("CFFEX");
	writer->transHisData("CFFEX");
	std::filesystem::path cffex_dsb = root / "his" / "ticks"
		/ "CFFEX" / "20260915" / "IF2609.dsb";
	for (uint32_t attempt = 0;
		attempt < 500 && !std::filesystem::exists(cffex_dsb); attempt++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	ASSERT_TRUE(std::filesystem::exists(cffex_dsb));
	EXPECT_FALSE(std::filesystem::exists(
		root / "recording" / "units" / "native-test-run"
		/ "20260915" / "CFFEX" / "IF2609.json"));
	EXPECT_FALSE(writer->isSessionProceeded("CFFEX"));

	DataWriterStopResult stop;
	writer->stopAndDrain(stop);
	destroy(writer);
	DLLHelper::free_library(library);
}

TEST(test_writer_unit_fact, marker_failure_never_publishes_completion)
{
	const char* dll_path = std::getenv("BWT_TEST_WRITER_DLL");
	ASSERT_NE(dll_path, nullptr);
	DllHandle library = DLLHelper::load_library(dll_path);
	ASSERT_NE(library, nullptr);
	auto create = (FuncCreateWriter)DLLHelper::get_symbol(library, "createWriter");
	auto destroy = (FuncDeleteWriter)DLLHelper::get_symbol(library, "deleteWriter");
	ASSERT_NE(create, nullptr);
	ASSERT_NE(destroy, nullptr);

	std::filesystem::path root = std::filesystem::temp_directory_path()
		/ "zbw83-native-unit-marker-failure";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(root);
#ifdef _WIN32
	_putenv_s("BWT_DATAKIT_RUN_ID", "native-marker-failure");
	_putenv_s("BWT_DATAKIT_SUBSCRIPTION_IDENTITY", "73d8a3c48fd95e85433ce792a1a99bb9f138c64a74f9fd3270ce697cc8a32253");
#else
	setenv("BWT_DATAKIT_RUN_ID", "native-marker-failure", 1);
	setenv("BWT_DATAKIT_SUBSCRIPTION_IDENTITY", "73d8a3c48fd95e85433ce792a1a99bb9f138c64a74f9fd3270ce697cc8a32253", 1);
#endif

	UnitFactSink sink;
	WTSVariant* config = writerConfig(root);
	IDataWriter* writer = create();
	ASSERT_TRUE(writer->init(config, &sink));
	config->release();
	std::filesystem::create_directory(root / "marker.ini");
	WTSTickData* value = tick(sink, "SHFE", 0);
	ASSERT_TRUE(writer->writeTick(value, 0));
	value->release();
	writer->beginSessionClose("SHFE");
	writer->transHisData("SHFE");
	std::filesystem::path dsb = root / "his" / "ticks"
		/ "SHFE" / "20260915" / "cu2609.dsb";
	for (uint32_t attempt = 0;
		attempt < 500 && !std::filesystem::exists(dsb); attempt++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	ASSERT_TRUE(std::filesystem::exists(dsb));
	EXPECT_FALSE(std::filesystem::exists(
		root / "recording" / "units" / "native-marker-failure"
		/ "20260915" / "SHFE" / "cu2609.json"));
	EXPECT_FALSE(writer->isSessionProceeded("SHFE"));

	DataWriterStopResult stop;
	writer->stopAndDrain(stop);
	destroy(writer);
	DLLHelper::free_library(library);
}

TEST(test_writer_unit_fact, history_file_failure_never_publishes_completion)
{
	const char* dll_path = std::getenv("BWT_TEST_WRITER_DLL");
	ASSERT_NE(dll_path, nullptr);
	DllHandle library = DLLHelper::load_library(dll_path);
	ASSERT_NE(library, nullptr);
	auto create = (FuncCreateWriter)DLLHelper::get_symbol(library, "createWriter");
	auto destroy = (FuncDeleteWriter)DLLHelper::get_symbol(library, "deleteWriter");
	ASSERT_NE(create, nullptr);
	ASSERT_NE(destroy, nullptr);

	std::filesystem::path root = std::filesystem::temp_directory_path()
		/ "zbw83-native-unit-file-failure";
	std::filesystem::remove_all(root);
	std::filesystem::create_directories(
		root / "his" / "ticks" / "SHFE" / "20260915" / "cu2609.dsb");
#ifdef _WIN32
	_putenv_s("BWT_DATAKIT_RUN_ID", "native-file-failure");
	_putenv_s("BWT_DATAKIT_SUBSCRIPTION_IDENTITY", "73d8a3c48fd95e85433ce792a1a99bb9f138c64a74f9fd3270ce697cc8a32253");
#else
	setenv("BWT_DATAKIT_RUN_ID", "native-file-failure", 1);
	setenv("BWT_DATAKIT_SUBSCRIPTION_IDENTITY", "73d8a3c48fd95e85433ce792a1a99bb9f138c64a74f9fd3270ce697cc8a32253", 1);
#endif

	UnitFactSink sink;
	WTSVariant* config = writerConfig(root);
	IDataWriter* writer = create();
	ASSERT_TRUE(writer->init(config, &sink));
	config->release();
	WTSTickData* value = tick(sink, "SHFE", 0);
	ASSERT_TRUE(writer->writeTick(value, 0));
	value->release();
	writer->beginSessionClose("SHFE");
	writer->transHisData("SHFE");
	for (uint32_t attempt = 0; attempt < 500 && !sink.failureLogged(); attempt++)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	ASSERT_TRUE(sink.failureLogged());
	EXPECT_FALSE(std::filesystem::exists(
		root / "recording" / "units" / "native-file-failure"
		/ "20260915" / "SHFE" / "cu2609.json"));
	EXPECT_FALSE(writer->isSessionProceeded("SHFE"));

	DataWriterStopResult stop;
	writer->stopAndDrain(stop);
	destroy(writer);
	DLLHelper::free_library(library);
}
