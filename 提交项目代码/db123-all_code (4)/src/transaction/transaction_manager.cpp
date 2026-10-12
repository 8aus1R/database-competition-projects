/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

/**
 * @description: 事务的开始方法
 * @return {Transaction*} 开始事务的指针
 * @param {Transaction*} txn 事务指针，空指针代表需要创建新事务，否则开始已有事务
 * @param {LogManager*} log_manager 日志管理器指针
 */

// 用一个双端队列来存储事务的写操作记录，每次操作放入到头部，回退每次操作尾部。
// 如果一开始是删除就给他插回去，是插入就给他删除，是更新就更新回去。
// 每次操作都生成一条日志，日志里存的是操作前的状态，后续可以用日志进行回退操作。

Transaction * TransactionManager::begin(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 判断传入事务参数是否为空指针
    // 2. 如果为空指针，创建新事务
    // 3. 把开始事务加入到全局事务表中
    // 4. 返回当前事务指针
    // 如果需要支持MVCC请在上述过程中添加代码
    std::unique_lock<std::mutex> lock(latch_);
    if (txn == nullptr) {
        // 生成新的事务ID
        txn = new Transaction(next_txn_id_++);
    }
    // 设置时间戳和全局事务。
    txn->set_start_ts(next_timestamp_++);
    txn_map.emplace(txn->get_transaction_id(), txn);

    auto* log = new BeginLogRecord(txn->get_transaction_id()); // 创建开始日志记录
    log->prev_lsn_ = txn->get_prev_lsn(); // 设置前一个日志序列号
    txn->set_prev_lsn(log_manager->add_log_to_buffer(log)); // 更新事务的前一个日志序列号

    return txn;
}

/**
 * @description: 事务的提交方法
 * @param {Transaction*} txn 需要提交的事务
 * @param {LogManager*} log_manager 日志管理器指针
 */
void TransactionManager::commit(Transaction* txn, LogManager* log_manager) {
    // Todo:
    // 1. 如果存在未提交的写操作，提交所有的写操作
    // 2. 释放所有锁
    // 3. 释放事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 如果需要支持MVCC请在上述过程中添加代码
    
    // 1. 释放事务持有的所有锁（隔离性：允许其他事务访问资源）
    auto lock_set = txn->get_lock_set(); // 获取事务的锁集
    for (auto i : *lock_set) {
        lock_manager_->unlock(txn, i); // 释放锁
    }
    txn->clear(); // 清空事务的锁集和写集

    auto* log = new CommitLogRecord(txn->get_transaction_id()); // 提交日志记录
    log->prev_lsn_ = txn->get_prev_lsn(); // 设置前一个日志序列号

    txn->set_prev_lsn(log_manager->add_log_to_buffer(log)); // 更新事务的前一个日志序列号
    log_manager->flush_log_to_disk(); // 刷新日志到磁盘
    txn->set_state(TransactionState::COMMITTED); // 更新事务状态为已提交
}

/**
 * @description: 事务的终止（回滚）方法
 * @param {Transaction *} txn 需要回滚的事务
 * @param {LogManager} *log_manager 日志管理器指针
 */
void TransactionManager::abort(Transaction * txn, LogManager *log_manager) {
    // Todo:
    // 1. 回滚所有写操作
    // 2. 释放所有锁
    // 3. 清空事务相关资源，eg.锁集
    // 4. 把事务日志刷入磁盘中
    // 5. 更新事务状态
    // 如果需要支持MVCC请在上述过程中添加代码
    
    Context context(lock_manager_, log_manager, txn);

    auto write_set = txn->get_write_set(); // 获取事务的写集
    while (!write_set->empty()) {
        auto write_record = write_set->back(); // 获取写集中的最后一个写记录
        write_set->pop_back();
        auto write_type = write_record->GetWriteType(); // 获取写操作类型
        auto table_name = write_record->GetTableName(); // 获取表名
        auto record = write_record->GetRecord(); // 获取记录数据
        auto rid = write_record->GetRid(); // 获取记录ID
        if (!sm_manager_->db_.is_table(table_name)) {
            throw TableExistsError(table_name);
        }
        auto file_handle = sm_manager_->fhs_.at(table_name).get(); // 获取文件句柄
        // 皆反
        switch (write_type) { // 根据写操作类型执行相应的回滚操作
            case WType::INSERT_TUPLE: {
                file_handle->delete_record(rid, &context); // 删除记录
                delete_record_in_index(txn, table_name, &record, rid);
                
                auto log_record = std::make_unique<DeleteLogRecord>(
                    txn->get_transaction_id(), record, rid, table_name); // 创建删除日志记录
                log_record->prev_lsn_ = txn->get_prev_lsn(); // 设置前一个日志序列号
                txn->set_prev_lsn(log_manager->add_log_to_buffer(log_record.get())); // 更新事务的前一个日志序列号
                
                break;
            }
            case WType::UPDATE_TUPLE: {
                file_handle->update_record(rid, record.data, &context); // 更新记录
                auto new_record = file_handle->get_record(rid, &context); // 获取旧记录
                delete_record_in_index(txn, table_name, new_record.get(), rid);
                insert_record_in_index(txn, table_name, &record, rid);

                auto log_record = std::make_unique<UpdateLogRecord>(
                    txn->get_transaction_id(), *new_record, record, rid, table_name); // 创建更新日志记录
                log_record->prev_lsn_ = txn->get_prev_lsn(); // 设置前一个日志序列号
                txn->set_prev_lsn(log_manager->add_log_to_buffer(log_record.get())); // 更新事务的前一个日志序列号
                break;
            }
            case WType::DELETE_TUPLE: {
                file_handle->insert_record(rid, record.data); // 插入记录
                insert_record_in_index(txn, table_name, &record, rid);

                auto log_record = std::make_unique<InsertLogRecord>(
                    txn->get_transaction_id(), record, rid, table_name); // 创建插入日志记录
                log_record->prev_lsn_ = txn->get_prev_lsn(); // 设置前一个日志序列号
                txn->set_prev_lsn(log_manager->add_log_to_buffer(log_record.get())); // 更新事务的前一个日志序列号
                break;
            }
            default:
                throw InternalError("Unexpected write type");
        }
    }
    write_set->clear();

    auto lock_set = txn->get_lock_set(); // 获取事务的锁集
    for (auto i : *lock_set) {
        lock_manager_->unlock(txn, i); // 释放锁
    }
    txn->clear(); // 清空事务的锁集和写集

    auto log = std::make_unique<AbortLogRecord>(txn->get_transaction_id()); // 创建回滚日志记录
    log->prev_lsn_ = txn->get_prev_lsn(); // 设置前一个日志序列号
    txn->set_prev_lsn(log_manager->add_log_to_buffer(log.get())); // 更新事务的前一个日志序列号
    log_manager->flush_log_to_disk(); // 刷新日志到磁盘
    txn->set_state(TransactionState::ABORTED); // 更新事务状态为已回滚
}

/**
 * @description: 删除索引中的记录
 * @param {const std::string&} table_name 表名
 * @param {RmRecord*} rec 记录指针
 * @param {Rid} rid_ 记录的Rid
 */
void TransactionManager::delete_record_in_index(Transaction* transaction,
                                                const std::string& table_name,
                                                RmRecord* rec, Rid rid_) {
    auto& tab = sm_manager_->db_.get_table(table_name);
    for (auto& index : tab.indexes) {
        auto index_name = sm_manager_->get_ix_manager()->get_index_name(
            table_name, index.cols);
        auto index_handle = sm_manager_->ihs_[index_name].get();
        auto key = std::make_unique<char[]>(index.col_tot_len);
        int offset = 0;
        for (int j = 0; j < index.col_num; ++j) {
            memcpy(key.get() + offset, rec->data + index.cols[j].offset,
                   index.cols[j].len);
            offset += index.cols[j].len;
        }
        index_handle->delete_entry(key.get(), transaction);
    }
}

/**
 * @description: 在索引中插入记录
 * @param {const std::string&} table_name 表名
 * @param {RmRecord*} rec 记录指针
 * @param {Rid} rid_ 记录的Rid
 */
void TransactionManager::insert_record_in_index(Transaction* transaction,
                                                const std::string& table_name,
                                                RmRecord* rec, Rid rid_) {
    auto& tab = sm_manager_->db_.get_table(table_name);
    for (auto& index : tab.indexes) {
        auto index_name = sm_manager_->get_ix_manager()->get_index_name(
            table_name, index.cols);
        auto index_handle = sm_manager_->ihs_[index_name].get();
        auto key = std::make_unique<char[]>(index.col_tot_len);
        int offset = 0;
        for (int j = 0; j < index.col_num; ++j) {
            memcpy(key.get() + offset, rec->data + index.cols[j].offset,
                   index.cols[j].len);
            offset += index.cols[j].len;
        }
        index_handle->insert_entry(key.get(), rid_, transaction);
    }
}