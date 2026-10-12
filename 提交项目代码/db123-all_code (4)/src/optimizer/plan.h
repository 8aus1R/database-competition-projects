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

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "parser/ast.h"

#include "parser/parser.h"

typedef enum PlanTag{
    T_Invalid = 1,
    T_Help,
    T_ShowTable,
    T_DescTable,
    T_CreateTable,
    T_DropTable,
    T_CreateIndex,
    T_DropIndex,
    T_ShowIndex,
    T_SetKnob,
    T_Insert,
    T_Update,
    T_Delete,
    T_select,
    T_Transaction_begin,
    T_Transaction_commit,
    T_Transaction_abort,
    T_Transaction_rollback,
    T_SeqScan,
    T_IndexScan,
    T_NestLoop,
    T_Aggregate,
    T_Group,
    T_Distinct,
    T_SortMerge,    // sort merge join
    T_Sort,
    T_Projection,
    T_Filter
} PlanTag;

// 查询执行计划
// 在计划这个部分添加一个接口进行打印输出，每次打印的时候只需要返回Explain接口即可。
class Plan
{
public:
    PlanTag tag;
    virtual ~Plan() = default;
    // explain 输出接口
    virtual void Explain(std::ostream& os, int indent = 0) const = 0;
};

class ScanPlan : public Plan
{
    public:
        ScanPlan(PlanTag tag, SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            conds_ = std::move(conds);
            TabMeta &tab = sm_manager->db_.get_table(tab_name_);
            cols_ = tab.cols;
            len_ = cols_.back().offset + cols_.back().len;
            fed_conds_ = conds_;
            index_col_names_ = index_col_names;
        
        }
        ~ScanPlan(){}
        // 以下变量同ScanExecutor中的变量
        std::string tab_name_;                     
        std::vector<ColMeta> cols_;                
        std::vector<Condition> conds_;             
        size_t len_;                               
        std::vector<Condition> fed_conds_;
        std::vector<std::string> index_col_names_;
        void Explain(std::ostream& os, int indent = 0) const override;
    
};

class FilterPlan : public Plan 
{
    public:
        FilterPlan(std::shared_ptr<Plan> subplan, std::vector<Condition> conds)
        {
            Plan::tag = T_Filter;
            subplan_ = std::move(subplan);
            conds_ = std::move(conds);
        }
        ~FilterPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<Condition> conds_;
        void Explain(std::ostream& os, int indent = 0) const override;
};

class JoinPlan : public Plan
{
    public:
        JoinPlan(PlanTag tag, std::shared_ptr<Plan> left, std::shared_ptr<Plan> right, std::vector<Condition> conds, JoinType type_ = JoinType::INNER_JOIN)
        {
            Plan::tag = tag;
            left_ = std::move(left);
            right_ = std::move(right);
            conds_ = std::move(conds);
            type = type_;
        }
        ~JoinPlan(){}
        // 左节点
        std::shared_ptr<Plan> left_;
        // 右节点
        std::shared_ptr<Plan> right_;
        // 连接条件
        std::vector<Condition> conds_;
        // future TODO: 后续可以支持的连接类型
        JoinType type;
        void Explain(std::ostream& os, int indent = 0) const override;
};

class ProjectionPlan : public Plan
{
    public:
        // 用这种构造函数就不需要去自己手动写两个了，如果少了一个参数，最后一个参数默认就是false，否则就是我们自己的值，比较方便。
        // 并且projection添加了一个参数，表示此时是否选择为*，如果为*就不进行投影下推了，我们放在投影进行判断。
        ProjectionPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols, bool is_select_all = false, int limit = -1)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_cols_ = std::move(sel_cols);
            is_select_all_ = is_select_all;
            limit_ = limit;
        }
        ~ProjectionPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_cols_;
        bool is_select_all_;
        int limit_;
        void Explain(std::ostream& os, int indent = 0) const override;
        
};

class DistinctPlan : public Plan
{
    public:
        // 这里需要进行多关键字排序，所以需要给参数都变为vector<>类型，然后对每一个条件依次进行排序判断。
        DistinctPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_col)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_col_ = sel_col;
        }
        ~DistinctPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_col_;
        void Explain(std::ostream& os, int indent = 0) const override {};
};

class SortPlan : public Plan
{
    public:
        // 这里需要进行多关键字排序，所以需要给参数都变为vector<>类型，然后对每一个条件依次进行排序判断。
        SortPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_col, std::vector<bool> is_desc, int limit)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_col_ = sel_col;
            is_desc_ = is_desc;
            limit_ = limit;
        }
        ~SortPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_col_;
        std::vector<bool> is_desc_;
        int limit_;
        void Explain(std::ostream& os, int indent) const override;
};

class AggregatePlan : public Plan
{
    public:
        // 添加聚合计划，支持三个参数，自己的tag，子计划以及选择的投影列。
        AggregatePlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_cols_ = sel_cols;
        }
        ~AggregatePlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_cols_;
        void Explain(std::ostream& os, int indent = 0) const override {}
};

class GroupPlan : public Plan
{
    public:
        // 添加分组计划，支持4个参数，自己的tag，子计划，投影列，group by选择的列，以及having条件。
        GroupPlan(PlanTag tag, std::shared_ptr<Plan> subplan, std::vector<TabCol> sel_cols, std::vector<TabCol> group_cols, std::vector<Condition> conds)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            sel_cols_ = sel_cols;
            group_cols_ = group_cols;
            conds_ = conds;
        }
        ~GroupPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::vector<TabCol> sel_cols_;
        std::vector<TabCol> group_cols_;
        std::vector<Condition> conds_;
        void Explain(std::ostream& os, int indent = 0) const override {}
};

// dml语句，包括insert; delete; update; select语句　
class DMLPlan : public Plan
{
    public:
        DMLPlan(PlanTag tag, std::shared_ptr<Plan> subplan,std::string tab_name,
                std::vector<Value> values, std::vector<Condition> conds,
                std::vector<SetClause> set_clauses)
        {
            Plan::tag = tag;
            subplan_ = std::move(subplan);
            tab_name_ = std::move(tab_name);
            values_ = std::move(values);
            conds_ = std::move(conds);
            set_clauses_ = std::move(set_clauses);
        }
        ~DMLPlan(){}
        std::shared_ptr<Plan> subplan_;
        std::string tab_name_;
        std::vector<Value> values_;
        std::vector<Condition> conds_;
        std::vector<SetClause> set_clauses_;
        void Explain(std::ostream& os, int indent = 0) const override {
            if (tag == T_select && subplan_) {
                subplan_->Explain(os, indent);
            }
        }
};

// ddl语句, 包括create/drop table; create/drop index;
class DDLPlan : public Plan
{
    public:
        DDLPlan(PlanTag tag, std::string tab_name, std::vector<std::string> col_names, std::vector<ColDef> cols)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);
            cols_ = std::move(cols);
            tab_col_names_ = std::move(col_names);
        }
        ~DDLPlan(){}
        std::string tab_name_;
        std::vector<std::string> tab_col_names_;
        std::vector<ColDef> cols_;
        void Explain(std::ostream& os, int indent = 0) const override {}
};

// help; show tables; desc tables; begin; abort; commit; rollback语句对应的plan
class OtherPlan : public Plan
{
    public:
        OtherPlan(PlanTag tag, std::string tab_name)
        {
            Plan::tag = tag;
            tab_name_ = std::move(tab_name);            
        }
        ~OtherPlan(){}
        std::string tab_name_;
        void Explain(std::ostream& os, int indent = 0) const override {}
};

// Set Knob Plan
class SetKnobPlan : public Plan
{
    public:
        SetKnobPlan(ast::SetKnobType knob_type, bool bool_value) {
            Plan::tag = T_SetKnob;
            set_knob_type_ = knob_type;
            bool_value_ = bool_value;
        }
    ast::SetKnobType set_knob_type_;
    bool bool_value_;
    void Explain(std::ostream& os, int indent = 0) const override {}
};

class plannerInfo{
    public:
    std::shared_ptr<ast::SelectStmt> parse;
    std::vector<Condition> where_conds;
    std::vector<TabCol> sel_cols;
    std::shared_ptr<Plan> plan;
    std::vector<std::shared_ptr<Plan>> table_scan_executors;
    std::vector<SetClause> set_clauses;
    plannerInfo(std::shared_ptr<ast::SelectStmt> parse_):parse(std::move(parse_)){}

};
