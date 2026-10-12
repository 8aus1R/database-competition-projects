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
    std::unique_ptr<AbstractExecutor> left_;  // 左儿子节点（需要join的表）
    std::unique_ptr<AbstractExecutor> right_;  // 右儿子节点（需要join的表）
    size_t len_;                 // join后获得的每条记录的长度
    std::vector<ColMeta> cols_;  // join后获得的记录的字段

    std::vector<Condition> fed_conds_;  // join条件
    bool is_end_; // 判断两表连接是否结束。

    ConditionEvaluator Cal;

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left,
                           std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        len_ = left_->tupleLen() + right_->tupleLen();
        cols_ = left_->cols();
        auto right_cols = right_->cols();
        // 将右边的表中每一个偏移量增加左边的长度，然后拼接到左边的后面。
        for (auto& col : right_cols) {
            col.offset += left_->tupleLen();
        }

        cols_.insert(cols_.end(), right_cols.begin(), right_cols.end());
        is_end_ = false;
        fed_conds_ = std::move(conds);
    }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    // 原理：采用暴力匹配的思想，右表的第一个和左表的所有记录预先匹配，如果找到了直接返回。
    // 否则右边的记录前进，左边重新回到起点，继续模拟匹配，一直到右边的表结束,类似下面的循环思路。
    // for (int right = 0; right < n; right++)
    //      for (int left = 0, left < n; left++)

    void beginTuple() override {
        // 调用左边和右边表的beginTuple,分别找到第一个有效位置.
        left_->beginTuple();
        right_->beginTuple();
        // 如果左边或者右边不存在有效位置,说明连接条件结束,标记is_end_, 直接返回.
        if (left_->is_end() || right_->is_end()) {
            is_end_ = true;
            return;
        }
        // 否则就进行两表遍历，看看是否可以找到第一条有效的连接位置。
        find_record();
    }

    void nextTuple() override {
        // 如果进行到达末尾，直接返回。否则就找到当前位置的下一条有效位置（预先移动左端点）。
        if (is_end())
            return;
        left_->nextTuple();
        // 如果左边表到达了末尾了，右边表前进，左边表重新开始。
        if (left_->is_end()) {
            right_->nextTuple();
            left_->beginTuple();
        }
        // 继续找到下一条有效的记录位。
        find_record();
    }
    // 实现连接左右表的next接口，创建一个新的记录，先拷贝左边的记录，再拷贝右边的记录，最后返回答案就可以了。
    std::unique_ptr<RmRecord> Next() override {
        auto record = std::make_unique<RmRecord>(len_);
        // 找到左表和右表的记录,然后全部赋值给record.
        auto left_record = left_->Next();
        auto right_record = right_->Next();
        memcpy(record->data, left_record->data, left_->tupleLen());
        memcpy(record->data + left_->tupleLen(), right_record->data,
               right_->tupleLen());
        return record;
    }

    bool is_end() const override { return is_end_; }

    Rid& rid() override { return _abstract_rid; }

    void find_record() {
        while (!right_->is_end()) {
            auto record = std::make_unique<RmRecord>(len_);
            auto left_record = left_->Next();
            auto right_record = right_->Next();
            memcpy(record->data, left_record->data, left_->tupleLen());
            memcpy(record->data + left_->tupleLen(), right_record->data,
                   right_->tupleLen());
            if (fed_conds_.empty() ||
                Cal.eval_conds(fed_conds_, cols_, *record.get())) {
                return;
            }
            left_->nextTuple();
            if (left_->is_end()) {
                right_->nextTuple();
                left_->beginTuple();
            }
        }
        is_end_ = true;
    }
};