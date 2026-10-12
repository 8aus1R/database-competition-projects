/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sm_manager.h"

#include <sys/stat.h>
#include <unistd.h>

#include <fstream>

#include "index/ix.h"
#include "record/rm.h"
#include "record_printer.h"
//数据库为一个目录，里面有很多文件；一张表就对应一个文件
/**
 * @description: 判断是否为一个文件夹
 * @return {bool} 返回是否为一个文件夹
 * @param {string&} db_name 数据库文件名称，与文件夹同名
 */
bool SmManager::is_dir(const std::string& db_name) {
    struct stat st;
    return stat(db_name.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/**
 * @description: 创建数据库，所有的数据库相关文件都放在数据库同名文件夹下
 * @param {string&} db_name 数据库名称
 */
void SmManager::create_db(const std::string& db_name) {
    if (is_dir(db_name)) {
        throw DatabaseExistsError(db_name);
    }
    //为数据库创建一个子目录
    std::string cmd = "mkdir " + db_name;
    if (system(cmd.c_str()) < 0) {  // 创建一个名为db_name的目录//执行该命令
        throw UnixError();
    }
    if (chdir(db_name.c_str()) < 0) {  // 进入名为db_name的目录
        throw UnixError();
    }
    //创建系统目录
    DbMeta *new_db = new DbMeta();
    new_db->name_ = db_name;

    // 注意，此处ofstream会在当前目录创建(如果没有此文件先创建)和打开一个名为DB_META_NAME的文件
    std::ofstream ofs(DB_META_NAME);

    // 将new_db中的信息，按照定义好的operator<<操作符，写入到ofs打开的DB_META_NAME文件中
    ofs << *new_db;  // 注意：此处重载了操作符<<

    delete new_db;

    // 创建日志文件
    disk_manager_->create_file(LOG_FILE_NAME);

    // 回到根目录
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 删除数据库，同时需要清空相关文件以及数据库同名文件夹
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::drop_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    std::string cmd = "rm -r " + db_name;
    if (system(cmd.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开数据库，找到数据库对应的文件夹，并加载数据库元数据和相关文件
 * @param {string&} db_name 数据库名称，与文件夹同名
 */
void SmManager::open_db(const std::string& db_name) {
    if (!is_dir(db_name)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (chdir(db_name.c_str()) < 0) {//当前工作目录切换到数据库目录
        throw UnixError();
    }
    std::ifstream ofs(DB_META_NAME);//获取数据库的基本信息
    ofs >> db_;
    for (auto& [tab_name, tab_info] : db_.tabs_) {
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));//储存已打开每个表的表名和表文件
        for (const auto& index : tab_info.indexes) {
            std::string ix_name = ix_manager_->get_index_name(tab_name, index.cols);
            ihs_.emplace(ix_name, ix_manager_->open_index(tab_name, index.cols));
        }
    }
}

/**
 * @description: 把数据库相关的元数据刷入磁盘中
 */
void SmManager::flush_meta() {
    // 默认清空文件
    std::ofstream ofs(DB_META_NAME);
    ofs << db_;
}

/**
 * @description: 关闭数据库并把数据落盘
 */
void SmManager::close_db() {
    flush_meta();
    for (auto& fh : fhs_) {  // 关闭所有的文件句柄
        rm_manager_->close_file(fh.second.get());
    }
    for (auto & t : ihs_) {
        ix_manager_->close_index(t.second.get());
    }
    //清理内存中的数据库信息（不会影响磁盘中的数据）
    db_.name_.clear();
    db_.tabs_.clear();
    fhs_.clear();
    ihs_.clear();
    if (chdir("..") < 0) {
        throw UnixError();
    }
}

/**
 * @description: 显示所有的表,通过测试需要将其结果写入到output.txt,详情看题目文档
 * @param {Context*} context 
 */
void SmManager::show_tables(Context* context) {
    std::fstream outfile;
    //打开 output.txt 文件，以追加模式（std::ios::app）写入内容，确保不覆盖原有数据。
    outfile.open("output.txt", std::ios::out | std::ios::app);
    //向文件写入表头 "| Tables |"。
    //使用 RecordPrinter 类（用于格式化输出记录）创建一个包含 1 列的表格，打印分隔线和表头行 Tables。
    outfile << "| Tables |\n";
    RecordPrinter printer(1);
    printer.print_separator(context);
    printer.print_record({"Tables"}, context);
    printer.print_separator(context);
    /*效果
    +--------+
    | Tables |
    +--------+*/

    for (auto &entry : db_.tabs_) {//遍历并输出所有表名
        auto &tab = entry.second;
        printer.print_record({tab.name}, context);
        outfile << "| " << tab.name << " |\n";
    }
    /*效果
    | users  |
    | orders |*/

    printer.print_separator(context);//输出表尾分隔线
    /*
    +--------+ */
    outfile.close();//关闭输出文件
}

/**
 * @description: 显示表的元数据
 * @param {string&} tab_name 表名称
 * @param {Context*} context 
 */
void SmManager::desc_table(const std::string& tab_name, Context* context) {
    TabMeta &tab = db_.get_table(tab_name);//获取表的元数据

    //创建包含三列标题的向量：字段名、数据类型、是否为主键
    std::vector<std::string> captions = {"Field", "Type", "Index"};
    RecordPrinter printer(captions.size());//初始化RecordPrinter对象，指定列数为3

    // Print header打印表头
    printer.print_separator(context);
    printer.print_record(captions, context);
    printer.print_separator(context);


    // Print fields
    for (auto &col : tab.cols) {
        std::vector<std::string> field_info = {col.name, coltype2str(col.type), col.index ? "YES" : "NO"};
        printer.print_record(field_info, context);
    }
    /*对表的每一列：
    提取列名col.name。
    通过coltype2str函数将列的数据类型（如INT、VARCHAR）转换为字符串。
    根据col.index判断是否为主键，输出YES或NO。*/

    // Print footer打印表尾分割线
    printer.print_separator(context);
}

/**
 * @description: 创建表
 * @param {string&} tab_name 表的名称
 * @param {vector<ColDef>&} col_defs 表的字段
 * @param {Context*} context 
 */
void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {//判断有无这个表
        throw TableExistsError(tab_name);
    }
    // Create table meta
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    for (auto &col_def : col_defs) {
        ColMeta col = {.tab_name = tab_name,
                       .name = col_def.name,
                       .type = col_def.type,
                       .len = col_def.len,
                       .offset = curr_offset,
                       .index = false};
        curr_offset += col_def.len;
        tab.cols.push_back(col);
    }
    tab.row_count = 0;
    // Create & open record file
    int record_size = curr_offset;  // record_size就是col meta所占的大小（表的元数据也是以记录的形式进行存储的）
    rm_manager_->create_file(tab_name, record_size);
    db_.tabs_[tab_name] = tab;
    // fhs_[tab_name] = rm_manager_->open_file(tab_name);
    fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));//打开文件并缓存文件句柄

    flush_meta();
}

/**
 * @description: 删除表
 * @param {string&} tab_name 表的名称
 * @param {Context*} context
 */
void SmManager::drop_table(const std::string& tab_name, Context* context) {
    if (!db_.is_table(tab_name)) {  // 如果表不存在
        throw TableNotFoundError(tab_name);
    }
    TabMeta &tab = db_.get_table(tab_name);  // 获取表的元数据
    
    for (const auto& index : tab.indexes) {  // 删除表的索引
        drop_index(tab_name, index.cols, context);
    }
    // 先关闭再删除表文件
    rm_manager_->close_file(fhs_[tab_name].get());
    rm_manager_->destroy_file(tab_name);

    fhs_.erase(tab_name);
    db_.tabs_.erase(tab_name);

    flush_meta();
}

void SmManager::update_table_row_count(const std::string& tab_name, int row_count) {
    // 更新表中的行数，利于后面进行查询优化的时候计算两表连接的价值。
    if (!db_.is_table(tab_name)) return;
    db_.get_table(tab_name).row_count = row_count;
    flush_meta();
}

/**
 * @description: 创建索引
 * @param {string&} tab_name 表的名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::create_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    
    TabMeta& tab = db_.get_table(tab_name); // 得到表的原数据。
    std::vector<ColMeta> cols; // 列的元数据，创建索引需要列的元数据。
    int tot_len = 0; // 记录的长度。
    for (int i = 0; i < col_names.size(); i++) {
        auto col = *tab.get_col(col_names[i]);
        cols.push_back(col);
        tot_len += col.len;
    }
    // 向表中添加索引项。
    tab.indexes.push_back(IndexMeta{tab_name, tot_len, (int)col_names.size(), cols});
    // 创建索引，并且添加到ihs_中去。其中ihs_是索引名映射索引句柄，便于对索引进行操作。
    ix_manager_->create_index(tab_name, cols);
    auto && ix_name = ix_manager_->get_index_name(tab_name, cols);
    ihs_.emplace(ix_name, ix_manager_->open_index(tab_name, cols));
    // 得到文件句柄，索引句柄以及扫描记录。
    auto && rfh = fhs_[tab_name].get();
    auto && ih = ihs_[ix_name].get();
    auto && scan_ = std::make_unique<RmScan>(rfh);
    // 在建立所有之前以及插入的记录，需要保证都是唯一的，不可以存在重复的情况。
    // 如果出现了重复值，那么就需要将原先插入的所有数据都回退回去，并且抛出错误，表示不合法。
    std::vector<char *> prev;
    while (!scan_->is_end()) {
        auto rid_ = scan_->rid();
        auto rec = rfh->get_record(rid_, context);
        char* key = new char[tot_len];
        int offset = 0;
        // 将这条记录中的部分数据依次放入key中，便于后面进行插入和删除。
        for (auto& col : cols) {
            memcpy(key + offset, rec->data + col.offset, col.len);
            offset += col.len;
        }
        if (ih->insert_entry(key, rid_, context->txn_) == -1) {
            for (int i = 0; i < prev.size(); i++) {
                ih->delete_entry(prev[i], context->txn_);
            }
            drop_index(tab_name, cols, context);
            throw RMDBError("create index error");
        }
        prev.push_back(key);
        scan_->next();
    }
    // ix_manager_->close_index(ih);
    flush_meta();
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<string>&} col_names 索引包含的字段名称
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    
    // 存在性检查，判断是否存在这个索引。
    if (!ix_manager_->exists(tab_name, col_names)) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    // 删除索引
    TabMeta& tab = db_.get_table(tab_name);
    auto && index_name = ix_manager_->get_index_name(tab_name, col_names);
    ix_manager_->close_index(ihs_.at(index_name).get());
    ix_manager_->destroy_index(tab_name, col_names);

    // 从tab的indexs中删除对应列，然后再ihs中删除。
    auto &&ix_meta = tab.get_index_meta(col_names);
    tab.indexes.erase(ix_meta);
    ihs_.erase(index_name);
    flush_meta();
    return ;
}

/**
 * @description: 删除索引
 * @param {string&} tab_name 表名称
 * @param {vector<ColMeta>&} 索引包含的字段元数据
 * @param {Context*} context
 */
void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    // 重载函数，直接给列名取出来之后再调用前面的函数。
    std::vector<std::string> cols_name;
    for (int i = 0; i < cols.size(); i++) {
        cols_name.push_back(cols[i].name);
    }
    drop_index(tab_name, cols_name, context);
    flush_meta();
    return ;
}


void SmManager::show_index(std::string &tab_name, Context* context) {
    std::fstream outfile;
    //打开 output.txt 文件，以追加模式（std::ios::app）写入内容，确保不覆盖原有数据。
    outfile.open("output.txt", std::ios::out | std::ios::app);
    //使用 RecordPrinter 类（用于格式化输出记录）创建一个包含 3 列的表格，打印分隔线和表头行 Tables。
    RecordPrinter printer(3);
    // 输出表开头分隔线
    printer.print_separator(context);

    TabMeta& tab = db_.get_table(tab_name);
    // 这里要注意格式呢，看别人写的时候还好，自己写的时候就没有注意细节了。
    // 每一个下划线之间都要有空格，然后","最后一个是不可以存在的，要注意了。
    for (auto &t : tab.indexes) {
        std::string col;
        col += "(";
        for (int i = 0; i < t.cols.size(); i++) {
            col += t.cols[i].name;
            if (i < (int)t.cols.size() - 1) col += ",";
        }
        col += ")";

        std::vector<std::string> field_info = {tab_name, "unique", col};
        printer.print_record(field_info, context);
        
        outfile << "| " << tab_name << " | unique | " << col << " |\n";
    }

    printer.print_separator(context);//输出表尾分隔线
    /*
    +--------+ */
    outfile.close();//关闭输出文件
}