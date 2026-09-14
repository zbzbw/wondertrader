/*!
 * \file BoostMappingFile.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief boost的内存映射文件组件的封装,方便使用
 */
#pragma once
#include <boost/filesystem.hpp>
#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#endif

class BoostMappingFile
{
public:
	BoostMappingFile()
	{
		_file_map=NULL;
		_map_region=NULL;
	}

	~BoostMappingFile()
	{
		close();
	}

	void close()
	{
		if(_map_region!=NULL)
			delete _map_region;

		if(_file_map!=NULL)
			delete _file_map;

		_file_map=NULL;
		_map_region=NULL;
	}

	bool sync()
	{
		if(_map_region && !_map_region->flush(0, 0, false))
			return false;
#ifdef _WIN32
		int file = -1;
		if (_sopen_s(
			&file, _file_name.c_str(), _O_RDWR | _O_BINARY,
			_SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
			return false;
		bool flushed = _commit(file) == 0;
		_close(file);
		return flushed;
#else
		return true;
#endif
	}

	void *addr()
	{
		if(_map_region)
			return _map_region->get_address();
		return NULL;
	}

	size_t size()
	{
		if(_map_region)
			return _map_region->get_size();
		return 0;
	}

	bool map(const char *filename,
		int mode=boost::interprocess::read_write,
		int mapmode=boost::interprocess::read_write,bool zeroother=true)
	{
		if (!boost::filesystem::exists(filename))
		{
			return false;
		}
		_file_name = filename;

		_file_map = new boost::interprocess::file_mapping(filename,(boost::interprocess::mode_t)mode);
		if(_file_map==NULL)
			return false;

		_map_region = new boost::interprocess::mapped_region(*_file_map,(boost::interprocess::mode_t)mapmode);
		if(_map_region==NULL)
		{
			delete _file_map;
			return false;
		}

		return true;
	}

	const char* filename()
	{
		return _file_name.c_str();
	}

	bool valid() const
	{
		return _file_map != NULL;
	}

private:
	std::string _file_name;
	boost::interprocess::file_mapping *_file_map;
	boost::interprocess::mapped_region *_map_region;
};

