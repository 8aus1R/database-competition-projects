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

// 当内存足够的时候直接使用快速排序进行操作会简单很多。
class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;                              // 框架中只支持一个键排序，需要自行修改数据结构支持多个键排序
    std::vector<bool> is_desc_;
    std::vector<std::unique_ptr<RmRecord>> cnt;
    int limit_;
    int idx;

   public:
    bool is_agg_sort;
    
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols, std::vector<bool> is_desc, int limit) {
        prev_ = std::move(prev);
        for (const auto& sel_col : sel_cols) {
            cols_.push_back(*get_col(prev_->cols(), sel_col));
        }
        is_desc_ = is_desc;
        limit_ = limit;
        idx = 0;
        is_agg_sort = false;
        if (prev_->getType() == "AggregateExecutor") {
            is_agg_sort = true;
        }
    }

    const std::vector<ColMeta> &cols() const override {
        return prev_->cols();
    };

    void beginTuple() override { 
        prev_->beginTuple();
        while (!prev_->is_end()) {
            cnt.push_back(prev_->Next());
            prev_->nextTuple();
        }
        idx = 0;
        std::sort(cnt.begin(), cnt.end(), [&] (const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b) {
            for (int i = 0; i < (int)cols_.size(); i++) {
                int cmp = compare(a, b, cols_[i]);
                if (cmp != 0) {
                    return is_desc_[i] ? cmp > 0 : cmp < 0;
                }
            }
            return false;
        });
    }

    void nextTuple() override {
        if (limit_ != -1) limit_--;
        idx++;
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(cnt[idx]);
    }

    bool is_end() const override {
        return idx >= (int)cnt.size() || (limit_ != -1 && limit_ <= 0);
    }

    bool is_aggregate() override {
        return prev_->is_aggregate();
    }

    int compare(const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b, ColMeta &col) {
        char * l_data = a->data + col.offset;
        char * r_data = b->data + col.offset;
        return ix_compare(l_data, r_data, col.type, col.len);
    }

    Rid &rid() override { return _abstract_rid; }

    std::string getType() { return "SortExecutor"; };
};