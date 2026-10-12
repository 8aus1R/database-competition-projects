/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <cstring>
#include "log_manager.h"

/**
 * @description: 添加日志记录到日志缓冲区中，并返回日志记录号
 * @param {LogRecord*} log_record 要写入缓冲区的日志记录
 * @return {lsn_t} 返回该日志的日志记录号
 */
lsn_t LogManager::add_log_to_buffer(LogRecord* log_record) { // 获取全局lsn
    std::lock_guard<std::mutex> guard(latch_); // 锁住log_buffer_，防止并发写入
    lsn_t lsn = global_lsn_++;
    log_record->lsn_ = lsn; // 设置日志记录的lsn
    // 2. 检查单条日志是否超过缓冲区容量
    if (log_buffer_.is_full(log_record->log_tot_len_)) {
        flush_log_to_disk(); // 如果缓冲区满了，则先将日志写入磁盘
    }
    // 序列化日志记录到缓冲区
    log_record->serialize(log_buffer_.buffer_ + log_buffer_.offset_);
    log_buffer_.offset_ += log_record->log_tot_len_; // 更新偏移量

    return lsn; // 返回日志记录号
}

/**
 * @description: 把日志缓冲区的内容刷到磁盘中，由于目前只设置了一个缓冲区，因此需要阻塞其他日志操作
 */

// 由于只涉及到了一个日志缓冲区，persist_lsn_变量暂时不需要用到，待实现。

void LogManager::flush_log_to_disk() { // 获取当前日志缓冲区的内容
    if (log_buffer_.offset_ == 0) {
        return; // 如果没有内容，则直接返回
    }
    // 将日志缓冲区的内容写入磁盘
    disk_manager_->write_log(log_buffer_.buffer_, log_buffer_.offset_);
    persist_lsn_ = global_lsn_ - 1; // 记录已经持久化到磁盘中的最后一条日志的日志号
    log_buffer_.offset_ = 0; // 重置偏移量
    memset(log_buffer_.buffer_, 0, sizeof(log_buffer_.buffer_)); // 清空缓冲区
}
