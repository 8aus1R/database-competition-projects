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
#include "defs.h"
#include "record/rm_defs.h"
#include "parser/ast.h"


struct TabCol {
    std::string tab_name;
    std::string col_name;
    std::string as_name;
    ast::Aggregatetype agg_type;
    friend bool operator<(const TabCol &x, const TabCol &y) {
        return std::make_pair(x.tab_name, x.col_name) < std::make_pair(y.tab_name, y.col_name);
    }
    bool operator==(const TabCol &y) const {
        return tab_name == y.tab_name && col_name == y.col_name;
    }
};

struct Value {
    ColType type;  // type of value
    union {
        int int_val;      // int value
        float float_val;  // float value
    };
    BigInt bigint_val;
    std::string str_val;  // string value
    DateTime datetime_val;
    std::shared_ptr<RmRecord> raw;  // raw record buffer

    void set_int(int int_val_) {
        type = TYPE_INT;
        int_val = int_val_;
    }

    void set_float(float float_val_) {
        type = TYPE_FLOAT;
        float_val = float_val_;
    }

    void set_str(std::string str_val_) {
        type = TYPE_STRING;
        str_val = std::move(str_val_);
    }

    void set_bigint(BigInt bigint_val_) {
        type = TYPE_BIGINT;
        bigint_val = bigint_val_;
    }
    void set_datetime(DateTime datetime_val_) {
        type = TYPE_DATETIME;
        datetime_val = datetime_val_;
    }

    // init_raw()存在构造函数，一个是存在参数的，一个是不存在参数的。
    // 两者的区别就是再string这里会存在一点影响，如果是字符串比较，我们就需要控制他们大小一致。
    // 所以对于字符串的处理我们就需要注意了,是否会存在字符串列的大小比较,如果存在就必须带参数.否则就没啥影响.
    void init_raw(int len) {
        assert(raw == nullptr);
        raw = std::make_shared<RmRecord>(len);
        if (type == TYPE_INT) {
            assert(len == sizeof(int));
            *(int *)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            assert(len == sizeof(float));
            *(float *)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            if (len < (int)str_val.size()) {
                throw StringOverflowError();
            }
            memset(raw->data, 0, len);
            memcpy(raw->data, str_val.c_str(), str_val.size());
        } else if (type == TYPE_BIGINT){
            //8byte
            assert(len == sizeof(BigInt));
            memset(raw->data, 0, len);
            memcpy(raw->data, &bigint_val, len);
        }else if (type == TYPE_DATETIME){
            //8byte
            assert(len == sizeof(DateTime));
            memset(raw->data, 0, len);
            memcpy(raw->data, &datetime_val, len);
        }
    }

    void init_raw() {
        assert(raw == nullptr);
        if (type == TYPE_INT) {
            raw = std::make_shared<RmRecord>(sizeof(int));
            *(int*)(raw->data) = int_val;
        } else if (type == TYPE_FLOAT) {
            raw = std::make_shared<RmRecord>(sizeof(float));
            *(float*)(raw->data) = float_val;
        } else if (type == TYPE_STRING) {
            raw = std::make_shared<RmRecord>(str_val.size());
            memcpy(raw->data, str_val.c_str(), str_val.size());
        } else if (type == TYPE_BIGINT){
            //8byte
            raw = std::make_shared<RmRecord>(sizeof(BigInt));
            memcpy(raw->data, &bigint_val, sizeof(BigInt));
        }else if (type == TYPE_DATETIME){
            //8byte
            raw = std::make_shared<RmRecord>(sizeof(DateTime));
            memcpy(raw->data, &datetime_val, sizeof(DateTime));
        }
    }
};

enum CompOp { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_IN, OP_BETWEEN_AND};

struct Condition {
    TabCol lhs_col;   // left-hand side column
    CompOp op;        // comparison operator
    bool is_rhs_val;  // true if right-hand side is a value (not a column)
    TabCol rhs_col;   // right-hand side column
    Value rhs_val;    // right-hand side value
    std::vector<Value> rhs_vals;
    JoinType join_type;
    std::pair<Value, Value> rhs_between_val; 
};

struct SetClause {
    TabCol lhs;
    Value rhs;
};

struct jointree {
    std::string left;
    std::string right;
    std::vector<Condition> conds;
    JoinType join_type;
};