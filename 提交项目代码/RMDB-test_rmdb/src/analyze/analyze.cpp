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

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */

// 通过词法分析和语法分析生成的抽象语法树ast，再利用do_analyze进行语义分析和重写，生成新的抽象语法树放入Query。
// 词法分析和语法分析只是判断语句的语法是否存在错误，至于里面的逻辑和语义是否正确我们需要通过analyze进行判断。

std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        // 如果判断出是选择语句，我们就进行分析操作。
        // 处理表名
        query->tables = std::move(x->tabs); // 将语法树中所有的表赋值到query里面去。
        /** TODO: 检查表是否存在 */
        for (auto &table: query->tables) { // 判断这些表是否真的在数据库中存在。
            bool is_table = sm_manager_->db_.is_table(table);
            if (!is_table) { // 如果不存在需要抛出错误。
                throw TableNotFoundError(table);
            }
        }
        // 处理target list，再target list中添加上表名，例如 a.id
        for (auto &sv_sel_col : x->cols) { // 遍历原先树上的cols，给TabCol进行赋值（添加表和表名）
            TabCol sel_col = {.tab_name = sv_sel_col->tab_name, .col_name = sv_sel_col->col_name};
            query->cols.push_back(sel_col);
        }
        
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols); // 得到所有表中所有的字段元数据，存入all_cols中。
        // 如果用户选择*，此时query->cols就为空，这个时候我们就需要打印出所有的列。否则就打印query->cols.
        if (query->cols.empty()) {
            // select all columns
            // 这里是选择所有的列都加入投影列，所以我们没有必要进行列元素校验。
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        } else { 
            // 否则需要检查一下这些字段元数据是否合法，并且进行自动补全。
            // 具体功能和作用可以去看看check_column语句的解释。

            // infer table name from column name
            for (auto &sel_col : query->cols) {
                sel_col = check_column(all_cols, sel_col);  // 列元数据校验
            }
        }

        // get_clause是将语法树中的codes赋值到query的conds中去。
        // check_clause是分析query中where语句是否合法，比如判断where语句中操作符两边的类型是否一致等功能。

        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        /** TODO: */
        // 首先给updata的参数全部记录下来：表名、set条件的处理。
        const std::string& tab_name = x->tab_name;
        const std::vector<std::shared_ptr<ast::SetClause>>& sv_conds = x->set_clauses;
        // 初始化query中的conds（清空query中旧的数据，将语法树的SetClause转移过来）。
        std::vector<SetClause>& conds = query->set_clauses;
        conds.clear();

        for (auto& expr : sv_conds) {
            SetClause cond;
            // set语句中操作符左边一定是列，右边一定是值，进行初始化赋值操作后加入query的SetClause中去。
            cond.lhs = {.tab_name = tab_name, .col_name = expr->col_name};
            cond.rhs = convert_sv_value(expr->val); // 将具体的值转化为Value类型再进行赋值。
            conds.push_back(cond);
        }
        
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        // 删除就和where进行匹配就可以了，没有涉及到其他操作。
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);        
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        // 插入需要将Value放入到query的Values中去，将值转化为Value后加入即可。
        // 处理insert 的values值
        for (auto &sv_val : x->vals) {
            query->values.push_back(convert_sv_value(sv_val));
        }
    } else {
        // do nothing
    }
    // 最后进行语法树的转移操作。
    query->parse = std::move(parse);
    return query;
}


TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    // 如果这张表中的列名没有给出，我们就需要进行查找。我们以select name from emp1,emp2为例。
    // 这个语句是合法的，但是我们需要判断name是属于哪张表格的（并且不可以存在重复）。
    // 如果emp1中存在name,我们就会更新target中的tab_name, 补全当前字段是属于哪张表格的。
    // 如果表名不为空，我们就只需要在特定的表格中进行查找（并且不可以存在重复）。
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name; // 找到是否只存在一个名字
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        // 更新target中的name，表示表对应的字段列。
        target.tab_name = tab_name;
    } else { // 判断这个列是否存在，在all_cols中逐一查找，并且需要保证不可以存在重复。
        /** TODO: Make sure target column exists */
        int count = 0;
        for (auto& col : all_cols) {  // 查找表格中是否存在当前列以及是否存在重复。
            if (col.name == target.col_name &&
                col.tab_name == target.tab_name) {
                count++;
                if (count > 1) {
                    throw AmbiguousColumnError(target.col_name);
                }
            }
        }
        if (count == 0) {  // 如果未能找到
            throw ColumnNotFoundError(target.col_name);
        }
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    // 找到所有表格中的所有字段，全部放入到all_cols中去。
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    // 先清空conds中的数据，准备将语法树中的sv_conds转移过去。
    conds.clear();
    for (auto &expr : sv_conds) {
        // 操作符左边的一定是列，对列进行赋值操作就可以实现了。
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        // 对于右边的类型我们需要单独进行判断，如果是Value就进行赋值，否则就对列进行赋值。
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true; // 如果is_rhs_val为true就表示是一个值，否则就表示为一个列。
            cond.rhs_val = convert_sv_value(rhs_val); // 对Value进行赋值。
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    // 检查conds中每一个Condition是否合法（是否在all_cols里面存在并且类型匹配）。
    // 我们以select * from emp where id = 2 and age = 2为例来进行讲解。
    // 对于id和age，我们需要找到他是emp中的一个字段并且进行补全(emp.id), 在检查id是否为int类型。
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols); // 得到所有的字段元数据。
    // Get raw values in where clause
    for (auto &cond : conds) { // 枚举所有的Condition进行判断，是否在all_clos存在以及操作符两边的类型是否匹配。
        // Infer table name from column name
        // 如果左边或者右边存在列，检查是否在all_clos中存在并且进行补全。
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) { // 如果为false表示右边的选择为列。
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        // 找到左边选择的类型。
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        // 找到右边选择的类型。
        ColType rhs_type;
        if (cond.is_rhs_val) { // 为true表示右边为一个Value。
            cond.rhs_val.init_raw(lhs_col->len);
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
        }
        if (lhs_type != rhs_type) {
            if (lhs_type == TYPE_STRING || rhs_type == TYPE_STRING) {
                throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
            }
        }
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val) {
    // 将语法树中的sv_value转化为query中的Value。
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        val.set_int(int_lit->val);
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        val.set_str(str_lit->val);
    } else {
        throw InternalError("Unexpected sv value type");
    }
    return val;
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    // 将语法树中的操作符全部转化为query中op对应的操作符。
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}
