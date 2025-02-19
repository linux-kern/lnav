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
 *
 * @file logfile_fwd.hh
 */

#ifndef lnav_logfile_fwd_hh
#define lnav_logfile_fwd_hh

#include <chrono>
#include <string>

#include "auto_fd.hh"
#include "file_format.hh"

using ui_clock = std::chrono::steady_clock;

class logfile;
class logline_observer;

enum class logfile_name_source {
    USER,
    ARCHIVE,
    REMOTE,
};

struct logfile_open_options {
    logfile_open_options &with_filename(const std::string& val) {
        this->loo_filename = val;

        return *this;
    };

    logfile_open_options &with_fd(auto_fd fd) {
        this->loo_fd = std::move(fd);

        return *this;
    };

    logfile_open_options &with_source(logfile_name_source src) {
        this->loo_source = src;

        return *this;
    }

    logfile_open_options &with_detect_format(bool val) {
        this->loo_detect_format = val;

        return *this;
    };

    logfile_open_options &with_include_in_session(bool val) {
        this->loo_include_in_session = val;

        return *this;
    };

    logfile_open_options &with_visibility(bool val) {
        this->loo_is_visible = val;

        return *this;
    }

    logfile_open_options &with_non_utf_visibility(bool val) {
        this->loo_non_utf_is_visible = val;

        return *this;
    };

    logfile_open_options &with_visible_size_limit(ssize_t val) {
        this->loo_visible_size_limit = val;

        return *this;
    }

    logfile_open_options &with_tail(bool val) {
        this->loo_tail = val;

        return *this;
    }

    logfile_open_options &with_file_format(file_format_t ff) {
        this->loo_file_format = ff;

        return *this;
    }

    std::string loo_filename;
    auto_fd loo_fd;
    logfile_name_source loo_source{logfile_name_source::USER};
    bool loo_detect_format{true};
    bool loo_include_in_session{true};
    bool loo_is_visible{true};
    bool loo_non_utf_is_visible{true};
    ssize_t loo_visible_size_limit{-1};
    bool loo_tail{true};
    file_format_t loo_file_format{file_format_t::FF_UNKNOWN};
};

#endif
