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

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle* fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_;
    std::unique_ptr<RecScan> scan_;  // table_iterator

    SmManager* sm_manager_;

    ConditionEvaluator Cal;

   public:
    SeqScanExecutor(SmManager* sm_manager, std::string tab_name,
                    std::vector<Condition> conds, Context* context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta& tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        // 记录是表中的一行数据，会存在很多字段。那么记录的长度就是最后一个字段的偏移量+最后一个字段的长度。
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta>& cols() const override { return cols_; }
    // scan作为一个连接条件，beginTuple是为了定位到第一条有效记录，然后通过next()将这条记录传递上去。
    void beginTuple() override {
        // 创建一个扫描记录scan_并且会自动指向第一个有效位，然后根据where的限制找到第一个合法记录位置。
        scan_ = std::make_unique<RmScan>(fh_);
        while (!scan_->is_end()) {  // 从头开始扫描
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            // 如果fed为空（表示不存在where条件）或者eval_conds成立（当前记录符合where条件），说明找到了第一个有效位，直接返回。
            if (fed_conds_.empty() ||
                Cal.eval_conds(fed_conds_, cols_, *rec.get())) {
                break;
            }
            // 否则就前往下一个有效的记录位进行查找。
            scan_->next();
        }
    }

    void nextTuple() override {
        // 找到当前rid_的下一个合法位置。

        // 前面下一个有效的记录位。
        if (!scan_->is_end()) {
            scan_->next();
        }
        // 找到第一个符合where条件的记录位。
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (fed_conds_.empty() ||
                Cal.eval_conds(fed_conds_, cols_, *rec.get())) {
                break;
            }
            scan_->next();
        }
    }
    // 实现scan的next()接口。返回当前rid_的记录就可以了，可以用过文件句柄调用get_record函数解决。
    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return scan_->is_end(); }

    Rid& rid() override { return rid_; }

};