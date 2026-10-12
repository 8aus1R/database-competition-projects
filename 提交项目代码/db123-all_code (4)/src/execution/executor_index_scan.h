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

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表名称
    TabMeta tab_;                       // 表的元数据
    std::vector<Condition> conds_;      // 扫描条件
    RmFileHandle* fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // 需要读取的字段
    size_t len_;                        // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;  // 扫描条件，和conds_字段相同

    std::vector<std::string>
        index_col_names_;   // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;  // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;
    IxIndexHandle* ih;

    SmManager* sm_manager_;

    ConditionEvaluator Cal;
    
   public:
    IndexScanExecutor(SmManager* sm_manager, std::string tab_name,
                      std::vector<Condition> conds,
                      std::vector<std::string> index_col_names,
                      Context* context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names;
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        // 得到索引句柄，方便对索引进行操作。
        auto index_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols);
        ih = sm_manager_->ihs_.at(index_name).get();

        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {{OP_EQ, OP_EQ}, {OP_NE, OP_NE},
                                            {OP_LT, OP_GT}, {OP_GT, OP_LT},
                                            {OP_LE, OP_GE}, {OP_GE, OP_LE}};
        // 保证对索引操作的列一定在左边，方便后续进行判断。
        for (auto& cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
        context_->lock_mgr_->lock_shared_on_table(
            context_->txn_, sm_manager_->fhs_[tab_name_]->GetFd());
    }

    void beginTuple() override {
        // 找到索引的最小和最大的范围。
        std::string ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_col_names_);
        RmRecord lower_record(index_meta_.col_tot_len), upper_record(index_meta_.col_tot_len);
        // 遍历选择的索引，构造需要查找的一段区间的记录,然后对这段区间进行扫描.
        int offset = 0;
        for (auto col : index_meta_.cols) {
            // 初始化最大和最小值，最小值最开始位于最左边的位置,如果不存在就从最左边开始扫描.
            // 最大值最开始位于最右边的位置,如果不存在最大值就处于最右边,最终会扫描到末尾.
            Value max_value, min_value; 
            // int flag_l = -1, flag_r = -1;
            if (col.type == TYPE_INT) {
                max_value.set_int(INT32_MAX);
                min_value.set_int(INT32_MIN);
            } else if (col.type == TYPE_FLOAT) {
                max_value.set_float(__DBL_MAX__);
                min_value.set_float(__DBL_MIN__);
            } else if (col.type == TYPE_STRING) {
                max_value.set_str(std::string(col.len, 255));
                min_value.set_str(std::string(col.len, 0));
            }
            min_value.init_raw(col.len);
            max_value.init_raw(col.len);
            // 这里分为3种情况.等于,小于/小于等于.大于/大于等于.不等于我们不进行索引.
            // 首先对于等于的情况,如果存在id = 2 or id = 3 or id = 5这种，找到最小和最大的区间[2,5]进行扫描。
            // 如果是小于符号，比如id < 50 or id < 60. 那么我们只需要更新最大值就可以了，找到一个最大的。
            // 如果是大于符号，比如id > 50 or id > 60. 那么对于这种情况，我们需要修改最小值即可。
            // 但是本题这样写过不了，由于不存在or运算，如果只按照或运算进行的话太慢了，所以这里也要修复。
            // 可以在where条件中判断是否为or，如果是的话就需要缩小，否则就扩大。
            // for (const auto& cond : fed_conds_) {
            //     if (cond.lhs_col.col_name == col.name && cond.is_rhs_val) {
            //         if (cond.op == OP_EQ) {
            //             if (!flag_l || check_cond(cond.rhs_val, min_value, OP_LT)) {
            //                 min_value = cond.rhs_val;
            //                 flag_l = 1;
            //             }
            //             if (!flag_r || check_cond(cond.rhs_val, max_value, OP_GT)) {
            //                 max_value = cond.rhs_val;
            //                 flag_r = 1;
            //             }
            //         } else if (cond.op == OP_LT || cond.op == OP_LE) {
            //             if (!flag_r || check_cond(cond.rhs_val, max_value, OP_GT)) {
            //                 max_value = cond.rhs_val;
            //                 flag_r = 1;
            //             }
            //         } else if (cond.op == OP_GT || cond.op == OP_GE) {
            //             if (!flag_l || check_cond(cond.rhs_val, min_value, OP_LT)) {
            //                 min_value = cond.rhs_val;
            //                 flag_l = 1;
            //             }
            //         }
            //     }
            // }

            // 对于与运算操作就是都必须满足，那么最小值就是最大的一个，最大值就是最小的一个，实现就比较简单了。
            // 后面可以单独判断是否为与运算和或运算，然后不断地修改区间的范围。
            // 如果为等于情况就是单独更新，如果是小于符号就更新最大值为其中的最小值，如果是大于情况就更新最小值为其中的最大值。
            for (const auto& cond : fed_conds_) {
                if (cond.lhs_col.col_name == col.name && cond.is_rhs_val) {
                    if (cond.op == OP_EQ) {
                        max_value = cond.rhs_val;
                        min_value = cond.rhs_val;
                    } else if (cond.op == OP_LT || cond.op == OP_LE) {
                        if (Cal.check_cond(cond.rhs_val, max_value, OP_LT)) max_value = cond.rhs_val;
                    } else if (cond.op == OP_GT || cond.op == OP_GE) {
                        if (Cal.check_cond(cond.rhs_val, min_value, OP_GT)) min_value = cond.rhs_val;
                    }
                }
            }
            memcpy(upper_record.data + offset, max_value.raw->data, col.len);
            memcpy(lower_record.data + offset, min_value.raw->data, col.len);
            offset += col.len;
        }
        auto start = ih->lower_bound(lower_record.data);
        auto end = ih->upper_bound(upper_record.data);
        scan_ = std::make_unique<IxScan>(ih, start, end, sm_manager_->get_bpm());

        while (!is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (fed_conds_.empty() || Cal.eval_conds(fed_conds_, cols_, *rec.get())) {
                break;
            }
            scan_->next();
        }
    }

    size_t tupleLen() const override {
        return len_;
    }

    void nextTuple() override {
        if (!is_end()) {
            scan_->next();
        }
        while (!is_end()) {
            rid_ = scan_->rid();
            auto record = fh_->get_record(rid_, context_);
            if (fed_conds_.empty() || Cal.eval_conds(fed_conds_, cols_, *record.get())) {
                break;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override {
        if (scan_->is_end()) return true;
        return false;
    }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    Rid& rid() override { return rid_; }

};