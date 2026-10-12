/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "planner.h"

#include <memory>
#include <set>
#include <map>

#include "execution/executor_delete.h"
#include "execution/executor_index_scan.h"
#include "execution/executor_insert.h"
#include "execution/executor_nestedloop_join.h"
#include "execution/executor_projection.h"
#include "execution/executor_seq_scan.h"
#include "execution/executor_update.h"
#include "index/ix.h"
#include "record_printer.h"

// 将操作符转化为字符串，方便后续打印操作。
static std::string comp_op2str(CompOp op) {
    switch (op) {
        case OP_EQ: return "=";
        case OP_NE: return "!=";
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        default: return "?";
    }
}


// 目前的索引匹配规则为：完全匹配索引字段，且全部为单点查询，不会自动调整where条件的顺序
bool Planner::get_index_cols(std::string tab_name, std::vector<Condition> curr_conds, std::vector<std::string>& index_col_names) {
    index_col_names.clear();
    for(auto& cond: curr_conds) {
        if(cond.is_rhs_val && cond.op == OP_EQ && cond.lhs_col.tab_name.compare(tab_name) == 0)
            index_col_names.push_back(cond.lhs_col.col_name);
    }
    TabMeta& tab = sm_manager_->db_.get_table(tab_name);
    if(tab.is_index(index_col_names)) return true;
    return false;
}

/**
 * @brief 表算子条件谓词生成
 *
 * @param conds 条件
 * @param tab_names 表名
 * @return std::vector<Condition>
 */
// 找到一个表中所有存在谓词的条件，找出来并返回，为后面谓词下推做准备。
std::vector<Condition> pop_conds(std::vector<Condition> &conds, std::string tab_names) {
    // auto has_tab = [&](const std::string &tab_name) {
    //     return std::find(tab_names.begin(), tab_names.end(), tab_name) != tab_names.end();
    // };
    std::vector<Condition> solved_conds;
    if (conds.empty()) return solved_conds;
    auto it = conds.begin();
    while (it != conds.end()) {
        if ((tab_names.compare(it->lhs_col.tab_name) == 0 && it->is_rhs_val) || (it->lhs_col.tab_name.compare(it->rhs_col.tab_name) == 0)) {
            solved_conds.emplace_back(std::move(*it));
            it = conds.erase(it);
        } else {
            it++;
        }
    }
    return solved_conds;
}

// 进行谓词下推操作，这里目前没有用到，需要继续改善当前函数。

int push_conds(Condition *cond, std::shared_ptr<Plan> plan)
{
    if(auto x = std::dynamic_pointer_cast<ScanPlan>(plan))
    {
        if(x->tab_name_.compare(cond->lhs_col.tab_name) == 0) {
            return 1;
        } else if(x->tab_name_.compare(cond->rhs_col.tab_name) == 0){
            return 2;
        } else {
            return 0;
        }
    }
    else if(auto x = std::dynamic_pointer_cast<JoinPlan>(plan))
    {
        int left_res = push_conds(cond, x->left_);
        // 条件已经下推到左子节点
        if(left_res == 3){
            return 3;
        }
        int right_res = push_conds(cond, x->right_);
        // 条件已经下推到右子节点
        if(right_res == 3){
            return 3;
        }
        // 左子节点或右子节点有一个没有匹配到条件的列
        if(left_res == 0 || right_res == 0) {
            return left_res + right_res;
        }
        // 左子节点匹配到条件的右边
        if(left_res == 2) {
            // 需要将左右两边的条件变换位置
            std::map<CompOp, CompOp> swap_op = {
                {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
            };
            std::swap(cond->lhs_col, cond->rhs_col);
            cond->op = swap_op.at(cond->op);
        }
        x->conds_.emplace_back(std::move(*cond));
        return 3;
    }
    return false;
}

// 弹出扫描计划，目前这个函数暂时也没有用到，代完善。

std::shared_ptr<Plan> pop_scan(int *scantbl, std::string table, std::vector<std::string> &joined_tables, 
                std::vector<std::shared_ptr<Plan>> plans)
{
    for (size_t i = 0; i < plans.size(); i++) {
        auto x = std::dynamic_pointer_cast<ScanPlan>(plans[i]);
        if(x->tab_name_.compare(table) == 0)
        {
            scantbl[i] = 1;
            joined_tables.emplace_back(x->tab_name_);
            return plans[i];
        }
    }
    return nullptr;
}

// 进行逻辑优化，这里主要是处理投影下推的操作，提前放入到一个hash表中，并且对where操作进行合并。

std::shared_ptr<Query> Planner::logical_optimization(std::shared_ptr<Query> query, Context *context)
{
    // 给where条件赋值每一个连接类型，然后将join和where条件合并了.
    for (int i = 0; i < (int)query->conds.size(); i++) {
        query->conds[i].join_type = JoinType::INNER_JOIN;
    }
    for (auto & t : query->join_conds) {
        query->conds.insert(query->conds.begin(), t.conds.begin(), t.conds.end());
    }

    if (!query->join_conds.empty() && !query->is_select_all) {
        std::map<std::string, std::set<std::string>> select_cols;  // 最终SELECT需要的列
        std::map<std::string, std::set<std::string>> join_cols;    // JOIN条件需要的列
        std::map<std::string, TabCol> cnt;                         // 用来记录tabcol，方便找到表的别名。
        // 初始化
        query->table_projections.clear();
        for (const auto& table : query->tables) {
            select_cols[table] = std::set<std::string>();
            join_cols[table] = std::set<std::string>();
        }
        
        // 1. 从SELECT列表中收集需要的列
        for (const auto& col : query->cols) {
            if (!col.tab_name.empty()) {
                select_cols[col.tab_name].insert(col.col_name);
                cnt[col.tab_name] = col;
            }
        }
        
        // 2. 对选择的where条件进行投影下推操作。
        for (const auto& cond : query->conds) {
            auto lhs_col = cond.lhs_col;
            // 如果当前不需要进行谓词下推，我们才进行投影。
            // 这里必须先判断再全部投影，不可以先写左边然后再判断右边是否合法。
            if (!cond.is_rhs_val) {
                // 投影左边
                join_cols[lhs_col.tab_name].insert(lhs_col.col_name);
                cnt[lhs_col.tab_name] = lhs_col;
                // 投影右边
                auto rhs_col = cond.rhs_col;
                join_cols[rhs_col.tab_name].insert(rhs_col.col_name);
                cnt[rhs_col.tab_name] = rhs_col;
            }
        }
        for (const auto& table : query->tables) {
            
            // 添加SELECT需要的列
            for (const auto& col_name : select_cols[table]) {
                std::string as_name = cnt[table].as_name;
                TabCol tab_col = {.tab_name = table, .col_name = col_name, .as_name = as_name, .agg_type = ast::Aggregatetype::SV_TYPE_NONE};
                query->table_projections[table].push_back(tab_col);
            }
            
            // 添加JOIN需要的列（但不在SELECT中的）
            for (const auto& col_name : join_cols[table]) {
                if (select_cols[table].find(col_name) == select_cols[table].end()) {
                    std::string as_name = cnt[table].as_name;
                    TabCol tab_col = {.tab_name = table, .col_name = col_name, .as_name = as_name, .agg_type = ast::Aggregatetype::SV_TYPE_NONE};
                    query->table_projections[table].push_back(tab_col);
                }
            }
        }
    }
    
    return query;
}

// 物理优化，主要是对一些计划的生成操作，比如聚合计划的生成，连接条件的生成。

std::shared_ptr<Plan> Planner::physical_optimization(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plan = make_one_rel(query);
    
    // 其他物理优化
    // 分组计划
    plan = generate_Group_plan(query, std::move(plan));
    // 聚合计划
    plan = generate_Aggregate_plan(query, std::move(plan));

    // 处理orderby
    plan = generate_sort_plan(query, std::move(plan)); 

    // 处理distinct
    plan = generate_Distinct_plan(query, std::move(plan));

    return plan;
}


// 连接条件的生成。
std::shared_ptr<Plan> Planner::make_one_rel(std::shared_ptr<Query> query)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    std::vector<std::string> tables = query->tables;
    // // Scan table , 生成表算子列表tab_nodes
    std::vector<std::shared_ptr<Plan>> table_scan_executors(tables.size());
    std::vector<int> table_cardinalities;
    // 首先对每一个表进行谓词下推操作，将他们格式的扫描计划统计出来并放入到table_scan_executors中去。
    for (size_t i = 0; i < tables.size(); i++) {
        auto curr_conds = pop_conds(query->conds, tables[i]);
        // int index_no = get_indexNo(tables[i], curr_conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(tables[i], curr_conds, index_col_names);
        std::shared_ptr<Plan> scan_plan;
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            scan_plan = std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, tables[i], curr_conds, index_col_names);
        } else {  // 存在索引
            scan_plan = std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, tables[i], curr_conds, index_col_names);
        }
        // 如果有条件，创建FilterPlan
        if (!index_exist && !curr_conds.empty()) {
            scan_plan = std::make_shared<FilterPlan>(scan_plan, curr_conds);
        }

        // 投影下推：在Scan上方添加Project节点，但是当SELECT *时不添加
        if (!query->is_select_all && query->table_projections.count(tables[i])) {
            scan_plan = std::make_shared<ProjectionPlan>(T_Projection, scan_plan, query->table_projections[tables[i]], query->is_select_all, x->limit);
        }
        table_scan_executors[i] = scan_plan;
        table_cardinalities.push_back(sm_manager_->db_.get_table(tables[i]).row_count);
    }
    // 只有一个表，不需要join。
    if(tables.size() == 1)
    {
        return table_scan_executors[0];
    }
    // 获取where条件
    auto conds = std::move(query->conds);

    // 准备好连接的顺序要求。
    std::vector<std::shared_ptr<Plan>> join_order;
    std::vector<std::string> join_order_names;

    std::vector<bool> used((int)tables.size(), false); // 标记哪些表以及被使用过了。

    // 处理semi_join，这里只支持两表之间的semi_join, 多表semi_join连接以及和semi_join和join搭配使用待完善。

    if (!query->join_conds.empty()) {
        std::shared_ptr<Plan> semi_join_plan = nullptr;
        for (int i = 0; i < (int)query->join_conds.size(); i++) {

            if (query->join_conds[i].join_type == JoinType::SEMI_JOIN) {

                auto left_idx = std::find(query->tables.begin(), query->tables.end(), query->join_conds[i].left) - query->tables.begin();
                auto right_idx = std::find(query->tables.begin(), query->tables.end(), query->join_conds[i].right) - query->tables.begin();

                auto left_plan = table_scan_executors[left_idx];
                auto right_plan = table_scan_executors[right_idx];

                // 创建SEMI JOIN计划
                semi_join_plan = std::make_shared<JoinPlan>(T_NestLoop, std::move(left_plan), std::move(right_plan), query->join_conds[i].conds, JoinType::SEMI_JOIN);
            }
        }
        if (semi_join_plan != nullptr) return semi_join_plan;
    }
    // 两表的连接价值就是记录的个数，每次需要两个表连接价值最小，从最小的方案开始就是最合适的。
    // 从剩余表中依次选择价值最小的表，放入到join_order中去。
    for (int k = 0; k < tables.size(); k++) {
        int min_next = -1;
        for (int i = 0; i < (int)tables.size(); ++i) {
            if (!used[i] && (min_next == -1 || table_cardinalities[i] < table_cardinalities[min_next])) min_next = i;
        }
        used[min_next] = true;
        join_order.push_back(table_scan_executors[min_next]);
        join_order_names.push_back(tables[min_next]);
    }

    // 按顺序左深树 join
    std::shared_ptr<Plan> plan = join_order[0];
    for (int i = 1; i < (int)join_order.size(); ++i) {
        // 查找连接条件：新表与左子树中所有表的连接条件
        std::vector<Condition> join_conds;
        std::string new_table = join_order_names[i];
        
        // 递归收集左子树中的所有表名，便于查找是否存在与当前表有where条件的。
        std::set<std::string> left_tables;
        std::function<void(const Plan*)> collect_left_tables = [&](const Plan* p) {
            if (auto scan = dynamic_cast<const ScanPlan*>(p)) {
                left_tables.insert(scan->tab_name_);
            } else if (auto filter = dynamic_cast<const FilterPlan*>(p)) {
                collect_left_tables(filter->subplan_.get());
            } else if (auto projection = dynamic_cast<const ProjectionPlan*>(p)) {
                collect_left_tables(projection->subplan_.get());
            } else if (auto sort = dynamic_cast<const SortPlan*>(p)) {
                collect_left_tables(sort->subplan_.get());
            } else if (auto join = dynamic_cast<const JoinPlan*>(p)) {
                collect_left_tables(join->left_.get());
                collect_left_tables(join->right_.get());
            }
        };
        collect_left_tables(plan.get());
        // 查找涉及新表和左子树中任意表的连接条件
        for (const auto& cond : conds) {
            if (!cond.is_rhs_val) {
                // 检查条件是否涉及新表和左子树中的表
                bool lhs_is_new = (cond.lhs_col.tab_name == new_table);
                bool rhs_is_new = (cond.rhs_col.tab_name == new_table);
                bool lhs_in_left = left_tables.count(cond.lhs_col.tab_name) > 0;
                bool rhs_in_left = left_tables.count(cond.rhs_col.tab_name) > 0;
                
                // 如果where条件中存在一个表是新表，另一个表是左子树中的一个表，说明此时where条件成立，进行连接操作。
                // 并且这里我们也不需要担心重复的操作，因为每次连接表都是依次相连，依次操作就会统计出所有where条件，不存在不合法情况。
                if ((lhs_is_new && rhs_in_left) || (rhs_is_new && lhs_in_left)) {
                    join_conds.push_back(cond);
                }
            }
        }
        plan = std::make_shared<JoinPlan>(T_NestLoop, plan, join_order[i], join_conds);
    }
    return plan;

}

std::shared_ptr<Plan> Planner::generate_sort_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if (!x->has_sort) return plan;
    return std::make_shared<SortPlan>(T_Sort, std::move(plan), query->order_by, query->is_desc, x->limit);
}

std::shared_ptr<Plan> Planner::generate_Aggregate_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if (x->has_aggregate == false) return plan;
    return std::make_shared<AggregatePlan>(T_Aggregate, std::move(plan), query->cols);
}

std::shared_ptr<Plan> Planner::generate_Group_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if (!x->has_group) {
        return plan;
    }   
    
    return std::make_shared<GroupPlan>(T_Group, std::move(plan), query->cols, query->group_cols, query->having_conds);
}

std::shared_ptr<Plan> Planner::generate_Distinct_plan(std::shared_ptr<Query> query, std::shared_ptr<Plan> plan)
{
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    if (!x->has_distinct) {
        return plan;
    }   
    
    return std::make_shared<DistinctPlan>(T_Distinct, std::move(plan), query->cols);
}


/**
 * @brief select plan 生成
 *
 * @param sel_cols select plan 选取的列
 * @param tab_names select plan 目标的表
 * @param conds select plan 选取条件
 */
std::shared_ptr<Plan> Planner::generate_select_plan(std::shared_ptr<Query> query, Context *context) {
    //逻辑优化
    query = logical_optimization(std::move(query), context);
    auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse);
    //物理优化
    auto sel_cols = query->cols;
    std::shared_ptr<Plan> plannerRoot = physical_optimization(query, context);
    plannerRoot = std::make_shared<ProjectionPlan>(T_Projection, std::move(plannerRoot), 
                                                        std::move(sel_cols), query->is_select_all, x->limit);

    return plannerRoot;
}

// 生成DDL语句和DML语句的查询执行计划
std::shared_ptr<Plan> Planner::do_planner(std::shared_ptr<Query> query, Context *context)
{
    std::shared_ptr<Plan> plannerRoot;
    if (auto x = std::dynamic_pointer_cast<ast::CreateTable>(query->parse)) {
        // create table;
        std::vector<ColDef> col_defs;
        for (auto &field : x->fields) {
            if (auto sv_col_def = std::dynamic_pointer_cast<ast::ColDef>(field)) {
                ColDef col_def = {.name = sv_col_def->col_name,
                                  .type = interp_sv_type(sv_col_def->type_len->type),
                                  .len = sv_col_def->type_len->len};
                col_defs.push_back(col_def);
            } else {
                throw InternalError("Unexpected field type");
            }
        }
        plannerRoot = std::make_shared<DDLPlan>(T_CreateTable, x->tab_name, std::vector<std::string>(), col_defs);
    } else if (auto x = std::dynamic_pointer_cast<ast::DropTable>(query->parse)) {
        // drop table;
        plannerRoot = std::make_shared<DDLPlan>(T_DropTable, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::CreateIndex>(query->parse)) {
        // create index;
        plannerRoot = std::make_shared<DDLPlan>(T_CreateIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DropIndex>(query->parse)) {
        // drop index
        plannerRoot = std::make_shared<DDLPlan>(T_DropIndex, x->tab_name, x->col_names, std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::ShowIndex>(query->parse)) {
        // show index
        plannerRoot = std::make_shared<DDLPlan>(T_ShowIndex, x->tab_name, std::vector<std::string>(), std::vector<ColDef>());
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(query->parse)) {
        // insert;
        plannerRoot = std::make_shared<DMLPlan>(T_Insert, std::shared_ptr<Plan>(),  x->tab_name,  
                                                    query->values, std::vector<Condition>(), std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(query->parse)) {
        // delete;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);
        
        if (index_exist == false) {  // 该表没有索引
            index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }

        plannerRoot = std::make_shared<DMLPlan>(T_Delete, table_scan_executors, x->tab_name,  
                                                std::vector<Value>(), query->conds, std::vector<SetClause>());
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(query->parse)) {
        // update;
        // 生成表扫描方式
        std::shared_ptr<Plan> table_scan_executors;
        // 只有一张表，不需要进行物理优化了
        // int index_no = get_indexNo(x->tab_name, query->conds);
        std::vector<std::string> index_col_names;
        bool index_exist = get_index_cols(x->tab_name, query->conds, index_col_names);

        if (index_exist == false) {  // 该表没有索引
        index_col_names.clear();
            table_scan_executors = 
                std::make_shared<ScanPlan>(T_SeqScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        } else {  // 存在索引
            table_scan_executors =
                std::make_shared<ScanPlan>(T_IndexScan, sm_manager_, x->tab_name, query->conds, index_col_names);
        }
        plannerRoot = std::make_shared<DMLPlan>(T_Update, table_scan_executors, x->tab_name,
                                                     std::vector<Value>(), query->conds, 
                                                     query->set_clauses);
    } else if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(query->parse)) {

        std::shared_ptr<plannerInfo> root = std::make_shared<plannerInfo>(x);
        // 生成select语句的查询执行计划
        std::shared_ptr<Plan> projection = generate_select_plan(std::move(query), context);
        plannerRoot = std::make_shared<DMLPlan>(T_select, projection, std::string(), std::vector<Value>(),
                                                    std::vector<Condition>(), std::vector<SetClause>());
    } else {
        throw InternalError("Unexpected AST root");
    }
    return plannerRoot;
}

// 用来实现explain接口打印

void print_indent(std::ostream& os, int indent) {
    for (int i = 0; i < indent; ++i) os << '\t';  // 一个tab缩进
}

void ScanPlan::Explain(std::ostream& os, int indent) const {
    print_indent(os, indent);
    os << "Scan(table=" << tab_name_ << ")\n";  // 使用真实表名，不是别名
}

void FilterPlan::Explain(std::ostream& os, int indent) const {
    print_indent(os, indent);
    os << "Filter(condition=[";
    std::vector<std::string> cond_strs;
    for (const auto& cond : conds_) {
        std::string lhs_tab_name = cond.lhs_col.as_name.empty() ? cond.lhs_col.tab_name : cond.lhs_col.as_name;
        std::string cond_str = lhs_tab_name + "." + cond.lhs_col.col_name;
        cond_str += comp_op2str(cond.op);
        if (cond.is_rhs_val) {
            if (cond.rhs_val.type == TYPE_STRING) {
                cond_str += "'" + cond.rhs_val.str_val + "'";
            } else if (cond.rhs_val.type == TYPE_INT) {
                cond_str += std::to_string(cond.rhs_val.int_val);
            } else if (cond.rhs_val.type == TYPE_FLOAT) {
                // 保持原始SQL格式，使用固定6位小数
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(6) << cond.rhs_val.float_val;
                cond_str += oss.str();
            }
        } else {
            std::string rhs_tab_name = cond.rhs_col.as_name.empty() ? cond.rhs_col.tab_name : cond.rhs_col.as_name;
            cond_str += rhs_tab_name + "." + cond.rhs_col.col_name;
        }
        cond_strs.push_back(cond_str);
    }
    std::sort(cond_strs.begin(), cond_strs.end());
    for (size_t i = 0; i < cond_strs.size(); ++i) {
        if (i > 0) os << ",";
        os << cond_strs[i];
    }
    os << "])\n";
    if (subplan_) subplan_->Explain(os, indent + 1);
}

void JoinPlan::Explain(std::ostream& os, int indent) const {
    print_indent(os, indent);
    
    // 收集所有表名
    std::vector<std::string> tables;
    std::function<void(const Plan*, std::vector<std::string>&)> collect_tables = [&](const Plan* p, std::vector<std::string>& ts) {
        if (auto scan = dynamic_cast<const ScanPlan*>(p)) {
            ts.push_back(scan->tab_name_);  // 使用真实表名，不是别名
        } else if (auto filter = dynamic_cast<const FilterPlan*>(p)) {
            collect_tables(filter->subplan_.get(), ts);
        } else if (auto projection = dynamic_cast<const ProjectionPlan*>(p)) {
            collect_tables(projection->subplan_.get(), ts);
        } else if (auto sort = dynamic_cast<const SortPlan*>(p)) {
            collect_tables(sort->subplan_.get(), ts);
        } else if (auto join = dynamic_cast<const JoinPlan*>(p)) {
            collect_tables(join->left_.get(), ts);
            collect_tables(join->right_.get(), ts);
        }
    };
    collect_tables(this, tables);
    std::sort(tables.begin(), tables.end());
    
    os << "Join(tables=[";
    for (size_t i = 0; i < tables.size(); ++i) {
        if (i) os << ",";
        os << tables[i];
    }
    os << "]";
    
    // 输出连接条件
    std::vector<std::string> conds_str;
    for (const auto& cond : conds_) {
        std::string lhs_tab_name = cond.lhs_col.as_name.empty() ? cond.lhs_col.tab_name : cond.lhs_col.as_name;
        std::string s = lhs_tab_name + "." + cond.lhs_col.col_name;
        // if (!cond.lhs_col.as_name.empty()) s += " AS " + cond.lhs_col.as_name;
        s += comp_op2str(cond.op);
        if (cond.is_rhs_val) {
            if (cond.rhs_val.type == TYPE_STRING) {
                s += "'" + cond.rhs_val.str_val + "'";
            } else if (cond.rhs_val.type == TYPE_INT) {
                s += std::to_string(cond.rhs_val.int_val);
            } else if (cond.rhs_val.type == TYPE_FLOAT) {
                // 保持原始SQL格式，使用固定6位小数
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(6) << cond.rhs_val.float_val;
                s += oss.str();
            }
        } else {
            std::string rhs_tab_name = cond.rhs_col.as_name.empty() ? cond.rhs_col.tab_name : cond.rhs_col.as_name;
            s += rhs_tab_name + "." + cond.rhs_col.col_name;
            // if (!cond.rhs_col.as_name.empty()) s += " AS " + cond.rhs_col.as_name;
        }
        conds_str.push_back(s);
    }
    std::sort(conds_str.begin(), conds_str.end());
    os << ",condition=[";
    for (size_t i = 0; i < conds_str.size(); ++i) {
        if (i) os << ",";
        os << conds_str[i];
    }
    os << "]";
    os << ")\n";
    
    // 递归输出子节点，按照Filter Join Project Scan的顺序输出
    std::vector<std::pair<int, std::shared_ptr<Plan>>> child_plans;
    
    // 定义节点类型的优先级
    auto get_plan_priority = [](const Plan* p) -> int {
        if (dynamic_cast<const FilterPlan*>(p)) return 1;
        if (dynamic_cast<const JoinPlan*>(p)) return 2;
        if (dynamic_cast<const ProjectionPlan*>(p)) return 3;
        if (dynamic_cast<const ScanPlan*>(p)) return 4;
        return 5; // 其他类型
    };
    
    // 收集子节点的表名（用于排序）
    std::function<std::string(const Plan*)> get_first_table = [&](const Plan* p) -> std::string {
        if (auto scan = dynamic_cast<const ScanPlan*>(p)) {
            return scan->tab_name_;
        } else if (auto filter = dynamic_cast<const FilterPlan*>(p)) {
            return get_first_table(filter->subplan_.get());
        } else if (auto projection = dynamic_cast<const ProjectionPlan*>(p)) {
            return get_first_table(projection->subplan_.get());
        }  else if (auto sort = dynamic_cast<const SortPlan*>(p)) {
            return get_first_table(sort->subplan_.get());
        } else if (auto join = dynamic_cast<const JoinPlan*>(p)) {
            return get_first_table(join->left_.get());
        }
        return "";
    };
    
    if (left_) {
        child_plans.emplace_back(get_plan_priority(left_.get()), left_);
    }
    if (right_) {
        child_plans.emplace_back(get_plan_priority(right_.get()), right_);
    }
    
    // 按优先级排序，同类型按表名升序排序
    std::sort(child_plans.begin(), child_plans.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first < b.first; // 按类型优先级排序
        }
        // 同类型按表名升序排序
        return get_first_table(a.second.get()) < get_first_table(b.second.get());
    });
    
    // 输出子节点
    for (const auto& child : child_plans) {
        child.second->Explain(os, indent + 1);
    }
}

void ProjectionPlan::Explain(std::ostream& os, int indent) const {
    print_indent(os, indent);
    os << "Project(columns=[";
    // 检查是否为select *
    if (is_select_all_) {
        os << "*";
    } else {
        std::vector<std::string> cols;
        for (const auto& c : sel_cols_) {
            std::string col_str;
            // 使用别名引用（如果有别名的话）
            if (!c.as_name.empty() && c.as_name != c.tab_name) {
                // 有别名，使用别名
                col_str = c.as_name + "." + c.col_name;
            } else if (!c.tab_name.empty()) {
                col_str = c.tab_name + "." + c.col_name;
            } else {
                col_str = c.col_name;
            }
            // if (!c.as_name.empty()) col_str += " AS " + c.as_name;
            cols.push_back(col_str);
        }
        std::sort(cols.begin(), cols.end());
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) os << ",";
            os << cols[i];
        }
    }
    os << "])\n";
    if (subplan_) subplan_->Explain(os, indent + 1);
}

void SortPlan::Explain(std::ostream& os, int indent) const {
    print_indent(os, indent);
    if (limit_ != -1) {
        os << " (limit=" << limit_ << ")";
    }
    os << "Sort By (";
    for (size_t i = 0; i < sel_col_.size(); ++i) {
        os << sel_col_[i].col_name << (is_desc_[i] ? " desc" : " asc");
        if (i != sel_col_.size() - 1) {
            os << ", ";
        }
    }
    os << ")" << std::endl;
    subplan_->Explain(os, indent + 1);
}