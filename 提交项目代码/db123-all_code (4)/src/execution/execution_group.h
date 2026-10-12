#pragma once
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

#include <unordered_map>

class GroupExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> group_cols_;
    std::vector<Condition> having_conds_;
    std::map<std::string, std::vector<std::unique_ptr<RmRecord>>>::iterator cnt;
    ConditionEvaluator Cal;
   public:
    std::map<std::string, std::vector<std::unique_ptr<RmRecord>>> grouped_records;

    GroupExecutor(std::unique_ptr<AbstractExecutor> prev,
                  const std::vector<TabCol>& sel_cols,
                  const std::vector<TabCol>& group_cols,
                  std::vector<Condition> conds) {
        prev_ = std::move(prev);
        for (const auto& sel_col : sel_cols) {
            if (sel_col.col_name == "*" &&
                sel_col.agg_type == ast::Aggregatetype::SV_TYPE_COUNT) {
                cols_.push_back(ColMeta{.tab_name = "",
                                        .name = "*",
                                        .type = TYPE_INT,
                                        .len = sizeof(int),
                                        .offset = 0});
                continue;
            }
            cols_.push_back(*get_col(prev_->cols(), sel_col));
        }
        for (const auto& group_col : group_cols) {
            group_cols_.push_back(*get_col(prev_->cols(), group_col));
        }
        having_conds_ = conds;
    }

    void beginTuple() override {
        prev_->beginTuple();
        grouped_records.clear();
        // 给所有记录进行存储并且分组。
        while (!prev_->is_end()) {
            auto tuple = prev_->Next();
            std::string group_key = generateGroupKey(tuple);
            grouped_records[group_key].emplace_back(std::move(tuple));
            prev_->nextTuple();
        }
        // 条件过滤，处理having条件，找到不合法的情况放入到ret中，从分组中删除。
        std::vector<std::string> ret;
        for (auto & [key, records] : grouped_records) {
            if (having_conds_.empty()) {
                break;
            }
            if (!eval_aggr_conds(cols_, having_conds_, records)) {
                ret.push_back(key);
            }
        }
        for (auto & key : ret) {
            grouped_records.erase(key);
        }
        cnt = grouped_records.begin();
    }

    void nextTuple() override {
        cnt++;
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(cnt->second.front());
    }

    const std::vector<ColMeta>& cols() const override { return prev_->cols(); }

    Rid& rid() override { return _abstract_rid; }

    bool is_end() const override {
        return cnt == grouped_records.end();
    }

    bool is_aggregate() override {
        return prev_->is_aggregate();
    }

   private:
    // 生成分组关键字。
    std::string generateGroupKey(std::unique_ptr<RmRecord>& record) {
        std::string group_key = "";
        for (const auto& group_col : group_cols_) {
            const char* col_data = record->data + group_col.offset;
            group_key += std::string(col_data, group_col.len);
        }
        return group_key;
    }
    // 判断此时的分组条件是否满足其中一个having条件。
    bool eval_aggr_cond(const std::vector<ColMeta>& rec_cols,
                        const Condition& cond,
                        std::vector<std::unique_ptr<RmRecord>>& rec) {
        auto copy_cond = cond;
        Value lhs_val = Cal.get_aggr_value(rec_cols, rec, copy_cond.lhs_col);
        Value rhs_val;
        if (cond.is_rhs_val) {
            rhs_val = cond.rhs_val;
        } else {
            rhs_val = Cal.get_aggr_value(rec_cols, rec, copy_cond.rhs_col);
        }
        return Cal.check_cond(lhs_val, rhs_val, cond.op);
    }

    bool eval_aggr_conds(const std::vector<ColMeta>& rec_cols,
                         const std::vector<Condition>& conds,
                         std::vector<std::unique_ptr<RmRecord>>& records) {
        return std::all_of(conds.begin(), conds.end(), [&] (const Condition& cond) {
            return eval_aggr_cond(rec_cols, cond, records);
        });
    }

    std::string getType() override { return "GROUP"; }
};