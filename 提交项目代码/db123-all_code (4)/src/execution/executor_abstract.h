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
#include "common/common.h"
#include "index/ix.h"
#include "system/sm.h"

// 封装了condition的比较，所有类型的比较操作都在这里面，如果需要比较直接调用就可以了。
class ConditionEvaluator {
public:
    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }
    // 判断当前记录是否满足当前的condition条件。
    bool eval_cond(Condition& condition, std::vector<ColMeta>& cols,
                   RmRecord& record) {
        Value invalid = Value{};
        Value& lhsValue =
            getOperandValue(condition.lhs_col, cols, record, invalid);
        Value& rhsValue =
            getOperandValue(condition.rhs_col, cols, record, condition.rhs_val);

        
        if (lhsValue.type != rhsValue.type) {
            if (lhsValue.type == TYPE_BIGINT && rhsValue.type == TYPE_INT) {
                BigInt bigint(rhsValue.int_val);
                rhsValue.set_bigint(bigint);
            } else if (lhsValue.type == TYPE_INT &&
                       rhsValue.type == TYPE_BIGINT) {
                BigInt bigint(lhsValue.int_val);
                lhsValue.set_bigint(bigint);
            } else if (lhsValue.type == TYPE_STRING &&
                       rhsValue.type == TYPE_DATETIME) {
                std::string str = rhsValue.datetime_val.get_datetime();
                rhsValue.set_str(str);
            }
        }
        if (lhsValue.type == TYPE_STRING) {
            while (rhsValue.str_val.size() < lhsValue.str_val.size()) {
                rhsValue.str_val += char(0);
            }
        }
        switch (condition.op) {
            case OP_EQ:
                return isEqual(lhsValue, rhsValue);
            case OP_NE:
                return !isEqual(lhsValue, rhsValue);
            case OP_LT:
                return isLessThan(lhsValue, rhsValue);
            case OP_GT:
                return isGreaterThan(lhsValue, rhsValue);
            case OP_LE:
                return isLessThanOrEqual(lhsValue, rhsValue);
            case OP_GE:
                return isGreaterThanOrEqual(lhsValue, rhsValue);
            case OP_IN:
                return isIN(lhsValue, condition.rhs_vals);
            case OP_BETWEEN_AND:
                return isBetWeenAnd(lhsValue, condition.rhs_between_val.first, condition.rhs_between_val.second);
            default:
                throw std::string("Invalid comparison operator");
        }
    }
    // 判断左右两条记录是否满足condition条件。
    bool eval_cond(Condition& condition, std::vector<ColMeta>& cols,
                   RmRecord& record_l, RmRecord& record_r) {
        Value invalid = Value{};
        Value& lhsValue =
            getOperandValue(condition.lhs_col, cols, record_l, invalid);
        Value& rhsValue = getOperandValue(condition.rhs_col, cols, record_r,
                                          condition.rhs_val);
        // rz-dev
        if (lhsValue.type != rhsValue.type) {
            if (lhsValue.type == TYPE_BIGINT && rhsValue.type == TYPE_INT) {
                BigInt bigint(rhsValue.int_val);
                rhsValue.set_bigint(bigint);
            } else if (lhsValue.type == TYPE_INT &&
                       rhsValue.type == TYPE_BIGINT) {
                BigInt bigint(rhsValue.int_val);
                rhsValue.set_bigint(bigint);
            }
        }
        if (lhsValue.type == TYPE_STRING) {
            while (rhsValue.str_val.size() < lhsValue.str_val.size()) {
                rhsValue.str_val += char(0);
            }
        }
        switch (condition.op) {
            case OP_EQ:
                return isEqual(lhsValue, rhsValue);
            case OP_NE:
                return !isEqual(lhsValue, rhsValue);
            case OP_LT:
                return isLessThan(lhsValue, rhsValue);
            case OP_GT:
                return isGreaterThan(lhsValue, rhsValue);
            case OP_LE:
                return isLessThanOrEqual(lhsValue, rhsValue);
            case OP_GE:
                return isGreaterThanOrEqual(lhsValue, rhsValue);
            case OP_IN:
                return isIN(lhsValue, condition.rhs_vals);
            case OP_BETWEEN_AND:
                return isBetWeenAnd(lhsValue, condition.rhs_between_val.first, condition.rhs_between_val.second);
            default:
                throw std::string("Invalid comparison operator");
            }
    }
    // 判断当前记录是否满足所有的conditions条件。
    bool eval_conds(std::vector<Condition>& conditions,
                    std::vector<ColMeta>& cols, RmRecord& record) {
        return std::all_of(conditions.begin(), conditions.end(),
                           [&](Condition& condition) {
                               return eval_cond(condition, cols, record);
                           });
    }
    // 判断两个值是否满足当前操作符。
    bool check_cond(Value lhsValue, Value rhsValue, CompOp op) {
        // 如果是合法类型但是又不一致，说明是存在大数和小数的比较了。
        // 浮点数我们不进行转化，后面我们进行特判，int和bigint我们转化为bigint。
        if (lhsValue.type != rhsValue.type) {
            if (lhsValue.type == TYPE_BIGINT && rhsValue.type == TYPE_INT) {
                BigInt bigint(rhsValue.int_val);
                rhsValue.set_bigint(bigint);
            } else if (lhsValue.type == TYPE_INT &&
                       rhsValue.type == TYPE_BIGINT) {
                BigInt bigint(rhsValue.bigint_val);
                lhsValue.set_bigint(bigint);
            }
        }
        // 这里需要补齐末尾0，由于字符串的长度不唯一，自己输入的长度并不会那么大，所以多出的需要自己补齐。
        if (lhsValue.type == TYPE_STRING) {
            while (rhsValue.str_val.size() < lhsValue.str_val.size()) {
                rhsValue.str_val += char(0);
            }
        }
        switch (op) {
            case OP_EQ:
                return isEqual(lhsValue, rhsValue);
            case OP_NE:
                return !isEqual(lhsValue, rhsValue);
            case OP_LT:
                return isLessThan(lhsValue, rhsValue);
            case OP_GT:
                return isGreaterThan(lhsValue, rhsValue);
            case OP_LE:
                return isLessThanOrEqual(lhsValue, rhsValue);
            case OP_GE:
                return isGreaterThanOrEqual(lhsValue, rhsValue);
            default:
                throw std::string("Invalid comparison operator");
        }
    }
    // 给出当前列属性，记录和值，如果本来就是一个值就不需要操作，否则就转化为Value。
    // 找到当前的列属性，然后转化为相应的类型，就可以求解出来了。
    Value& getOperandValue(TabCol& col, std::vector<ColMeta>& cols,
                           RmRecord& record, Value& value) {

        if (col.tab_name.empty() && col.col_name.empty()) {
            // 使用常量值
            return value;
        } else {
            // 使用列值
            value = Value{};
            auto meta = *get_col(cols, col);
            auto type = meta.type;
            if (type == TYPE_INT) {
                char* charPointer1 =
                    reinterpret_cast<char*>(record.data + meta.offset);
                int int_val = *reinterpret_cast<int*>(charPointer1);
                value.set_int(int_val);
                value.init_raw(meta.len);
                return value;
            } else if (type == TYPE_FLOAT) {
                char* charPointer2 =
                    reinterpret_cast<char*>(record.data + meta.offset);
                float float_val =
                    *reinterpret_cast<float*>(charPointer2);
                value.set_float(float_val);
                value.init_raw(meta.len);
                return value;
            } else if (type == TYPE_STRING) {
                char* charPointer3 =
                    reinterpret_cast<char*>(record.data + meta.offset);
                std::string str(charPointer3, charPointer3 + meta.len);
                value.set_str(str);
                value.init_raw(meta.len);
                return value;
            } else if (type == TYPE_BIGINT) {
                char* charPointer4 =
                    reinterpret_cast<char*>(record.data + meta.offset);
                BigInt bigint_val =
                    *reinterpret_cast<BigInt*>(charPointer4);
                value.set_bigint(bigint_val);
                value.init_raw(meta.len);
                return value;
            } else if (type == TYPE_DATETIME) {
                char* charPointer5 =
                    reinterpret_cast<char*>(record.data + meta.offset);
                DateTime datetime_val =
                    *reinterpret_cast<DateTime*>(charPointer5);
                value.set_datetime(datetime_val);
                value.init_raw(meta.len);
                return value;
            }
            throw std::string("Invalid column reference");
        }
    }
    // 对聚合函数的having进行预处理，转化为val然后进行比较(这里只支持int,float,和string)
    // 如果左边不存在聚合函数，那就正常进行，否则就需要分类讨论，每一种聚合函数单独判断和操作。
    Value get_aggr_value(
        const std::vector<ColMeta>& rec_cols,
        const std::vector<std::unique_ptr<RmRecord>>& rec,
        const TabCol& tab_col) {
        Value val;
        ColMeta col_meta;
        if (tab_col.agg_type == ast::Aggregatetype::SV_TYPE_COUNT && tab_col.col_name == "*") {
            col_meta = ColMeta{.tab_name = "",
                               .name = "*",
                               .type = TYPE_INT,
                               .len = sizeof(int),
                               .offset = 0};
        } else {
            col_meta = *get_col(rec_cols, tab_col);
        }
        // 如果不是聚合，就正常赋值，否则是count就是次数，是sum就是求和等操作。
        if (tab_col.agg_type == ast::Aggregatetype::SV_TYPE_NONE) {
            if (col_meta.type == TYPE_INT) {
                val.set_int(*(int*)(rec[0]->data + col_meta.offset));
            } else if (col_meta.type == TYPE_FLOAT) {
                val.set_float(
                    *(float*)(rec[0]->data + col_meta.offset));
            } else {
                val.set_str(std::string(rec[0]->data + col_meta.offset,
                                        col_meta.len));
            }
        } else if (tab_col.agg_type == ast::Aggregatetype::SV_TYPE_COUNT) {
            val.set_int(rec.size());
        } else if (tab_col.agg_type == ast::Aggregatetype::SV_TYPE_SUM) {
            if (col_meta.type == TYPE_INT) {
                int sum = 0;
                for (const auto& record : rec) {
                    sum += *(int*)(record->data + col_meta.offset);
                }
                val.set_int(sum);
            } else if (col_meta.type == TYPE_FLOAT) {
                float sum = 0;
                for (const auto& record : rec) {
                    sum += *(float*)(record->data + col_meta.offset);
                }
                val.set_float(sum);
            }
        } else if (tab_col.agg_type == ast::Aggregatetype::SV_TYPE_MAX) {
            if (col_meta.type == TYPE_INT) {
                int max = std::numeric_limits<int>::min();
                for (const auto& record : rec) {
                    max =
                        std::max(max, *(int*)(record->data + col_meta.offset));
                }
                val.set_int(max);
            } else if (col_meta.type == TYPE_FLOAT) {
                float max = std::numeric_limits<float>::min();
                for (const auto& record : rec) {
                    max = std::max(max,
                                   *(float*)(record->data + col_meta.offset));
                }
                val.set_float(max);
            } else if (col_meta.type == TYPE_STRING) {
                std::string max = "";
                for (const auto& record : rec) {
                    std::string str(record->data + col_meta.offset,
                                    col_meta.len);
                    max = std::max(max, str);
                }
                val.set_str(max);
            }
        } else if (tab_col.agg_type == ast::Aggregatetype::SV_TYPE_MIN) {
            if (col_meta.type == TYPE_INT) {
                int min = std::numeric_limits<int>::max();
                for (const auto& record : rec) {
                    min =
                        std::min(min, *(int*)(record->data + col_meta.offset));
                }
                val.set_int(min);
            } else if (col_meta.type == TYPE_FLOAT) {
                float min = std::numeric_limits<float>::max();
                for (const auto& record : rec) {
                    min = std::min(min,
                                   *(float*)(record->data + col_meta.offset));
                }
                val.set_float(min);
            } else if (col_meta.type == TYPE_STRING) {
                std::string min = std::string(255, 255);
                for (const auto& record : rec) {
                    std::string str(record->data + col_meta.offset,
                                    col_meta.len);
                    min = std::min(min, str);
                }
                val.set_str(min);
            }
        }else if (tab_col.agg_type == ast::Aggregatetype::SV_TYPE_AVG) {
            if (col_meta.type == TYPE_INT) {
                int sum = 0, count = 0;
                for (const auto& record : rec) {
                    sum += *(int*)(record->data + col_meta.offset);
                    count++;
                }
                val.set_float((float)sum / count);
            } else if (col_meta.type == TYPE_FLOAT) {
                float sum = 0;
                for (const auto& record : rec) {
                    sum += *(float*)(record->data + col_meta.offset);
                }
                val.set_float((float)sum / rec.size());
            }
        }
        return val;
    }

    static bool isEqual(const Value& lhs, const Value& rhs) {
        if ((lhs.type == TYPE_INT) && (rhs.type == TYPE_FLOAT)) {
            return lhs.int_val == rhs.float_val;
        } else if ((lhs.type == TYPE_FLOAT) && (rhs.type == TYPE_INT)) {
            return lhs.float_val == rhs.int_val;
        } else if ((lhs.type == TYPE_INT) && (rhs.type == TYPE_INT)) {
            return lhs.int_val == rhs.int_val;
        } else if ((lhs.type == TYPE_FLOAT) && (rhs.type == TYPE_FLOAT)) {
            return lhs.float_val == rhs.float_val;
        } else if ((lhs.type == TYPE_STRING) && (rhs.type == TYPE_STRING)) {
            return lhs.str_val == rhs.str_val;
        } else if ((lhs.type == TYPE_BIGINT) && (rhs.type == TYPE_BIGINT)) {
            return lhs.bigint_val == rhs.bigint_val;
        } else if ((lhs.type == TYPE_DATETIME) && (rhs.type == TYPE_DATETIME)) {
            return lhs.datetime_val == rhs.datetime_val;
        } else {
            throw std::string("Invalid value type");
        }
    }

    static bool isLessThan(const Value& lhs, const Value& rhs) {
        if ((lhs.type == TYPE_INT) && (rhs.type == TYPE_FLOAT)) {
            return lhs.int_val < rhs.float_val;
        } else if ((lhs.type == TYPE_FLOAT) && (rhs.type == TYPE_INT)) {
            return lhs.float_val < rhs.int_val;
        } else if ((lhs.type == TYPE_INT) && (rhs.type == TYPE_INT)) {
            return lhs.int_val < rhs.int_val;
        } else if ((lhs.type == TYPE_FLOAT) && (rhs.type == TYPE_FLOAT)) {
            return lhs.float_val < rhs.float_val;
        } else if ((lhs.type == TYPE_STRING) && (rhs.type == TYPE_STRING)) {
            return lhs.str_val < rhs.str_val;
        } else if ((lhs.type == TYPE_BIGINT) && (rhs.type == TYPE_BIGINT)) {
            return lhs.bigint_val < rhs.bigint_val;
        } else if ((lhs.type == TYPE_DATETIME) && (rhs.type == TYPE_DATETIME)) {
            return lhs.datetime_val < rhs.datetime_val;
        } else {
            throw std::string("Invalid value type");
        }
    }

    static bool isGreaterThan(const Value& lhs, const Value& rhs) {
        if ((lhs.type == TYPE_INT) && (rhs.type == TYPE_FLOAT)) {
            return lhs.int_val > rhs.float_val;
        } else if ((lhs.type == TYPE_FLOAT) && (rhs.type == TYPE_INT)) {
            return lhs.float_val > rhs.int_val;
        } else if ((lhs.type == TYPE_INT) && (rhs.type == TYPE_INT)) {
            return lhs.int_val > rhs.int_val;
        } else if ((lhs.type == TYPE_FLOAT) && (rhs.type == TYPE_FLOAT)) {
            return lhs.float_val > rhs.float_val;
        } else if ((lhs.type == TYPE_STRING) && (rhs.type == TYPE_STRING)) {
            return lhs.str_val > rhs.str_val;
        } else if ((lhs.type == TYPE_BIGINT) && (rhs.type == TYPE_BIGINT)) {
            return lhs.bigint_val > rhs.bigint_val;
        } else if ((lhs.type == TYPE_DATETIME) && (rhs.type == TYPE_DATETIME)) {
            return lhs.datetime_val > rhs.datetime_val;
        } else {
            throw std::string("Invalid value type");
        }
    }

    static bool isLessThanOrEqual(const Value& lhs, const Value& rhs) {
        return isEqual(lhs, rhs) || isLessThan(lhs, rhs);
    }

    static bool isGreaterThanOrEqual(const Value& lhs, const Value& rhs) {
        return isEqual(lhs, rhs) || isGreaterThan(lhs, rhs);
    }
    static bool isBetWeenAnd(const Value& lhs, const Value& rhs1, const Value & rhs2) {
        return isGreaterThanOrEqual(lhs, rhs1) && isLessThanOrEqual(lhs, rhs2);
    }
    static bool isIN(const Value& lhs, const std::vector<Value> rhs_vals) {
        for (auto & rhs : rhs_vals) {
            if (isEqual(lhs, rhs)) return true;
        }
        return false;
    }
};


class AbstractExecutor {
   public:
    Rid _abstract_rid;

    Context *context_;

    virtual ~AbstractExecutor() = default;

    virtual size_t tupleLen() const { return 0; };

    virtual const std::vector<ColMeta> &cols() const {
        std::vector<ColMeta> *_cols = nullptr;
        return *_cols;
    };

    virtual std::string getType() { return "AbstractExecutor"; };

    virtual void beginTuple(){};

    virtual void nextTuple(){};

    virtual bool is_end() const { return true; };

    virtual Rid &rid() = 0;

    virtual std::unique_ptr<RmRecord> Next() = 0;

    virtual ColMeta get_col_offset(const TabCol &target) { return ColMeta();};

    virtual bool is_aggregate() { return false; };

    std::vector<ColMeta>::const_iterator get_col(const std::vector<ColMeta> &rec_cols, const TabCol &target) {
        auto pos = std::find_if(rec_cols.begin(), rec_cols.end(), [&](const ColMeta &col) {
            return col.tab_name == target.tab_name && col.name == target.col_name;
        });
        if (pos == rec_cols.end()) {
            throw ColumnNotFoundError(target.tab_name + '.' + target.col_name);
        }
        return pos;
    }
    ConditionEvaluator Cal;
};