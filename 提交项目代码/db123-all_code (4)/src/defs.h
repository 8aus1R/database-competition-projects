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

#include <iostream>
#include <map>
#include <string.h>

// 此处重载了<<操作符，在ColMeta中进行了调用
template<typename T, typename = typename std::enable_if<std::is_enum<T>::value, T>::type>
std::ostream &operator<<(std::ostream &os, const T &enum_val) {
    os << static_cast<int>(enum_val);
    return os;
}

template<typename T, typename = typename std::enable_if<std::is_enum<T>::value, T>::type>
std::istream &operator>>(std::istream &is, T &enum_val) {
    int int_val;
    is >> int_val;
    enum_val = static_cast<T>(int_val);
    return is;
}

// 对记录进行封装，需要知道页号和槽位号，就可以确定当前记录的位置。
struct Rid {
    int page_no;
    int slot_no;

    friend bool operator==(const Rid &x, const Rid &y) {
        return x.page_no == y.page_no && x.slot_no == y.slot_no;
    }

    friend bool operator!=(const Rid &x, const Rid &y) { return !(x == y); }
};

// 定义的类型，如果又存在了一个新的类型我们就继续操作。
enum ColType {
    TYPE_INT, TYPE_FLOAT, TYPE_STRING, TYPE_BIGINT, TYPE_DATETIME
};

// 识别类型，方便我们进行报错。
inline std::string coltype2str(ColType type) {
    std::map<ColType, std::string> m = {
            {TYPE_INT,    "INT"},
            {TYPE_FLOAT,  "FLOAT"},
            {TYPE_STRING, "STRING"},
            {TYPE_BIGINT, "LONG LONG"}, 
            {TYPE_DATETIME, "STRING"}
    };
    return m.at(type);
}

// 对扫描算子进行多态实现，扫描算子的实现可以存在多种，顺序扫描和索引扫描，如果还存在其他的就继续添加，整理框架不需要改变。
class RecScan {
public:
    virtual ~RecScan() = default;

    virtual void next() = 0;

    virtual bool is_end() const = 0;

    virtual Rid rid() const = 0;
};

// 聚合函数的6中操作，如果还存在就继续添加。
enum class AggregateType {
    NONE,
    COUNT,
    SUM,
    MAX,
    MIN,
    AVG
};

// 封装对bigint的赋值和比较，对于判断是否合法在lex分析语法中进行，可以明确的区分当前类型是否合法。
class BigInt {
public:
    long long value;
    bool flag = 0;      // 判断数值范围是否合法
    BigInt() {
        value = 0;
    }
    //由char* 生成BigInt构造函数
    BigInt(char* bit) {
        value = std::atoll(bit);
    }
    //操作符重载
    BigInt(int val) {
        value = val;
    }

    //转化为string
    std::string tostring() {
        return std::to_string(value);
    }
    
    bool operator>(const BigInt& other) const {
        return value > other.value;
    }
    bool operator<(const BigInt& other) const {
        return value < other.value;
    }
    bool operator==(const BigInt& other) const {
        return value == other.value;
    }
};

// 封装对datetime的赋值、判断、比较操作，由于我们一下子区分不出日期是否合法，所以我们可以先存储起来，然后自己进行判断。
class DateTime {
public:
    char* value;
    bool flag = true;      // 判断数值范围是否合法

    DateTime() {
        value = nullptr;
    }
    // 由char* 生成DateTime构造函数
    DateTime(char* bit) {
        value = bit;
    }
    
    DateTime(std::string bit) {
        value = strcpy((char*)malloc(bit.length()+1), bit.c_str());
    }

    //转化为string
    std::string get_datetime() {
        return value;
    }

    // 判断是否合法
    bool isLegal(){
        /* 最小值为'1000-01-01 00:00:00'，最大值为'9999-12-31 23:59:59' */
        std::string tmp(value);

        int p1 = tmp.find('-');       // 第一个‘-’的位置
        int p2 = tmp.find('-', p1 + 1);   // 第二个‘-’的位置
        int p3 = tmp.find(' ', p2);   // ' '的位置
        int p4 = tmp.find(':', p3);   // 第一个‘:’的位置
        int p5 = tmp.find(':', p4 + 1);   // 第二个‘:’的位置

        // 判断年份是否合法
        int year = std::stoi(tmp.substr(0, p1));
        if (year < 1000 || year > 9999 || p1 != 4){ return false; }

        // 判断月份是否合法
        int month = std::stoi(tmp.substr(p1 + 1, p2));
        if (month < 1 || month > 12 || p2 != 7){ return false; }

        // 判断日期是否合法
        int day = std::stoi(tmp.substr(p2 + 1, p3));
        if (p3 != 10){ return false; }
        if (month == 4 || month == 6 || month == 9 || month == 11){
            if (day < 1 || day > 30){ return false; }
        }
        if (month == 1 || month == 3 || month == 5 || month == 7 || month == 8 || month == 10 || month == 12){
            if (day < 1 || day > 31){ return false; }
        }
            // 平年闰年的2月
        bool isLeapYear = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
        if (isLeapYear && month == 2){
            if (day < 1 || day > 29){ return false; }
        }
        if (!isLeapYear && month == 2){
            if (day < 1 || day > 28){ return false; }
        }

        // 判断小时是否合法
        int hour = std::stoi(tmp.substr(p3 + 1, p4));
        if (hour < 0 || hour > 23 || p4 != 13){ return false;}

        // 判断分钟是否合法
        int minute = std::stoi(tmp.substr(p4 + 1, p5));
        if (minute < 0 || minute > 59 || p5 != 16){ return false;}

        // 判断秒是否合法
        int end = tmp.size();
        int second = std::stoi(tmp.substr(p5 + 1, end));
        if (second < 0 || second > 59 || end != 19){ return false;}

        return true;
    }

    bool operator>(const DateTime& other) const {
        std::string this_value(value);
        std::string that_value(other.value);
        return this_value > that_value;
    }
    bool operator<(const DateTime& other) const {
        std::string this_value(value);
        std::string that_value(other.value);
        return this_value < that_value;
    }
    bool operator==(const DateTime& other) const {
        std::string this_value(value);
        std::string that_value(other.value);
        return this_value == that_value;
    }
};

