// CloudClient.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//
#include "pch.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <unordered_map>
//#include <winsock2.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"

#define SERVER_PORT	9000
#define SERVER_ADDR	"192.168.43.173"
#define LIST_FILE	"backup.list"
#define BACKUP_URI	"/list/"
#define MAX_RANGE_SIZE	(10<<20)
#define BACKUP_SUFFIX	".download"

namespace bf = boost::filesystem;

class RangeInfo
{
	private:
		bf::path _file;
		int64_t _range_start;
		int64_t _range_end;
	public:
		bool _exit_code;
	public:
		RangeInfo():_exit_code(true){ }
		void SetRange(const bf::path &file, int64_t start, int64_t end) {
			_file = file;
			_range_start = start;
			_range_end = end;
		}
		void BackUp() {
            std::cout<<"already in BackUp\n";
			std::string filename = _file.filename().string();
			std::string body;

			size_t content_len = _range_end - _range_start + 1;
			body.resize(content_len);

			std::ifstream fs(_file.string(), std::ios::binary);
			if (!fs.is_open()) {
				std::cerr << "open file:" << filename << " error\n";
				_exit_code = false;
				return;
			}
			
			fs.seekg(_range_start, std::ios::beg);
			fs.read(&body[0], content_len);
			if (!fs.good()) {
				std::cerr << "read file:" << filename << " body error\n";
				// printf_s("read file:[%s]-[%lld-%lld] body error\n", _file.string().c_str(), _range_start, _range_end);
				_exit_code = false;
				return;
			}
			fs.close();
            std::cout<<"now set headers\n";
			//printf_s("read file:[%s]-[%lld-%lld] success\n", _file.string().c_str(), _range_start, _range_end);
			std::string url = BACKUP_URI + filename;
            std::cout<<"url = "<<url<<"\n";
            httplib::Headers hdr;
			std::stringstream range_hdr;
			range_hdr << "bytes=" << _range_start << "-" << _range_end;
			hdr.insert(std::make_pair("Range", range_hdr.str()));
			hdr.insert(std::make_pair("Content-Length", std::to_string(content_len)));
			httplib::SSLClient client(SERVER_ADDR, SERVER_PORT);
            //std::cout<<"body= "<<body<<"\n";
            std::cout<<"send datas\n";
			auto res = client.Put(url.c_str(), hdr, body, "text/plain");

			if ((!res) || (res && res->status != 200)) {
				std::cerr << "backup file:" << filename << " failed!\n";
				_exit_code = false;
				return;
			}
            std::cout<<"success\n";
			return;
		}
};

class CloudClient
{
public:
	CloudClient(const std::string &path, int wait_time = 3) :
		_backup_dir(path), _wait_time(wait_time) { 
		//WSADATA	 wsadata;
		//WSAStartup(MAKEWORD(2, 2), &wsadata);
		if (!bf::exists(_backup_dir)) {
			bf::create_directory(_backup_dir);
		}
	}
	~CloudClient() {
		//WSACleanup();
	}
	bool Start() {
		while (1) {
			BackupGetRecord();
			BackupDirListen(_backup_dir);
			BackupSetRecord();
			sleep(_wait_time * 10);
		}
		return true;
	}
	bool BackupSetRecord() {
		std::ofstream fs(LIST_FILE, std::ios::binary | std::ios::trunc);
		if (!fs.is_open()) {
			std::cerr << "open file error\n";
			return false;
		}
		std::stringstream backup_info;
		for (auto it = _backup_list.begin(); it != _backup_list.end(); ++it) {
			backup_info << it->first << " " << it->second << "\n";
		}
		fs.write(backup_info.str().c_str(), backup_info.str().size());
		if (!fs.good()) {
			std::cerr << "write backup info error\n";
			return false;
		}
		fs.close();
		return true;
	}
	bool BackupGetRecord() {
		std::ifstream fs(LIST_FILE, std::ios::binary);
		if (!fs.is_open()) {
			std::cerr << "open list file error\n";
			return false;
		}
		fs.seekg(0, std::ios::end);
		size_t size = fs.tellg();
		fs.seekg(0, std::ios::beg);
		std::string buf;
		buf.resize(size);
		fs.read(&buf[0], size);
		if (!fs.good()) {
			std::cerr << "read list file error\n";
			return false;
		}

		std::vector<std::string> list;
		boost::split(list, buf, boost::is_any_of("\n"));
		for (int i = 0; i < list.size(); ++i) {
			size_t pos = list[i].find(" ");
			if (pos == std::string::npos) {
				continue;
			}
			std::string key = list[i].substr(0, pos);
			std::string val = list[i].substr(pos + 1);
			_backup_list.insert(std::make_pair(key, val));
		}
		return true;
	}
	bool BackupDirListen(const bf::path &dir) {
		if (!bf::exists(dir)) {
			bf::create_directory(dir);
		}
		bf::directory_iterator item_begin(dir);
		bf::directory_iterator item_end;

		for (; item_begin != item_end; ++item_begin) {
			if (bf::is_directory(item_begin->status())) {
				BackupDirListen(item_begin->path());
				continue;
			}
			std::string file = item_begin->path().string();
			std::string etag_new = GetFileEtag(item_begin->path());
			auto it = _backup_list.find(file);
			if (it != _backup_list.end() && it->second == etag_new) {
				continue;
			}
			std::cerr << "prepare to backup file:[" << file << "]\n";
			if (FileBackup(item_begin->path())) {
				_backup_list[file] = etag_new;
			}
			std::cerr << "file:[" << file << " backup over\n";
		}
		return true;
	}

	static void thr_start(RangeInfo *range) {
        std::cout<<"already in thr_start\n";
		range->BackUp();
		return;
	}
	bool FileBackup(const bf::path &file) {
		int64_t fsize = bf::file_size(file);
		int64_t count = fsize / MAX_RANGE_SIZE;
		std::vector<std::thread> _thr_list;
		std::vector<RangeInfo> _range_list(count + 1);
		std::cerr << "file1:[" << file.string() << " " << fsize << " " << count << "]\n";
        std::cout<<"count = "<<count<<"\n";
        for (int i = 0; i <= count; i++) {
			int64_t start = i * MAX_RANGE_SIZE;
			int64_t end = ((i + 1) * MAX_RANGE_SIZE) - 1;
			if (i == count) {
				end = fsize - 1;
			}
			_range_list[i].SetRange(file, start, end);
			_thr_list.push_back(std::thread(thr_start, &_range_list[i]));
		}
		bool ret = true;
		for (int i = 0; i <= count; i++) {
			_thr_list[i].join();
			if (_range_list[i]._exit_code == false) {
				ret = false;
			}
		}
		return ret;
	}
	std::string GetFileEtag(const bf::path &file) {
		std::time_t mtime;
		uintmax_t size;
		mtime = bf::last_write_time(file);
		size = bf::file_size(file);

		std::stringstream etag;
		etag << std::hex << size;
		etag << "-";
		etag << std::hex << mtime;
		return etag.str();
	}
private:
	int _wait_time;
	bf::path _backup_dir;
	std::unordered_map<std::string, std::string> _backup_list;
};

int main()
{
	CloudClient cli("haha",1);
	cli.Start();
	return 0;
}
