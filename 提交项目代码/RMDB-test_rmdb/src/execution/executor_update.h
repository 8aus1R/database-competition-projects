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
    RmFileHandle* fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager* sm_manager_;

   public:
    UpdateExecutor(SmManager* sm_manager, const std::string& tab_name,
                   std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids,
                   Context* context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = set_clauses;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = conds;
        rids_ = rids;
        context_ = context;
    }
    void delete_index(RmRecord* rec, Rid rid_) {
        // 删除索引
        for (auto& index : tab_.indexes) {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
                tab_name_, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            char* key = new char[index.col_tot_len];
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, rec->data + index.cols[j].offset,
                       index.cols[j].len);
                offset += index.cols[j].len;
            }

            ih->delete_entry(key, context_->txn_);
            delete[] key;
        }
    }

    bool insert_index(RmRecord* rec, Rid rid_) {
        // 插入索引
        for (int i = 0; i < (int)tab_.indexes.size(); i++) {
            auto& index = tab_.indexes[i];
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(
                tab_name_, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            char* key = new char[index.col_tot_len];
            int offset = 0;
            for (int j = 0; j < index.col_num; ++j) {
                memcpy(key + offset, rec->data + index.cols[j].offset,
                       index.cols[j].len);
                offset += index.cols[j].len;
            }

            auto result = ih->insert_entry(key, rid_, context_->txn_);
            delete[] key;
            if (result == -1) {
                return false;
            }
        }
        return true;
    }
    std::unique_ptr<RmRecord> Next() override {
        // 实现update的next接口。
        // 首先预处理出set语句中操作符左边对应的列的元数据，后面就可以O(1)的时间获取，加快速度。
        std::map<TabCol, ColMeta> mp;
        for (const auto& i : set_clauses_) {
            ColMeta col = *get_col(tab_.cols, i.lhs);
            mp[i.lhs] = col;
        }
        // 枚举rids_中每一条rid，并且更新对应位置的数据。
        for (auto rid : rids_) {
            auto old_rec = fh_->get_record(rid, context_);
            auto rec = fh_->get_record(rid, context_);
            delete_index(rec.get(), rid);
            for (const auto& i : set_clauses_) {
                auto col = mp[i.lhs];
                auto value = i.rhs;
                if (col.type != value.type) {
                    if (col.type == TYPE_STRING || col.type == TYPE_INT || value.type == TYPE_STRING) {
                        throw IncompatibleTypeError(coltype2str(col.type), coltype2str(value.type));
                    }
                    value.set_float((float)value.int_val);
                }
                // 初始化value中的记录数据，并赋值到rec中的对应位置。
                value.init_raw(col.len);
                //更新记录数据
                memcpy(rec->data + col.offset, value.raw->data, col.len);
            }
            if (!insert_index(rec.get(), rid)) {
                fh_->update_record(rid, old_rec->data, context_);
                insert_index(old_rec.get(), rid);
                throw InternalError("Index Error");
            };
            // 通过文件句柄调用update_record进行更新记录
            fh_->update_record(rid, rec->data, context_);
        }
        return nullptr;
    }

    Rid& rid() override { return _abstract_rid; }
};