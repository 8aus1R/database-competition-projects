/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }

    void delete_index(RmRecord *rec) {
        for (auto & index : tab_.indexes) {
            auto ix = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(index.tab_name, index.cols)).get();
            char *key = new char [index.col_tot_len];
            int offset = 0;
            for (auto & col : index.cols) {
                memcpy(key + offset, rec->data + col.offset, col.len);
                offset += col.len;
            }
            ix->delete_entry(key, context_->txn_);
        }
    }

    bool insert_index(RmRecord *rec, Rid & rid) {
        for(size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto& index = tab_.indexes[i];
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            char* key = new char[index.col_tot_len];
            int offset = 0;
            for(int i = 0; i < index.col_num; ++i) {
                memcpy(key + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                offset += index.cols[i].len;
            }
            if (ih->insert_entry(key, rid, context_->txn_) == -1) {
                for (size_t j = 0; j < i; j++) {
                    auto& index = tab_.indexes[j];
                    auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                    char* key = new char[index.col_tot_len];
                    int offset = 0;
                    for (int i = 0; i < index.col_num; ++i) {
                        memcpy(key + offset, rec->data + index.cols[i].offset, index.cols[i].len);
                        offset += index.cols[i].len;
                    }
                    ih->delete_entry(key, context_->txn_);
                }
                return false;
            }
        }
        return true;
    }

    std::unique_ptr<RmRecord> Next() override {
        int cnt = -1;
        std::vector<std::pair<RmRecord*, RmRecord*>> record;
        for (int i = 0; i < (int)rids_.size(); i++) {
            auto rid = rids_[i];
            auto rec = fh_->get_record(rid, context_);
            auto old_rec = fh_->get_record(rid, context_);
            record.push_back({rec.get(), old_rec.get()});
            delete_index(old_rec.get());
            for (auto & t : set_clauses_) {
                auto col = *get_col(tab_.cols, t.lhs);
                auto val = t.rhs;
                if (col.type != val.type) {
                    if (col.type == TYPE_BIGINT && val.type == TYPE_INT) {
                        BigInt bigint(val.int_val);
                        val.set_bigint(bigint);
                    } else if (col.type == TYPE_INT && val.type == TYPE_BIGINT) {
                        int value = val.bigint_val.value;
                        val.set_int(value);
                    } else if (col.type == TYPE_STRING && val.type == TYPE_DATETIME) {
                        std::string str_val = val.datetime_val.get_datetime();
                        val.set_str(str_val);
                    } else if (col.type == TYPE_FLOAT && val.type == TYPE_INT) {
                        float float_val = 1.0 * val.int_val;
                        val.set_float(float_val);
                    }else {
                        throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
                    }
                }
                if (val.type == TYPE_BIGINT) {
                    if (val.bigint_val.flag == 1) {
                        throw RMDBError("The value is not valid");
                    }
                }
                if (val.type == TYPE_DATETIME and val.datetime_val.flag == false){  // 如果是DateTime且该类型不合法
                    throw RMDBError("The value is not valid");
                }
                val.init_raw(col.len);
                memcpy(rec->data + col.offset, val.raw->data, col.len);    
            }
            if (!insert_index(rec.get(), rid)) {
                cnt = i;
                insert_index(old_rec.get(), rid);
                break;
            }
            fh_->update_record(rid, rec->data, context_);
            //更新事务
            auto* wr = new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rid, *old_rec); // 创建写记录
            context_->txn_->append_write_record(wr); // 添加到事务的写集
        }
        if (cnt != -1) {
            for (int i = 0; i < cnt; i++) {
                auto rec = record[i].first;
                auto old_rec = record[i].second;
                auto rid = rids_[i];
                delete_index(rec);
                insert_index(old_rec, rid);
                fh_->update_record(rid, old_rec->data, context_);
            }
            throw RMDBError("update error");
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};