/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"
#include "optimizer/optimizer.h"
#include "portal.h"

extern std::unordered_map<std::string, std::vector<ColMeta>> rec_col;

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */

std::unordered_map<std::string, std::string> alias_tab_map; // 别名映射的hash表。

// 对val进行set赋值，对类型和具体大小进行操作。
static void val_set_init(Value & val, ColType & type, std::string x) {
    if (type == TYPE_INT) {
        val.set_int(*(int *)x.c_str());
    } else if (type == TYPE_FLOAT) {
        val.set_float(*(float *)x.c_str());
    } else if (type == TYPE_STRING) {
        val.set_str(x);
    } else if (type == TYPE_BIGINT) {
        val.set_bigint(BigInt(*(long long *)x.c_str()));
    } else if (type == TYPE_DATETIME) {
        val.set_datetime(DateTime(x));
    } else {
        throw InternalError("Unexpected sv value type");
    }
}

// 对val进行raw赋值，对真实的数据进行存入。
static void val_raw_init(Value & val, ColType & type, int len) {
    if (type == TYPE_FLOAT) {
        val.init_raw(sizeof(float));
    } else if (type == TYPE_INT) {
        val.init_raw(sizeof(int));
    } else if (type == TYPE_STRING) {
        val.init_raw(len);
    } else if (type == TYPE_BIGINT) {
        val.init_raw(sizeof(BigInt));
    } else if (type == TYPE_DATETIME) {
        val.init_raw(sizeof(DateTime));
    }
}

// 判断左右两边的类型是否匹配，如果不合法需要抛出错误。
static void check_type(ColType lhs_type, ColType rhs_type) {
    if ((lhs_type == TYPE_INT && rhs_type == TYPE_FLOAT) || (lhs_type == TYPE_FLOAT && rhs_type == TYPE_INT)){}
    else if ((lhs_type == TYPE_BIGINT && rhs_type == TYPE_INT) || (lhs_type == TYPE_INT && rhs_type == TYPE_BIGINT)){}
    else if (lhs_type == TYPE_STRING && rhs_type == TYPE_DATETIME){}
    else {
        throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
    }
    return ;
}

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        // 处理表名
        query->tables = std::move(x->tabs);
        alias_tab_map.clear();
        query->values.clear();
        /** TODO: 检查表是否存在 */
        for (int i = 0; i < (int)query->tables.size(); i++) {
            if (!sm_manager_->db_.is_table(query->tables[i])) {
                throw TableNotFoundError(query->tables[i]);
            }
            // 处理别名和表名的映射关系，可以通过别名去找到表名。
            if (x->alias[i] == "") {
                x->alias[i] = query->tables[i];
            }
            alias_tab_map[x->alias[i]] = query->tables[i];
            query->alias_tab_map[x->alias[i]] = query->tables[i];
        }
        // 处理target list，再target list中添加上表名，例如 a.id
        for (auto &sv_sel_col : x->cols) {
            TabCol sel_col = {.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name, .as_name = sv_sel_col->as_name, .agg_type = sv_sel_col->agg_type};
            query->cols.push_back(sel_col);
        }
        // 处理joinconds条件
        query->join_conds.clear();
        for (auto & t : x->jointree) {
            jointree join = {.left = t->left, .right = t->right, .conds = {}, .join_type = t->type};
            get_clause(t->conds, join.conds, query->tables);
            check_clause(join.conds);
            query->join_conds.push_back(join);
        }
        
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        if (query->cols.empty()) {
            // select all columns
            query->is_select_all = true;
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        } else {
            // infer table name from column name
            for (auto &sel_col : query->cols) {
                if (sel_col.col_name == "*" && sel_col.agg_type == ast::Aggregatetype::SV_TYPE_COUNT) {
                    sel_col.col_name = all_cols[0].name;
                    sel_col.tab_name = all_cols[0].tab_name;
                }
                sel_col = check_column(all_cols, sel_col);  // 列元数据校验
            }
        }

        // 处理group by
        for (auto& sv_group_col : x->group_cols) {
            
            TabCol group_col = {.tab_name = sv_group_col->tab_name,
                                .col_name = sv_group_col->col_name,
                                .as_name = sv_group_col->as_name,
                                .agg_type = sv_group_col->agg_type};
            query->group_cols.push_back(group_col);
        }
        // 如果有group by，检查group by的列是否存在
        for (auto& group_col : query->group_cols) {
            group_col = check_column(all_cols, group_col);  // Group列元数据校验
        }
        // 处理order by
        if (x->has_sort) {
            for (auto & t : x->order) {
                TabCol sel_col = {.tab_name = t->cols->tab_name, .col_name = t->cols->col_name,
                .as_name = t->cols->as_name, .agg_type = t->cols->agg_type};
                sel_col = check_column(all_cols, sel_col);
                query->order_by.push_back(sel_col);
                query->is_desc.push_back(t->orderby_dir == ast::OrderByDir::OrderBy_DESC ? true : false);
            }
        }

        // 之前check_column判断group by的列中不会判断count(*),这里重新检查一下。
        check_group(query->group_cols, query->tables);

        //处理where条件
        get_clause(x->conds, query->conds, query->tables);
        check_clause(query->conds);
        // 处理having条件
        get_clause(x->having_conds, query->having_conds, query->tables);
        check_clause(query->having_conds);   

        // 检查where条件中是否有聚合函数
        check_conds_with_aggregate(query->conds);
        // 检查having条件中是否有不是聚合函数也不是group by的列
        check_having_conds(query->having_conds, query->group_cols);
        // 检查group by和select中的列是否符合规范
        check_col_group_and_aggr(query->cols, query->group_cols);
        // 检测group by不存在时，是否有having条件
        check_without_group(query->group_cols, query->having_conds);

        

    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        /** TODO: */
        set_clause(x->tab_name, x->set_clauses, query->set_clauses);
        get_clause(x->conds, query->conds, {x->tab_name});
        check_clause(query->conds);  
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        //处理where条件
        get_clause(x->conds, query->conds, {x->tab_name});
        check_clause(query->conds);        
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}


TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                // to-do.当列名或者表名取了别名之后应该如何处理,semi_join部分如何处理
                // if (!tab_name.empty()) {
                //     throw AmbiguousColumnError(target.col_name);
                // }
                // if (!alias_tab_map.count(col.tab_name)) {
                //     throw TableNotFoundError(col.tab_name);
                // }
                tab_name = col.tab_name;
                break;
            }
        }
        if (tab_name.empty()) {
            if (target.col_name == "*" && target.agg_type == ast::Aggregatetype::SV_TYPE_COUNT) {
                return target;
            }
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
        if (!alias_tab_map.count(target.tab_name)) {
            for (auto & t : alias_tab_map) {
                if (t.second == tab_name) {
                    target.as_name = t.first;
                    break;
                }
            }
        }
    } else {
        /** TODO: Make sure target column exists */
        // 如果没有找到就一定是不合法的，表名一旦设置了别名就不可以用原先的名字了。
        if (!alias_tab_map.count(target.tab_name)) {
            throw TableNotFoundError(target.tab_name);
        }
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name && col.tab_name == alias_tab_map[target.tab_name]) {
                if (target.tab_name != alias_tab_map[target.tab_name]) {
                    target.as_name = target.tab_name;
                }
                target.tab_name = alias_tab_map[target.tab_name];
                return target;
            }
        }
        throw ColumnNotFoundError(target.col_name);
    }
    return target;
}

void Analyze::set_clause(std::string &tab_name, const std::vector<std::shared_ptr<ast::SetClause>> &sv_set_clauses, std::vector<SetClause> &set_clauses) {
    set_clauses.clear();

    for (auto & t : sv_set_clauses) {
        SetClause ret;
        ret.lhs = {.tab_name = tab_name, .col_name = t->col_name};
        ret.rhs = convert_sv_value(t->val);
        set_clauses.push_back(ret);
    }
    return ;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds, const std::vector<std::string> &tab_names) {
    conds.clear();
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name, 
            .as_name = expr->lhs->as_name, .agg_type = expr->lhs->agg_type};
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        cond.op = convert_sv_comp_op(expr->op);

        if (cond.op == OP_BETWEEN_AND) { // 先判断是否为between，这里单独进行操作。
            cond.rhs_between_val.first = convert_sv_value(expr->rhs_betwenn_val.first);
            cond.rhs_between_val.second = convert_sv_value(expr->rhs_betwenn_val.second);
        } else if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_val = convert_sv_value(rhs_val);
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name, 
            .as_name = rhs_col->as_name, .agg_type = rhs_col->agg_type};

            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        } else if (auto rhs_subquery = std::dynamic_pointer_cast<ast::SelectStmt>(expr->subquery)) {
            // 判断当前条件右边是否是子查询，如果是就单独对右边进行操作。
            // 将右边的查询条件执行完，然后将记录和对应的列元数据统计出来，方便后续判断是否合法。
            auto sub_Query = do_analyze(rhs_subquery);
            auto planner = std::make_unique<Planner>(sm_manager_);
            auto optimizer = std::make_unique<Optimizer>(sm_manager_, planner.get());
            std::shared_ptr<Plan> plan = optimizer->plan_query(sub_Query, nullptr);
            auto portal = std::make_unique<Portal>(sm_manager_);
            std::shared_ptr<PortalStmt> portalStmt = portal->start(plan, nullptr);
            auto ql_manager = std::make_unique<QlManager>(sm_manager_, nullptr, nullptr);
            ql_manager->sub_select_result(std::move(portalStmt->root), nullptr);
            
            // 如果右边子查询结果为空或者右边存在多个列，这些都是不合法的，需要舍弃。
            if (rec_col.size() == 0) {
                throw RMDBError("rhs_query is nullptr");
            }
            if (rec_col.begin()->second.size() > 1) {
                throw RMDBError("sub_select failure");
            }
            // 将当前位置标记为值，如果是in语句，那么右边就是一个值列表，否则就是一个值。
            cond.is_rhs_val = true;
            if (cond.op == OP_IN) {
                for (auto & [x, y] : rec_col) {
                    Value val;
                    auto rhs_type = y[0].type;
                    val_set_init(val, rhs_type, x);
                    cond.rhs_vals.push_back(val);
                }
            } else {
                // 如果是值，那么就只可以存在一条记录，不可以存在多行记录。
                if (rec_col.size() > 1) {
                    throw RMDBError("sub_select failure");
                }
                Value val;
                auto rhs_type = rec_col.begin()->second[0].type;
                val_set_init(val, rhs_type, rec_col.begin()->first);
                cond.rhs_val = val;
            }
        } else if (cond.op == OP_IN) {
            cond.is_rhs_val = true;
            for (auto & rhs_val : expr->in_vals) {
                cond.rhs_vals.push_back(convert_sv_value(rhs_val));
            }
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);

    // Get raw values in where clause
    for (auto &cond : conds) {
        if (cond.op == OP_BETWEEN_AND) {
            TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
            auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
            ColType lhs_type = lhs_col->type;
            ColType rhs_type1 = cond.rhs_between_val.first.type;
            ColType rhs_type2 = cond.rhs_between_val.second.type;
            if (rhs_type1 != rhs_type2) {
                check_type(rhs_type1, rhs_type2);
            }
            if (lhs_type != rhs_type1) {
                check_type(lhs_type, rhs_type1);
            }
            val_raw_init(cond.rhs_between_val.first, rhs_type1, lhs_col->len);
            val_raw_init(cond.rhs_between_val.second, rhs_type2, lhs_col->len);
            continue;
        }
        if (cond.lhs_col.agg_type == ast::Aggregatetype::SV_TYPE_COUNT &&
            cond.lhs_col.col_name == "*") {
            // count(*)类型一定是一个int，判断右边是什么类型，进行分类讨论。
            // 下面的判断条件可以参考不是count的说明.
            ColType lhs_type = TYPE_INT;
            ColType rhs_type;
            if (cond.is_rhs_val) {
                rhs_type = cond.rhs_val.type;
                cond.rhs_val.init_raw();
            } else {
                rhs_type = sm_manager_->db_.get_table(cond.rhs_col.tab_name)
                               .get_col(cond.rhs_col.col_name)
                               ->type;
            }
            if (lhs_type != rhs_type) {
                check_type(lhs_type, rhs_type);
            }
            continue;
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            if (cond.op == OP_IN) {
                for (auto & rhs_val : cond.rhs_vals) {
                    rhs_type = rhs_val.type;
                    val_raw_init(rhs_val, rhs_type, lhs_col->len);
                }
                continue;
            } else {
                rhs_type = cond.rhs_val.type;
                val_raw_init(cond.rhs_val, rhs_type, lhs_col->len);
            }
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            check_type(lhs_type, rhs_type);
        }
    }
}

void Analyze::check_group(const std::vector<TabCol>& group_cols,
                          const std::vector<std::string>& tab_names) {
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // 检查group by中选择的列是否存在。
    for (auto& group_col : group_cols) {
        bool found = false;
        for (auto& col : all_cols) {
            if (col.name == group_col.col_name &&
                col.tab_name == group_col.tab_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw RMDBError("Group by column not found");
        }
    }
}

void Analyze::check_conds_with_aggregate(const std::vector<Condition>& conds) {
    for (auto& cond : conds) {
        if (cond.lhs_col.agg_type != ast::Aggregatetype::SV_TYPE_NONE ||
            (!cond.is_rhs_val && cond.rhs_col.agg_type != ast::Aggregatetype::SV_TYPE_NONE && cond.op != OP_BETWEEN_AND)) {
            throw RMDBError("Aggregate column in where clause");
        }
    }
}

void Analyze::check_having_conds(const std::vector<Condition>& having_conds,
                                 const std::vector<TabCol>& group_cols) {
    for (auto& cond : having_conds) {
        if (cond.lhs_col.agg_type == ast::Aggregatetype::SV_TYPE_NONE) {
            // 如果左边不是聚合函数，就需要判断左边选择的列是否是group by中的。
            bool found = std::any_of(
                group_cols.begin(), group_cols.end(),
                [&](const TabCol& group_col) {
                    if (cond.lhs_col.col_name == group_col.col_name &&
                        cond.lhs_col.tab_name == group_col.tab_name) {
                        return true;
                    }
                    return false;
                });
            if (!found) {
                throw RMDBError("Non aggregate column not in group by");
            }
        }
        // 如果右边为一个列，必须是group by中的一个子列。
        if (!cond.is_rhs_val &&
            cond.rhs_col.agg_type == ast::Aggregatetype::SV_TYPE_NONE) {
            bool found = std::any_of(
                group_cols.begin(), group_cols.end(),
                [&](const TabCol& group_col) {
                    if (cond.rhs_col.col_name == group_col.col_name &&
                        cond.rhs_col.tab_name == group_col.tab_name) {
                        return true;
                    }
                    return false;
                });
            if (!found) {
                throw RMDBError("Non aggregate column not in group by");
            }
        }
    }
}

void Analyze::check_col_group_and_aggr(const std::vector<TabCol>& cols,
                                       const std::vector<TabCol>& group_cols) {
    if (group_cols.empty()) {
        // 如果不存在group by，那么聚合函数的列就不可以和普通的列一起使用。
        bool has_aggr =
            std::any_of(cols.begin(), cols.end(), [](const TabCol& col) {
                return col.agg_type != ast::Aggregatetype::SV_TYPE_NONE;
            });
        bool has_non_aggr =
            std::any_of(cols.begin(), cols.end(), [](const TabCol& col) {
                return col.agg_type == ast::Aggregatetype::SV_TYPE_NONE;
            });
        if (has_aggr && has_non_aggr) {
            throw RMDBError(
                "Non aggregate column in select list with aggregate column");
        }
    } else {
        // 如果存在group by，那么选择的普通列必须是group by选择的子列。
        for (auto& col : cols) {
            if (col.agg_type != ast::Aggregatetype::SV_TYPE_NONE) {
                continue;
            }
            bool found =
                std::any_of(group_cols.begin(), group_cols.end(),
                            [&](const TabCol& group_col) {
                                if (col.col_name == group_col.col_name) {
                                    return true;
                                }
                                return false;
                            });
            if (!found) {
                throw RMDBError("Non aggregate column not in group by");
            }
        }
    }
}

void Analyze::check_without_group(const std::vector<TabCol>& group_cols,
                                  const std::vector<Condition>& having_conds) {
    // 如果没有group by只存在having条件，直接报错。
    if (group_cols.empty() && !having_conds.empty()) {
        throw RMDBError("Having clause without group by");
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else if (auto bigint_lit = std::dynamic_pointer_cast<ast::BigIntLit>(sv_val)) {
        val.set_bigint(bigint_lit->val);
    } else if (auto datetime_lit = std::dynamic_pointer_cast<ast::DateTimeLit>(sv_val)) {
        val.set_datetime(datetime_lit->val);
    }else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
        {ast::SV_OP_IN, OP_IN}, {ast::SV_OP_BETWEEN_AND, OP_BETWEEN_AND}, 
    };
    return m.at(op);
}
