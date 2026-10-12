#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

#include <unordered_set>

// 去重算子的实现，还需要改进，去重和聚合函数的语法没有实现，以及去重计划树的位置也不是很完美。

class DistinctExecutor : public AbstractExecutor {

private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    std::vector<TabCol> sel_cols_;
    std::vector<std::unique_ptr<RmRecord>> cnt;
    std::unordered_set<std::string> s;
    int len;
    int idx;
public:
    DistinctExecutor(std::unique_ptr<AbstractExecutor> prev,
                  const std::vector<TabCol>& sel_cols) {
        prev_ = std::move(prev);
        cols_ = prev_->cols();
        sel_cols_ = std::move(sel_cols);
        len = 0;
        for (auto & sel_col : sel_cols) {
            auto pos = *get_col(cols_, sel_col);
            len += pos.len;
        }
        idx = 0;
    }

    void beginTuple() override {
        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto rec = prev_->Next();
            auto record = std::make_unique<RmRecord>(len); 
            int offset = 0;
            for (int i = 0; i < sel_cols_.size(); i++) {
                auto col = *get_col(cols_, sel_cols_[i]);
                if (prev_->is_aggregate()) {
                    col = cols_[i];
                }
                memcpy(record->data + offset, rec->data + col.offset, col.len);
                offset += col.len;
            }
            auto rec_str = std::string(record->data, len);
            if (!s.count(rec_str)) {
                s.insert(rec_str);
                cnt.push_back(std::move(record));
            }
            prev_->nextTuple();
        }
        idx = 0;
    }

    void nextTuple() override {
        idx++;
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(cnt[idx]);
    }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    Rid& rid() override { return _abstract_rid; }

    bool is_end() const override {
        return idx >= (int)cnt.size();
    }

    bool is_aggregate() override {
        return prev_->is_aggregate();
    }

    std::string getType() override { return "Distinct"; }
};