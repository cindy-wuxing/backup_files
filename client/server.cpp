//#ifndef __M_SRV_H__
//#define __M_SRV_H__ 
#include "httplib.h"
#include <fstream>
#include <unordered_map>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <pthread.h>
#include <zlib.h>
#include<iostream>

#define SERVER_ADDR "192.168.43.173"
#define SERVER_PORT 9000
#define SERVER_BASE "www"
#define SERVER_CERT_PATH    "./cert.pem"
#define SERVER_KEY_PATH     "./key.pem"
#define SERVER_REQ_PATH     "/list/"
#define SERVER_BACKUP_PATH  SERVER_BASE"/list/"
#define SERVER_ZIP_PATH     SERVER_BASE"/zip/"
#define SERVER_LIST_FILE    SERVER_BASE"/file.list"

#define GZIP_BACKUP_TIME    180

namespace bf = boost::filesystem;

class BackupFile
{
    private:
        std::unordered_map<std::string, std::string> _file_list;
        pthread_rwlock_t _rwlock;
    public:
        static bool Compress(const std::string &sfile, const std::string &dfile) {
            int filefd = open(sfile.c_str(), O_RDONLY);
            if (filefd < 0) {
                std::cerr << "open file " << dfile << " error\n";
                return false;
            }
            int gzipfd = open(dfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664);
            if (gzipfd < 0) {
                std::cerr << "open zip file " << sfile << " error\n";
                close(filefd);
return false;
            }
            flock(filefd, LOCK_SH);
            flock(gzipfd, LOCK_EX);
            gzFile zf = gzdopen(gzipfd, "w");  
            if (zf == NULL) {
                std::cerr << "open zip file " << dfile << " error\n";
                flock(gzipfd, LOCK_UN);
                flock(filefd, LOCK_UN);
                close(filefd);
                close(gzipfd);
                return false;
            }
            int num , ret;
            char buf[1024] = {0};
            while((num = read(filefd, buf, 1024)) > 0) {
                ret = gzwrite(zf, buf, num);
                if (ret == 0) {
                    std::cerr << "write zip data to file " << dfile << " error\n";
                    flock(gzipfd, LOCK_UN);
                    flock(filefd, LOCK_UN);
                    gzclose(zf);
                    close(filefd);
                    close(gzipfd);
                    return false;
                }
            }
            flock(gzipfd, LOCK_UN);
            flock(filefd, LOCK_UN);
            gzclose(zf);
            close(filefd);
            close(gzipfd);
            return true;
        }
        static bool UnCompress(const std::string &sfile, const std::string &dfile) {
            int gzipfd = open(sfile.c_str(), O_RDONLY);
            if (gzipfd < 0) {
                std::cerr << "open zip file " << sfile << " error\n";
                return false;
            }
            int filefd = open(dfile.c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0664);
            if (filefd < 0) {
                std::cerr << "open file " << dfile << " error\n";
                close(gzipfd);
                return false;
            }
            flock(gzipfd, LOCK_SH);
            flock(filefd, LOCK_EX);
            gzFile zf = gzdopen(gzipfd, "rb");
            if (zf == NULL) {
                std::cerr << "open zip file " << sfile << " error\n";
                flock(filefd, LOCK_UN);
                flock(gzipfd, LOCK_UN);
                close(gzipfd);
                close(filefd);
            }
            int ret, num;
            char buf[1024] = {0};
            while((num = gzread(zf, buf, 1024)) > 0) {
                int ret = write(filefd, buf, num);
                if (ret < 0) {
                    std::cerr << "uncompress write file " << dfile << " error\n";
                    flock(filefd, LOCK_UN);
                    flock(gzipfd, LOCK_UN);
                    gzclose(zf);
                    close(gzipfd);
                    close(filefd);
                    return false;
                }
            }
            flock(filefd, LOCK_UN);
            flock(gzipfd, LOCK_UN);
            gzclose(zf);
            close(gzipfd);
            close(filefd);
            return true;
        }
    public:
        BackupFile() {
            pthread_rwlock_init(&_rwlock, NULL);
        }
        ~BackupFile() {
            pthread_rwlock_destroy(&_rwlock);
        }
        bool GZipBackupFile() {
            bf::directory_iterator iter_begin(SERVER_BACKUP_PATH);
            bf::directory_iterator iter_end;
            for ( ; iter_begin != iter_end; ++iter_begin) {
                if (bf::is_directory(iter_begin->status())) {
                    continue;
                }
                time_t cur_t = time(NULL);
                time_t mod_t = bf::last_write_time(iter_begin->path());
                //printf("file:%s-time:%ld-%ld-%ld\n", iter_begin->path().string().c_str(), mod_t, cur_t, mod_t - cur_t);
                if ((cur_t - mod_t) > GZIP_BACKUP_TIME) {
                    std::string file = iter_begin->path().string();
                    std::string gzip = SERVER_ZIP_PATH + iter_begin->path().filename().native() + ".gz";
                    Compress(file, gzip);
                    pthread_rwlock_wrlock(&_rwlock);
                    _file_list[file] = gzip;
                    unlink(file.c_str());//当有其它进程正在适用文件时，则删除失败，应该循环删除
                    printf("unlink file: %s\n", file.c_str());
                    pthread_rwlock_unlock(&_rwlock);
                }
            }

            return true;
        }
        bool GetFileList(std::vector<std::string> &list) {
            pthread_rwlock_rdlock(&_rwlock);
            for(auto it = _file_list.begin(); it != _file_list.end(); ++it) {
                list.push_back(it->first);
            }
            pthread_rwlock_unlock(&_rwlock);
            return true;
        }
        bool HasFile(const std::string &file) {
            pthread_rwlock_rdlock(&_rwlock);
            auto it = _file_list.find(file);
            if (it == _file_list.end()) {
                std::cerr << "has no file " << file << std::endl;
                pthread_rwlock_unlock(&_rwlock);
                return false;
            }
            pthread_rwlock_unlock(&_rwlock);
            return true;
        }
        bool ReadFile(const std::string &file, std::string &body) {
            bf::path path(file);
            size_t fsize = bf::file_size(path);
            body.resize(fsize);
            int fd = open(file.c_str(), O_RDONLY);
            if (fd < 0) {
                std::cerr << "open file " << file << " error\n";
                return false;
            }
            flock(fd, LOCK_SH);
            int ret = read(fd, &body[0], fsize);
            if (ret < 0) {
                std::cerr << "read file " << file << " body error\n";
                flock(fd, LOCK_UN);
                return false;
            }
            flock(fd, LOCK_UN);
            close(fd);
            return true;
        }
        bool ReadZip(const std::string &zip, const std::string &file, std::string &body) {
            if (UnCompress(zip, file) == false) {
                return false;
            }
            if (ReadFile(file, body) == false) {
                return false;
            }
            pthread_rwlock_wrlock(&_rwlock);
            _file_list[file] = "";
            pthread_rwlock_unlock(&_rwlock);
            return true;
        }
        bool ReadFileBody(const std::string &file, std::string &body) {
            if (HasFile(file) == false) {
                return false;
            }
            bf::path path(file);
            if (bf::exists(path)) {
                return ReadFile(file, body);
            }
            pthread_rwlock_rdlock(&_rwlock);
            auto it = _file_list.find(file);
            if (it != _file_list.end()) {
                std::string zip = it->second;
                pthread_rwlock_unlock(&_rwlock);
                return ReadZip(zip, file, body);
            }
            pthread_rwlock_unlock(&_rwlock);
            return false;
        }
        bool WriteFileBody(const std::string &file, const std::string &body, int64_t offset) {
            std::cout<<"already in WriteFileBody\n";
            int fd = open(file.c_str(), O_CREAT|O_WRONLY, 0664);
            if (fd < 0) {
                std::cerr << "open file " << file << " error\n";
                return false;
            }
            flock(fd, LOCK_EX);
            lseek(fd, offset, SEEK_SET);
            int ret = write(fd, &body[0], body.size());
            if (ret < 0) {
                std::cerr << "write file " << file << " error\n";
                flock(fd, LOCK_UN);
                return false;
            }
            std::cout<<"now write is ok\n";
            flock(fd, LOCK_UN);
            close(fd);
            pthread_rwlock_wrlock(&_rwlock);
            _file_list[file] = "";
            pthread_rwlock_unlock(&_rwlock);
            return true;
        }
        bool GetRecored() {
            std::string body;
            if (!bf::exists(SERVER_LIST_FILE)) {
                std::cerr << "gzip list file " << SERVER_LIST_FILE << "is not exists\n";
                return false;
            }
            if (ReadFile(SERVER_LIST_FILE, body) == false) {
                return false;
            }
            std::vector<std::string> list;
            boost::split(list, body, boost::is_any_of("\n"));
            pthread_rwlock_wrlock(&_rwlock);
            for (int i = 0; i < list.size(); i++) {
                size_t pos = list[i].find(" ");
                if (pos == std::string::npos) {
                    continue;
                }
                std::string file = list[i].substr(0, pos);
                std::string zip = list[i].substr(pos + 1);
                _file_list[file] = zip;
            }
            pthread_rwlock_unlock(&_rwlock);
            return true;
        }
        bool SetRecored() {
            int fd = open(SERVER_LIST_FILE, O_CREAT|O_WRONLY|O_TRUNC, 0664);
            if (fd < 0) {
                std::cerr << "open file " << SERVER_LIST_FILE << " error\n";
                return false;
            }
            flock(fd, LOCK_EX);
            pthread_rwlock_rdlock(&_rwlock);
            for (auto it = _file_list.begin(); it != _file_list.end(); ++it) {
                std::string info = it->first + " " + it->second + "\n";
                int ret = write(fd, info.c_str(), info.size());
                if (ret < 0) {
                    std::cerr << "write file " << SERVER_LIST_FILE << " error\n";
                    pthread_rwlock_unlock(&_rwlock);
                    return false;
                }
            }
            pthread_rwlock_unlock(&_rwlock);
            flock(fd, LOCK_UN);
            close(fd);
            return true;
        }
};

static BackupFile _backup;

class CloudServer
{
    public:
        CloudServer(const std::string &cert, const std::string &key): 
            _cert_file(cert), _private_key(key) {
            if (!bf::exists(SERVER_BASE)) {
                bf::create_directory(SERVER_BASE);
            }
            if (!bf::exists(SERVER_BACKUP_PATH)) {
                bf::create_directory(SERVER_BACKUP_PATH);
            }
            if (!bf::exists(SERVER_ZIP_PATH)) {
                bf::create_directory(SERVER_ZIP_PATH);
            }
        }
        bool Start() {
            _backup.GetRecored();
            std::thread thr(GZipStart, &_backup);
            thr.detach();
            httplib::SSLServer srv(_cert_file.c_str(), _private_key.c_str());
            srv.set_base_dir(SERVER_BASE);
            srv.Get("/(list/{0,1}){0,1}", GetFileList);
            srv.Put("/list/(.*)", PutFileBackup);
            srv.Get("/list/(.*)", FileDownload);

            srv.listen(SERVER_ADDR, SERVER_PORT);
            return true;
        }
    private:
        static void GZipStart(BackupFile *backup) {
            while(1) {
                backup->GZipBackupFile();
                backup->SetRecored();
                sleep(10);
            }
        }
        static void FileDownload(const httplib::Request &req, httplib::Response &rsp) {
            std::string body;
            std::string path = SERVER_BASE + req.path;
            _backup.ReadFileBody(path, body);
            rsp.set_header("Content-Length", std::to_string(body.size()).c_str());
            rsp.set_content(&body[0], body.size(), "text/plain");
        }
        static void GetFileList(const httplib::Request &req, httplib::Response &rsp) {
            std::stringstream body;
            std::vector<std::string> list;
            _backup.GetFileList(list);
            body << "<html><body><hr /><ol>";
            for (int i = 0; i < list.size(); i++) {
                bf::path file(list[i]);
                std::string req_path = SERVER_REQ_PATH + file.filename().native();
                body << "<a href='" << req_path << "'>";
                body << "<li><h4>" << file.filename().native() << "</h4></li>";
                body << "</a>";
            }
            body << "</ol><hr /></body></html>";
            rsp.set_content(body.str(), "text/html");
            return;
        }
        static void PutFileBackup(const httplib::Request &req, httplib::Response &rsp) {
            std::cout<<"a file need to backup\n";
            std::string realpath = SERVER_BACKUP_PATH;
            bf::path file(req.path);
            realpath += file.filename().native();
            std::cout<<"realpath = "<<realpath<<"\n";

            if (req.has_header("Range")) {
                std::string range = req.get_header_value("Range");
                int64_t r_start, r_end, r_len;
                GetFileRange(range, r_start, r_end, r_len);
                printf("put file:[%s] body:[%ld-%ld]-[%ld]\n", realpath.c_str(), r_start, r_end, req.body.size());
                _backup.WriteFileBody(realpath, req.body, r_start);
            }else {
                _backup.WriteFileBody(realpath, req.body, 0);
            }

            return;
        }
        static bool GetFileRange(const std::string &range, int64_t &start, int64_t &end, int64_t &len) {
            //Range: bytes=200-1000
            std::stringstream tmp;
            std::string unit = "bytes=";
            std::string seg = "-";
            size_t pos1 = range.find(unit);
            size_t pos2 = range.find(seg);
            if (pos1 == std::string::npos || pos2 == std::string::npos) {
                return false;
            }
            std::string str_start = range.substr(pos1 + unit.size(), pos2 - (pos1 + unit.size()));
            std::string str_end = range.substr(pos2 + seg.size());
            tmp << str_start;
            tmp >> start;
            tmp.clear();
            tmp << str_end;
            tmp >> end;
            len = end - start + 1;
            return true;
        }
    private:
        std::string _cert_file;
        std::string _private_key;
};

//#endif

int main()
{
    CloudServer server(SERVER_CERT_PATH, SERVER_KEY_PATH);

    server.Start();
    return 0;
}
