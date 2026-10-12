#pragma once
#include <climits>
#include <numeric>
#include <unordered_map>
#include <vector>
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "execution_group.h"
#include "index/ix.h"
#include "system/sm.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> output_cols_;
    std::vector<TabCol> sel_cols_;
    std::vector<std::unique_ptr<RmRecord>> aggregated_records_;
    int idx;

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev,
                      const std::vector<TabCol>& sel_cols)
        : prev_(std::move(prev)) {
        // 构造输出列
        for (const auto& sel_col : sel_cols) {
            if (sel_col.col_name == "*" &&
                sel_col.agg_type == ast::Aggregatetype::SV_TYPE_COUNT) {
                cols_.push_back(ColMeta{.tab_name = "",
                                        .name = "*",
                                        .type = TYPE_INT,
                                        .len = sizeof(int),
                                        .offset = 0});
            } else {
                cols_.push_back(*prev_->get_col(prev_->cols(), sel_col));
            }
            output_cols_.push_back(cols_.back());
        }
        // 聚合函数聚合改变类型, 如果是count就必须是int, 所以这里需要单独判断.
        output_cols_.front().offset = 0;
        if (sel_cols[0].agg_type == ast::Aggregatetype::SV_TYPE_COUNT) {
            output_cols_.front().type = TYPE_INT;
            output_cols_.front().len = sizeof(int);
        }else if (sel_cols[0].agg_type == ast::Aggregatetype::SV_TYPE_AVG) {
            output_cols_.front().type = TYPE_FLOAT;
            output_cols_.front().len = sizeof(float);
        }
        for (size_t i = 1; i < output_cols_.size(); ++i) {
            if (sel_cols[i].agg_type == ast::Aggregatetype::SV_TYPE_COUNT) {
                output_cols_[i].type = TYPE_INT;
                output_cols_[i].len = sizeof(int);
            } else if (sel_cols[i].agg_type == ast::Aggregatetype::SV_TYPE_AVG) {
                output_cols_[i].type = TYPE_FLOAT;
                output_cols_[i].len = sizeof(float);
            }
            output_cols_[i].offset =
                output_cols_[i - 1].offset + output_cols_[i - 1].len;
        }
        sel_cols_ = std::move(sel_cols);
    }
    void beginTuple() override {
        prev_->beginTuple();
        // 判断是否存在分组，如果存在那就需要将分组的所有记录进行聚合统计，否则就正常统计。
        aggregated_records_ = std::vector<std::unique_ptr<RmRecord>>();
        if (auto x = dynamic_cast<GroupExecutor*>(prev_.get())) {
            for (auto & [_, rec] : x->grouped_records) {
                auto record = aggregateGroup(rec);
                if (record) {
                    aggregated_records_.push_back(std::move(record));
                }
            }
        } else {
            std::vector<std::unique_ptr<RmRecord>> records;
            while (!prev_->is_end()) {
                records.push_back(prev_->Next());
                prev_->nextTuple();
            }
            auto record = aggregateGroup(records);
            if (record) {
                aggregated_records_.push_back(std::move(record));
            }
        }
        idx = 0;
    }

    void nextTuple() override {
        idx++;
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(aggregated_records_[idx]);
    }

    std::unique_ptr<RmRecord> aggregateGroup( // 将聚合函数这一条记录生成出来。
        const std::vector<std::unique_ptr<RmRecord>>& records) {
        bool is_count = true; // 判断是否全为count
        for (const auto& t : sel_cols_) {
            if (t.agg_type != ast::Aggregatetype::SV_TYPE_COUNT) {
                is_count = false;
                break;
            }
        }
        // 如果没有记录，且不全是count，返回空表示不合法，否则全返回0.
        // 这里还需要实现append函数,由于记录大小未知,相当于要实现一个尾差.
        if (records.empty()) {
            if (!is_count) {
                return nullptr;
            }

            auto result = std::make_unique<RmRecord>();
            for (int i = 0; i < (int)sel_cols_.size(); i++) {
                auto count_value = Value();
                count_value.set_int(0);
                count_value.init_raw();
                result->append(count_value.raw->data, count_value.raw->size);
            }
            return result;
        }
        auto result = std::make_unique<RmRecord>();
        for (size_t i = 0; i < sel_cols_.size(); ++i) {
            Value res = Cal.get_aggr_value(cols_, records, sel_cols_[i]);
            res.init_raw();
            result->append(res.raw->data, res.raw->size);
        }
        return result;
    }

    bool is_end() const override {
        return idx >= (int)aggregated_records_.size();
    }

    bool is_aggregate() override {
        return true;
    }

    const std::vector<ColMeta>& cols() const override { return output_cols_; }

    Rid& rid() override { return _abstract_rid; }

};