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

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;        // 投影节点的儿子节点
    std::vector<ColMeta> cols_;                     // 需要投影的字段
    size_t len_;                                    // 字段总长度
    std::vector<size_t> sel_idxs_; // 需要投影的列的下表.        

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);

        size_t curr_offset = 0;
        // emp中存在name,age,score三个字段，我现在只需要打印后面两个，那么后面两个字段的偏移量需要被改变。
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            // 得到当前字段在col中的下表，用sel_idxs_记录。
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            // 重新计算每个col的偏移量，并赋值为cols_。
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
    }

    const std::vector<ColMeta>& cols() const override { return cols_; }
    // 投影列是根目录，他的第一条合法记录就是孩子的第一个合法记录，不需要其他操作。
    void beginTuple() override {
        prev_->beginTuple();  // 调用子节点的beginTuple
    }

    void nextTuple() override { prev_->nextTuple(); }

    // 实现project的next接口，将需要投影的列的记录计算出来并返回出去。
    std::unique_ptr<RmRecord> Next() override {
        auto rec = std::make_unique<RmRecord>(len_);
        auto& prev_cols = prev_->cols(); // 得到cols的所有列。
        auto prev_rec = prev_->Next(); // 得到之前的完整记录。

        for (size_t i = 0; i < sel_idxs_.size(); i++) {  // 从prev_rec中取出需要的字段
            auto idx = sel_idxs_[i];
            auto prev_col = prev_cols[idx]; // 找到当前记录再所有列中的对应位置。

            auto col = cols_[i];
            auto prev_val = prev_rec->data + prev_col.offset; // 取出当前记录的数据。

            memcpy(rec->data + col.offset, prev_val, col.len); // 赋值到rec的对应位置。
        }
        return rec;
    }

    bool is_end() const override { return prev_->is_end(); }

    Rid& rid() override { return _abstract_rid; }

};