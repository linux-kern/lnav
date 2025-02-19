/**
 * Copyright (c) 2020, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include "all_logs_vtab.hh"
#include "string_attr_type.hh"

static auto intern_lifetime = intern_string::get_table_lifetime();

all_logs_vtab::all_logs_vtab()
    : log_vtab_impl(intern_string::lookup("all_logs")),
      alv_value_meta(intern_string::lookup("log_format"),
                     value_kind_t::VALUE_TEXT,
                     0),
      alv_msg_meta(intern_string::lookup("log_msg_format"),
                   value_kind_t::VALUE_TEXT,
                   1),
      alv_schema_meta(intern_string::lookup("log_msg_schema"),
                      value_kind_t::VALUE_TEXT,
                      2) {
    this->alv_value_meta.lvm_identifier = true;
    this->alv_msg_meta.lvm_identifier = true;
    this->alv_schema_meta.lvm_identifier = true;
}

void all_logs_vtab::get_columns(std::vector<vtab_column> &cols) const
{
    cols.emplace_back(vtab_column(this->alv_value_meta.lvm_name.get())
                          .with_comment("The name of the log file format"));
    cols.emplace_back(vtab_column(this->alv_msg_meta.lvm_name.get())
                          .with_comment("The message format with variables replaced by hash marks"));
    cols.emplace_back(this->alv_schema_meta.lvm_name.get(), SQLITE3_TEXT, "", true,
                      "The ID for the message schema");
}

void all_logs_vtab::extract(std::shared_ptr<logfile> lf, uint64_t line_number,
                            shared_buffer_ref &line,
                            std::vector<logline_value> &values)
{
    auto format = lf->get_format();
    values.emplace_back(this->alv_value_meta, format->get_name());

    std::vector<logline_value> sub_values;

    this->vi_attrs.clear();
    format->annotate(line_number, line, this->vi_attrs, sub_values, false);

    auto body = find_string_attr_range(this->vi_attrs, &SA_BODY);
    if (body.lr_start == -1) {
        body.lr_start = 0;
        body.lr_end = line.length();
    }

    data_scanner ds(line, body.lr_start, body.lr_end);
    data_parser dp(&ds);
    std::string str;

    dp.dp_msg_format = &str;
    dp.parse();

    tmp_shared_buffer tsb(str.c_str());

    values.emplace_back(this->alv_msg_meta, tsb.tsb_ref);

    this->alv_schema_manager.invalidate_refs();
    dp.dp_schema_id.to_string(this->alv_schema_buffer.data());
    shared_buffer_ref schema_ref;
    schema_ref.share(this->alv_schema_manager,
                     this->alv_schema_buffer.data(),
                     data_parser::schema_id_t::STRING_SIZE - 1);
    values.emplace_back(this->alv_schema_meta, schema_ref);
}

bool all_logs_vtab::is_valid(log_cursor &lc, logfile_sub_source &lss)
{
    auto cl = lss.at(lc.lc_curr_line);
    auto lf = lss.find(cl);
    auto lf_iter = lf->begin() + cl;

    if (!lf_iter->is_message()) {
        return false;
    }

    return true;
}

bool all_logs_vtab::next(log_cursor &lc, logfile_sub_source &lss)
{
    lc.lc_curr_line = lc.lc_curr_line + vis_line_t(1);
    lc.lc_sub_index = 0;

    if (lc.is_eof()) {
        return true;
    }

    return this->is_valid(lc, lss);
}
