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
#include <queue>
#include <sstream>

#define MAX_CHUNK_SIZE 5

class SortExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;                              // 框架中只支持一个键排序，需要自行修改数据结构支持多个键排序
    std::vector<bool> is_desc_;
    int limit_;
    std::unique_ptr<RmRecord> current_tuple_;
    std::deque<std::unique_ptr<RmRecord> > records_;
    size_t len_; // 字段总长度
    // 题目要求排序结果写入 sorted_results.txt，字符串
    std::fstream out_expected_file_;
    // 排序结果实际读取文件，二进制
    std::fstream outfile_;
    // 实际二进制文件名
    std::string filename_;
    std::deque<std::string> temp_files_;
    // 为每个 sort 算子实例分配一个 ID
    size_t id_;
    bool is_end_;
    const std::size_t batch_size = 2; // 二路归并, 可以修改为多路归并

    static std::size_t generateID() {
        static size_t current_id = 0;
        return ++current_id;
    }

    // 排序一部分元组并放入块（临时文件）中
    void sortAndSaveChunk() {
        // 给满足小于等于缓冲池大小的全部加入内存，然后进行排序。
        std::sort(records_.begin(), records_.end(), [&] (const std::unique_ptr<RmRecord> &a, const std::unique_ptr<RmRecord> &b) {
            for (int i = 0; i < (int)cols_.size(); i++) {
                auto &&lhs = a->data + cols_[i].offset;
                auto &&rhs = b->data + cols_[i].offset;
                int cmp = ix_compare(lhs, rhs, cols_[i].type, cols_[i].len);
                if (cmp != 0) {
                    return is_desc_[i] ? cmp > 0 : cmp < 0;
                }
            }
            return false;
        });
        // 将排序好的这个部分写到temp_file中去，记录下来。
        std::string temp_filename = "sorted_chunk_" + std::to_string(temp_files_.size()) + ".tmp";
        std::ofstream temp_file(temp_filename, std::ios::out | std::ios::binary);
        for (const auto &record: records_) {
            temp_file.write(record->data, record->size);
        }
        temp_file.close();
        temp_files_.emplace_back(temp_filename);
        records_.clear();
    }
    // 打印数组到sort_result中去。
    void print() {
        assert(temp_files_.size() == 1);

        // 最终排序好的文件名
        filename_ = std::move(temp_files_[0]);
        temp_files_.clear();

        outfile_.open(filename_, std::ios::in | std::ios::binary);

        // 以期望格式写入 sorted_results.txt
        out_expected_file_.open("sorted_results.txt", std::ios::out | std::ios::app);

        // 打印表头
        out_expected_file_ << "|";
        for (auto &col_meta: prev_->cols()) {
            out_expected_file_ << " " << col_meta.name << " |";
        }
        out_expected_file_ << "\n";

        while (true) {
            current_tuple_ = std::make_unique<RmRecord>(len_);
            outfile_.read(current_tuple_->data, current_tuple_->size);
            if (outfile_.gcount() > 0) {
                // 打印记录
                std::vector<std::string> columns;
                columns.reserve(prev_->cols().size());

                for (auto &col: prev_->cols()) {
                    std::string col_str;
                    char *rec_buf = current_tuple_->data + col.offset;
                    if (col.type == TYPE_INT) {
                        col_str = std::to_string(*(int *) rec_buf);
                    } else if (col.type == TYPE_FLOAT) {
                        col_str = std::to_string(*(float *) rec_buf);
                    } else if (col.type == TYPE_STRING) {
                        col_str = std::string((char *) rec_buf, col.len);
                        col_str.resize(strlen(col_str.c_str()));
                    }
                    columns.emplace_back(col_str);
                }

                // 打印记录
                out_expected_file_ << "|";
                for (auto &col: columns) {
                    out_expected_file_ << " " << col << " |";
                }
                out_expected_file_ << "\n";
            } else {
                break;
            }
        }

        outfile_.close();
        out_expected_file_.close();
    }

    void mergeTwoFile() {
        // 必须要支持多级归并排序，因为系统可以使用的文件描述符有限，超过使用限制会发生阻塞
        // 排序阶段，已经完成了第几级排序
        std::size_t level = 1;
        // 每个阶段的临时文件
        std::deque<std::string> level_temp_files;
        
        // 最后归并为一个排好序的文件
        while (temp_files_.size() > 1) {
            std::size_t total_files = temp_files_.size(); // 当层操作的总次数
            std::size_t processed_files = 0; // 当层操作到了第几个。

            while (processed_files < total_files) {
                std::vector<std::ifstream> temp_file_streams; // 临时操作文件
                // 将batch_size个放入，并且判断此时是否会超过总长度（最后一组可能不满）。
                for (size_t i = 0; i < batch_size && processed_files < total_files; ++i, ++processed_files) {
                    const auto &filename = temp_files_[processed_files];
                    temp_file_streams.emplace_back(filename, std::ios::in | std::ios::binary);
                }
                
                std::string temp_filename = "sorted_ID" + std::to_string(id_) + "_chunk_" + std::to_string(level) + "_"
                                            + std::to_string(level_temp_files.size()) + ".tmp";
                std::ofstream temp_file(temp_filename, std::ios::out | std::ios::binary);
                level_temp_files.emplace_back(std::move(temp_filename));
                // 如果长度为1，就单独进行操作，直接全部写入。
                if (temp_file_streams.size() == 1) {
                    std::unique_ptr<RmRecord> left_record = std::make_unique<RmRecord>(len_);
                    temp_file_streams[0].read(left_record->data, left_record->size);
                    while (temp_file_streams[0].gcount()) {
                        temp_file.write(left_record->data, left_record->size);
                        temp_file_streams[0].read(left_record->data, left_record->size);
                    }
                    temp_file_streams[0].close();
                    temp_file.close();
                    break;
                }
                
                // 否则进行两个有序记录的合并，双指针去模拟可以做到线性复杂度。
                std::unique_ptr<RmRecord> left_record = std::make_unique<RmRecord>(len_);
                std::unique_ptr<RmRecord> right_record = std::make_unique<RmRecord>(len_);
                temp_file_streams[0].read(left_record->data, left_record->size);
                temp_file_streams[1].read(right_record->data, right_record->size);
                while (temp_file_streams[0].gcount() && temp_file_streams[1].gcount()) {
                    int cmp = -1, flag = -1; // 需要分类讨论升序和降序，以及两边大小的情况。
                    for (int i = 0; i < (int)cols_.size(); i++) {
                        auto lhs = left_record->data + cols_[i].offset;
                        auto rhs = right_record->data + cols_[i].offset;
                        cmp = ix_compare(lhs, rhs, cols_[i].type, cols_[i].len);
                        if (cmp != 0) {
                            flag = is_desc_[i];
                            break;
                        }
                    }
                    // 如果flag为-1表示两者相等，可以同时移动。
                    // 如果flag为1并且cmp也为1，表示降序左边大或者升序右边大，那么就操作左边，相反同理。
                    if (flag == -1) {
                        temp_file.write(left_record->data, left_record->size);
                        temp_file.write(right_record->data, right_record->size);
                        temp_file_streams[0].read(left_record->data, left_record->size);
                        temp_file_streams[1].read(right_record->data, right_record->size);
                    } else if ((flag == 1 && cmp > 0) || (flag == 0 && cmp < 0)) {
                        temp_file.write(left_record->data, left_record->size);
                        temp_file_streams[0].read(left_record->data, left_record->size);
                    } else {
                        temp_file.write(right_record->data, right_record->size);
                        temp_file_streams[1].read(right_record->data, right_record->size);
                    }
                }

                while (temp_file_streams[0].gcount()) {
                    temp_file.write(left_record->data, left_record->size);
                    temp_file_streams[0].read(left_record->data, left_record->size);
                }
                temp_file_streams[0].close();

                while (temp_file_streams[1].gcount()) {
                    temp_file.write(right_record->data, right_record->size);
                    temp_file_streams[1].read(right_record->data, right_record->size);
                }
                temp_file_streams[1].close();

                temp_file.close();
            }

            // 清除之前的临时文件
            for (auto &temp_file: temp_files_) {
                unlink(temp_file.c_str());
            }
            // 然后将本层执行的临时文件赋值给总文件，不断地进行迭代循环。
            temp_files_ = std::move(level_temp_files);
            level_temp_files.clear();
            ++level;
        }
    }

    void mergeKFile() {
        // 自定义堆排序。
        auto compareRecords = [&](std::pair<std::unique_ptr<RmRecord>, std::ifstream *> &a,
                                  std::pair<std::unique_ptr<RmRecord>, std::ifstream *> &b) {
            for (int i = 0; i < (int)cols_.size(); i++) {
                auto &&lhs = a.first->data + cols_[i].offset;
                auto &&rhs = b.first->data + cols_[i].offset;
                int cmp = ix_compare(lhs, rhs, cols_[i].type, cols_[i].len);
                if (cmp != 0) {
                    return is_desc_[i] ? cmp < 0 : cmp > 0;
                }
            }
            return false;
        };

        std::priority_queue<
            std::pair<std::unique_ptr<RmRecord>, std::ifstream *>,
            std::vector<std::pair<std::unique_ptr<RmRecord>, std::ifstream *>>,
            decltype(compareRecords) > min_heap(compareRecords);

        // 必须要支持多级归并排序，因为系统可以使用的文件描述符有限，超过使用限制会发生阻塞
        // 排序阶段，已经完成了第几级排序
        std::size_t level = 1;
        // 每个阶段的临时文件
        std::deque<std::string> level_temp_files;
        
        // 最后归并为一个排好序的文件
        while (temp_files_.size() > 1) {
            std::size_t total_files = temp_files_.size(); // 当层操作的总次数
            std::size_t processed_files = 0; // 当层操作到了第几个。

            while (processed_files < total_files) {
                std::vector<std::ifstream> temp_file_streams; // 临时操作文件
                // 将batch_size个放入，并且判断此时是否会超过总长度（最后一组可能不满）。
                for (size_t i = 0; i < batch_size && processed_files < total_files; ++i, ++processed_files) {
                    const auto &filename = temp_files_[processed_files];
                    temp_file_streams.emplace_back(filename, std::ios::in | std::ios::binary);
                }
                // 初始化最小堆，将每一个文件的第一个元素放入到最小堆中去，每次选出最小的写入。
                for (auto &file_stream: temp_file_streams) {
                    auto record = std::make_unique<RmRecord>(len_);
                    file_stream.read(record->data, record->size);
                    if (file_stream.gcount() > 0) {
                        min_heap.emplace(std::move(record), &file_stream);
                    }
                }

                std::string temp_filename = "sorted_ID" + std::to_string(id_) + "_chunk_" + std::to_string(level) + "_"
                                            + std::to_string(level_temp_files.size()) + ".tmp";
                std::ofstream temp_file(temp_filename, std::ios::out | std::ios::binary);
                level_temp_files.emplace_back(std::move(temp_filename));

                // 放到第 level 阶段临时文件中, 并且进行堆模拟，将两个文件合并为一个文件。
                while (!min_heap.empty()) {
                    auto &[record, file_stream] = min_heap.top();
                    temp_file.write(record->data, record->size);
                    auto new_record = std::make_unique<RmRecord>(len_);
                    file_stream->read(new_record->data, new_record->size);
                    if (file_stream->gcount() > 0) {
                        min_heap.emplace(std::move(new_record), file_stream);
                    } else {
                        file_stream->close();
                    }
                    min_heap.pop();
                }

                temp_file.close();
            }

            // 清除之前的临时文件
            for (auto &temp_file: temp_files_) {
                unlink(temp_file.c_str());
            }
            // 然后将本层执行的临时文件赋值给总文件，不断地进行迭代循环。
            temp_files_ = std::move(level_temp_files);
            level_temp_files.clear();
            ++level;
        }
    }

    // 合并每一个文件存储的信息，最终变为一个块。
    void mergeSortedChunks() {
        if (batch_size > 2) {
            mergeKFile();
        } else {
            mergeTwoFile();
        }
        print();
    }

   public:
    
    SortExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<TabCol> sel_cols, std::vector<bool> is_desc, int limit) {
        prev_ = std::move(prev);
        for (const auto& sel_col : sel_cols) {
            cols_.push_back(*get_col(prev_->cols(), sel_col));
        }
        is_desc_ = is_desc;
        limit_ = limit;
        len_ = prev_->tupleLen();
        id_ = generateID();
    }

    const std::vector<ColMeta> &cols() const override {
        return prev_->cols();
    };

    size_t tupleLen() const override {
        return len_;
    }

    void beginTuple() override {
        records_.clear();
        temp_files_.clear();
        current_tuple_ = nullptr;
        is_end_ = false;
        // 内存中可能放不下，实现外部排序
        for (prev_->beginTuple(); !prev_->is_end(); prev_->nextTuple()) {
            records_.emplace_back(prev_->Next());
            if (records_.size() >= MAX_CHUNK_SIZE) {
                sortAndSaveChunk();
            }
        }

        // 剩下未满最大值的记录们也写入磁盘
        if (!records_.empty()) {
            sortAndSaveChunk();
        }

        // 空表
        if (temp_files_.empty()) {
            is_end_ = true;
            return;
        }

        // 合并所有块
        mergeSortedChunks();

        // 读模式
        outfile_.open(filename_, std::ios::in | std::ios::binary);

        // 缓存记录，每次最多值可以读取MAX_CHUNK_SIZE多个。
        do {
            current_tuple_ = std::make_unique<RmRecord>(len_);
            outfile_.read(current_tuple_->data, current_tuple_->size);
            if (outfile_.gcount() == 0) {
                break;
            }
            records_.emplace_back(std::move(current_tuple_));
        } while (records_.size() < MAX_CHUNK_SIZE);

        if (!records_.empty()) {
            current_tuple_ = std::move(records_.front());
            records_.pop_front();
        } else {
            is_end_ = true;
            current_tuple_ = nullptr;
            outfile_.close();
        }
    }

    void nextTuple() override {
        if (limit_ != -1) limit_--;
        if (!records_.empty()) {
            current_tuple_ = std::move(records_.front());
            records_.pop_front();
            return;
        }

        do {
            current_tuple_ = std::make_unique<RmRecord>(len_);
            outfile_.read(current_tuple_->data, current_tuple_->size);
            if (outfile_.gcount() == 0) {
                break;
            }
            records_.emplace_back(std::move(current_tuple_));
        } while (records_.size() < MAX_CHUNK_SIZE);

        if (!records_.empty()) {
            current_tuple_ = std::move(records_.front());
            records_.pop_front();
        } else {
            is_end_ = true;
            current_tuple_ = nullptr;
            outfile_.close();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return std::move(current_tuple_);
    }

    bool is_end() const override {
        return is_end_ || (limit_ != -1 && limit_ <= 0);
    }

    bool is_aggregate() override {
        return prev_->is_aggregate();
    }

    Rid &rid() override { return _abstract_rid; }

    std::string getType() { return "SortExecutor"; };
};