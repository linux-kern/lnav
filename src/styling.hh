/**
 * Copyright (c) 2019, Timothy Stack
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

#ifndef styling_hh
#define styling_hh

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "log_level.hh"
#include "base/result.h"
#include "base/intern_string.hh"
#include "mapbox/variant.hpp"

struct rgb_color {
    static Result<rgb_color, std::string> from_str(const string_fragment &sf);

    explicit rgb_color(short r = -1, short g = -1, short b = -1)
        : rc_r(r), rc_g(g), rc_b(b)
    {
    }

    bool empty() const
    {
        return this->rc_r == -1 &&
               this->rc_g == -1 &&
               this->rc_b == -1;
    }

    bool operator==(const rgb_color &rhs) const;

    bool operator!=(const rgb_color &rhs) const;

    bool operator<(const rgb_color &rhs) const;

    bool operator>(const rgb_color &rhs) const;

    bool operator<=(const rgb_color &rhs) const;

    bool operator>=(const rgb_color &rhs) const;

    short rc_r;
    short rc_g;
    short rc_b;
};

struct lab_color {
    lab_color() : lc_l(0), lc_a(0), lc_b(0) {
    };

    explicit lab_color(const rgb_color &rgb);

    double deltaE(const lab_color &other) const;

    lab_color& operator=(const lab_color &other) {
        this->lc_l = other.lc_l;
        this->lc_a = other.lc_a;
        this->lc_b = other.lc_b;

        return *this;
    };

    bool operator==(const lab_color &rhs) const;

    bool operator!=(const lab_color &rhs) const;

    bool operator<(const lab_color &rhs) const;

    bool operator>(const lab_color &rhs) const;

    bool operator<=(const lab_color &rhs) const;

    bool operator>=(const lab_color &rhs) const;

    double lc_l;
    double lc_a;
    double lc_b;
};

struct term_color {
    short xc_id;
    std::string xc_name;
    std::string xc_hex;
    rgb_color xc_color;
    lab_color xc_lab_color;
};

struct term_color_palette {
    explicit term_color_palette(const string_fragment& json);

    short match_color(const lab_color &to_match);

    std::vector<term_color> tc_palette;
};

namespace styling {

struct semantic {};

class color_unit {
public:
    static Result<color_unit, std::string> from_str(const string_fragment& sf);

    static color_unit make_empty() {
        return { rgb_color{} };
    }

    bool empty() const {
        return this->cu_value.match(
            [](semantic) { return false; },
            [](const rgb_color& rc) { return rc.empty(); }
        );
    }

    using variants_t = mapbox::util::variant<semantic, rgb_color>;

    variants_t cu_value;

private:
    color_unit(variants_t value) : cu_value(std::move(value)) {}
};

}

struct style_config {
    std::string sc_color;
    std::string sc_background_color;
    bool sc_underline{false};
    bool sc_bold{false};
};

struct highlighter_config {
    std::string hc_regex;
    style_config hc_style;
};

struct lnav_theme {
    std::map<std::string, std::string> lt_vars;
    style_config lt_style_identifier;
    style_config lt_style_text;
    style_config lt_style_alt_text;
    style_config lt_style_ok;
    style_config lt_style_error;
    style_config lt_style_warning;
    style_config lt_style_popup;
    style_config lt_style_focused;
    style_config lt_style_disabled_focused;
    style_config lt_style_scrollbar;
    style_config lt_style_hidden;
    style_config lt_style_adjusted_time;
    style_config lt_style_skewed_time;
    style_config lt_style_offset_time;
    style_config lt_style_invalid_msg;
    style_config lt_style_status_title;
    style_config lt_style_status_title_hotkey;
    style_config lt_style_status_disabled_title;
    style_config lt_style_status_subtitle;
    style_config lt_style_status_hotkey;
    style_config lt_style_keyword;
    style_config lt_style_string;
    style_config lt_style_comment;
    style_config lt_style_doc_directive;
    style_config lt_style_variable;
    style_config lt_style_symbol;
    style_config lt_style_number;
    style_config lt_style_re_special;
    style_config lt_style_re_repeat;
    style_config lt_style_diff_delete;
    style_config lt_style_diff_add;
    style_config lt_style_diff_section;
    style_config lt_style_low_threshold;
    style_config lt_style_med_threshold;
    style_config lt_style_high_threshold;
    style_config lt_style_status;
    style_config lt_style_warn_status;
    style_config lt_style_alert_status;
    style_config lt_style_active_status;
    style_config lt_style_inactive_status;
    style_config lt_style_inactive_alert_status;
    style_config lt_style_file;
    std::map<log_level_t, style_config> lt_level_styles;
    std::map<std::string, highlighter_config> lt_highlights;
};

extern term_color_palette *xterm_colors();
extern term_color_palette *ansi_colors();

#endif
