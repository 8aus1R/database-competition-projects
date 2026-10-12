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

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> left_;    // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;   // 右儿子节点（需要join的表）
    size_t len_;                                // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;                 // join后获得的记录的字段

    std::vector<Condition> fed_conds_;          // join条件
    std::vector<std::unique_ptr<RmRecord>> cnt;
    int idx;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right, 
                            std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        
        auto right_cols = right_->cols();
        for (auto &col : right_cols) {
            col.offset += left_->tupleLen();
        }
        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        fed_conds_ = std::move(conds);
    }

    size_t tupleLen() const override {
        return len_;
    }

    const std::vector<ColMeta> &cols() const override {
        return cols_;
    }

    void beginTuple() override {
        cnt = std::vector<std::unique_ptr<RmRecord>>();
        idx = 0;
        for (left_->beginTuple(); !left_->is_end(); left_->nextTuple()) {
            auto l = left_->Next();
            for (right_->beginTuple(); !right_->is_end(); right_->nextTuple()) {
                auto rec = std::make_unique<RmRecord>(len_);
                auto r = right_->Next();
                memcpy(rec->data, l->data, left_->tupleLen());
                memcpy(rec->data + left_->tupleLen(), r->data, right_->tupleLen());
                if (fed_conds_.empty() || Cal.eval_conds(fed_conds_, cols_, *rec.get())) {
                    cnt.push_back(std::move(rec));
                }
            }
        }
    }

    void nextTuple() override {
        idx++;
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(cnt[idx]);
    }

    bool is_end() const override {
        return idx >= (int)cnt.size();
    }

    std::string getType() override { return "Nestedloop_join"; };

    Rid &rid() override { return _abstract_rid; }
};