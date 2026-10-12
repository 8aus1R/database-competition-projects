/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle* file_handle) : file_handle_(file_handle) {
    // Todo:
    // 初始化file_handle和rid（指向第一个存放了记录的位置）
    rid_ = {RM_FIRST_RECORD_PAGE, RM_NO_PAGE};
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Todo:
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    for (int page_no = rid_.page_no; page_no < file_handle_->file_hdr_.num_pages; page_no++) {
        RmPageHandle rm_page_handle = file_handle_->fetch_page_handle(page_no);
        int max_n = file_handle_->file_hdr_.num_records_per_page;
        //查找当前页面中，从 rid_.slot_no 之后的第一个有效记录槽位（slot_no）
        int slot_no = Bitmap::next_bit(true, rm_page_handle.bitmap, max_n, rid_.slot_no);
        if (slot_no < max_n) {//找到有效槽位
            rid_ = {page_no, slot_no};//更新rid为当前记录的位置并退出
            return;
        }
        //若当前页面从 rid_.slot_no 之后无有效记录，
        //重置 rid_.slot_no 为 RM_NO_PAGE（表示当前页面已扫描完毕），继续遍历下一个页面
        rid_.slot_no = RM_NO_PAGE;
    }
    //若所有页面均已遍历且未找到有效记录，将rid_.page_no 设为 RM_NO_PAGE（表示扫描结束，无更多记录）
    rid_.page_no = RM_NO_PAGE;
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // Todo: 修改返回值

    return rid_.page_no == RM_NO_PAGE;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}