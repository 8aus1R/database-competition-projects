/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/disk_manager.h"

#include <assert.h>    // for assert
#include <string.h>    // for memset
#include <sys/stat.h>  // for stat
#include <unistd.h>    // for lseek

#include "defs.h"

DiskManager::DiskManager() : fd2pageno_{} {}
// DiskManager::DiskManager() : fd2pageno_{}{};

/**
 * @description: 将数据写入文件的指定磁盘页面中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 写入目标页面的page_id
 * @param {char} *offset 要写入磁盘的数据
 * @param {int} num_bytes 要写入磁盘的数据大小
 */
void DiskManager::write_page(int fd, page_id_t page_no, const char* offset,
                             int num_bytes) {
    // Todo:
    // 1.lseek()定位到文件头，通过(fd,page_no)可以定位指定页面及其在磁盘文件中的偏移量
    // 2.调用write()函数
    // 注意write返回值与num_bytes不等时 throw InternalError("DiskManager::write_page Error");
    
    off_t file_offset = lseek(fd, page_no * num_bytes, SEEK_SET); //将文件指针定位到目标页面的起始位置
    if (file_offset < 0) {
        throw UnixError();
    }
    if (write(fd, offset, num_bytes) != num_bytes) {//将数据写入文件，并比较写入文件的大小与目标大小是否一致
        throw InternalError("DiskManager::write_page Error");
    }
}



/**
 * @description: 读取文件中指定编号的页面中的部分数据到内存中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 指定的页面编号
 * @param {char} *offset 读取的内容写入到offset中
 * @param {int} num_bytes 读取的数据量大小
 */
void DiskManager::read_page(int fd, page_id_t page_no, char* offset,
                            int num_bytes) {
    // Todo:
    // 1.lseek()定位到文件头，通过(fd,page_no)可以定位指定页面及其在磁盘文件中的偏移量
    // 2.调用read()函数
    // 注意read返回值与num_bytes不等时，throw InternalError("DiskManager::read_page Error");
    off_t ret = lseek(fd, page_no * num_bytes, SEEK_SET);//将文件指针定位到目标页面的起始位置
    if (ret < 0) {
        throw UnixError();
    }
    if (read(fd, offset, num_bytes) != static_cast<size_t>(num_bytes)) {//读取数据并比较读取数据大小与目标大小是否一致
        throw InternalError("DiskManager::read_page Error");
    }
}

/**
 * @description: 分配一个新的页号
 * @return {page_id_t} 分配的新页号
 * @param {int} fd 指定文件的文件句柄
 */
page_id_t DiskManager::allocate_page(int fd) {
    // 简单的自增分配策略，指定文件的页面编号加1
    assert(fd >= 0 && fd < MAX_FD);//保证文件描述符在范围内
    return fd2pageno_[fd]++;//当前文件的页面数加一
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {}

bool DiskManager::is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    //stat用来获取数据元信息并存储到st中。S_ISDIR 是一个宏，用于判断 st_mode 是否表示目录
}

void DiskManager::create_dir(const std::string& path) {
    // Create a subdirectory
    std::string cmd = "mkdir " + path;//构建系统命令
    if (system(cmd.c_str()) < 0) {  // 创建一个名为path的目录
        throw UnixError();
    }
}

void DiskManager::destroy_dir(const std::string& path) {
    std::string cmd = "rm -r " + path;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 判断指定路径文件是否存在
 * @return {bool} 若指定路径文件存在则返回true 
 * @param {string} &path 指定路径文件
 */
bool DiskManager::is_file(const std::string& path) {
    // 用struct stat获取文件信息
    struct stat st = {0};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    //stat 函数通过文件路径获取文件的元数据，并将结果填充到 st 结构体中
    //st_mode 字段存储文件的类型和权限信息。
    //S_ISREG 是一个宏，用于判断 st_mode 是否表示常规文件
}

/**
 * @description: 用于创建指定路径文件
 * @return {*}
 * @param {string} &path
 */
void DiskManager::create_file(const std::string& path) {
    // Todo:
    // 调用open()函数，使用O_CREAT模式
    // 注意不能重复创建相同文件
    int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR); // 使用O_create模式进行操作，
    // O_create表示创建文件，O_excl用于报错处理，比如创建文件时已存在，O_rdwr表示文件的类型，支持读和写的操作。
    // s_irusr是定义文件的限权，表示可以从目录中读取文件，S_iwusr就是可以写入当前文件到目录中。

    if (fd == -1) {
        throw FileExistsError("DiskManager::create_file Error");
    }
    
    close(fd); // 创建之后默认是打开的，创建完之后需要关闭文件。
}

/**
 * @description: 删除指定路径的文件
 * @param {string} &path 文件所在路径
 */
void DiskManager::destroy_file(const std::string& path) {
    // Todo:
    // 调用unlink()函数
    // 注意不能删除未关闭的文件
    if (path2fd_.count(path)) { //判断是否为已经打开的文件
        throw FileNotClosedError(path);
    }
    if (!is_file(path)) { //判断文件是否存在
        throw FileNotFoundError(path);
    }
    int res = unlink(path.c_str());//是 POSIX 系统调用，用于删除文件或目录链接，如果失败返回值就是-1。
    if (res < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开指定路径文件 
 * @return {int} 返回打开的文件的文件句柄
 * @param {string} &path 文件所在路径
 */
int DiskManager::open_file(const std::string& path) {
    // Todo:
    // 调用open()函数，使用O_RDWR模式
    // 注意不能重复打开相同文件，并且需要更新文件打开列表
    if (!is_file(path)) { //判断文件是否存在
        throw FileNotFoundError(path);
    }
    if (path2fd_.count(path)) { //判断是否为已经打开的文件
        throw FileNotClosedError(path);
    }
    int fd = open(path.c_str(), O_RDWR); //打开文件，并给予文件的属性：O_RDWR，表示可读可写，如果失败返回-1.
    if (fd < 0) {
        throw UnixError();
    }
    path2fd_[path] = fd;//更新映射表
    fd2path_[fd] = path;
    return fd;
}

/**
 * @description:用于关闭指定路径文件 
 * @param {int} fd 打开的文件的文件句柄
 */
void DiskManager::close_file(int fd) {
    // Todo:
    // 调用close()函数
    // 注意不能关闭未打开的文件，并且需要更新文件打开列表
    if (!fd2path_.count(fd)) {//判断文件是否已关闭。
        throw FileNotOpenError(fd);
    }
    if (close(fd) < 0) {//关闭文件，若失败返回-1.
        throw UnixError();
    }
    std::string path = fd2path_[fd];//删除映射关系
    fd2path_.erase(fd);
    path2fd_.erase(path);
}

/**
 * @description: 获得文件的大小
 * @return {int} 文件的大小
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_size(const std::string& file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

/**
 * @description: 根据文件句柄获得文件名
 * @return {string} 文件句柄对应文件的文件名
 * @param {int} fd 文件句柄
 */
std::string DiskManager::get_file_name(int fd) {
    if (!fd2path_.count(fd)) {//判断文件是否已关闭。
        throw FileNotOpenError(fd);
    }
    return fd2path_[fd];
}

/**
 * @description:  获得文件名对应的文件句柄
 * @return {int} 文件句柄
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_fd(const std::string& file_name) {
    if (!path2fd_.count(file_name)) {
        return open_file(file_name);
    }
    return path2fd_[file_name];
}

/**
 * @description:  读取日志文件内容
 * @return {int} 返回读取的数据量，若为-1说明读取数据的起始位置超过了文件大小
 * @param {char} *log_data 读取内容到log_data中
 * @param {int} size 读取的数据量大小
 * @param {int} offset 读取的内容在文件中的位置
 */
int DiskManager::read_log(char* log_data, int size, int offset) {
    // read log file from the previous end
    if (log_fd_ == -1) {//文件未打开，则打开文件
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    int file_size = get_file_size(LOG_FILE_NAME);//获取文件大小
    if (offset > file_size) {//若偏移量超过文件大小，返回 -1 表示错误。
        return -1;
    }

    size = std::min(size, file_size - offset);//调整读取大小，确保不会读取超过文件末尾的数据
    if (size == 0)
        return 0;
    lseek(log_fd_, offset, SEEK_SET);//文件指针移动到指定偏移量
    ssize_t bytes_read = read(log_fd_, log_data, size);//从磁盘读取数据
    assert(bytes_read == size);//判断读取大小是否正确
    return bytes_read;
}

/**
 * @description: 写日志内容
 * @param {char} *log_data 要写入的日志内容
 * @param {int} size 要写入的内容大小
 */
void DiskManager::write_log(char* log_data, int size) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }

    // write from the file_end
    lseek(log_fd_, 0, SEEK_END);
    ssize_t bytes_write = write(log_fd_, log_data, size);
    if (bytes_write != size) {
        throw UnixError();
    }
}