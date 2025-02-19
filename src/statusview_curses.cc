/**
 * Copyright (c) 2007-2012, Timothy Stack
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
 * @file statusview_curses.cc
 */

#include "config.h"

#include <vector>
#include <algorithm>

#include "statusview_curses.hh"

using namespace std;

void status_field::set_value(std::string value)
{
    string_attrs_t &sa = this->sf_value.get_attrs();

    sa.clear();

    scrub_ansi_string(value, sa);
    this->sf_value.with_string(value);

    if (this->sf_cylon) {
        this->do_cylon();
    }
}

void status_field::do_cylon()
{
    string_attrs_t &sa = this->sf_value.get_attrs();

    remove_string_attr(sa, &view_curses::VC_STYLE);

    struct line_range lr(this->sf_cylon_pos, this->sf_width);
    view_colors &vc = view_colors::singleton();

    sa.emplace_back(lr, &view_curses::VC_STYLE,
                    vc.attrs_for_role(view_colors::VCR_ACTIVE_STATUS) | A_REVERSE);

    this->sf_cylon_pos += 1;
    if (this->sf_cylon_pos > this->sf_width) {
        this->sf_cylon_pos = 0;
    }
}

void status_field::set_stitch_value(view_colors::role_t left,
                                    view_colors::role_t right)
{
    string_attrs_t &sa = this->sf_value.get_attrs();
    struct line_range lr(0, 1);

    this->sf_value.get_string() = "::";
    sa.clear();
    sa.emplace_back(lr, &view_curses::VC_ROLE, left);
    lr.lr_start = 1;
    lr.lr_end   = 2;
    sa.emplace_back(lr, &view_curses::VC_ROLE, right);
}

void statusview_curses::do_update()
{
    int           top, attrs, field, field_count, left = 0, right;
    view_colors & vc = view_colors::singleton();
    unsigned long width, height;

    if (!this->vc_visible) {
        return;
    }

    getmaxyx(this->sc_window, height, width);
    this->window_change();

    top   = this->sc_top < 0 ? height + this->sc_top : this->sc_top;
    right = width;
    attrs = vc.attrs_for_role(this->sc_enabled ? view_colors::VCR_STATUS
        : view_colors::VCR_INACTIVE_STATUS);

    wattron(this->sc_window, attrs);
    wmove(this->sc_window, top, 0);
    wclrtoeol(this->sc_window);
    whline(this->sc_window, ' ', width);
    wattroff(this->sc_window, attrs);

    if (this->sc_source != nullptr) {
        field_count = this->sc_source->statusview_fields();
        for (field = 0; field < field_count; field++) {
            status_field &sf = this->sc_source->statusview_value_for_field(
                field);
            struct line_range lr(0, sf.get_width());
            attr_line_t val;
            int x;

            val = sf.get_value();
            if (!this->sc_enabled) {
                for (auto &sa : val.get_attrs()) {
                    if (sa.sa_type == &view_curses::VC_STYLE) {
                        sa.sa_value.sav_int &= ~(A_REVERSE | A_COLOR);
                    } else if (sa.sa_type == &view_curses::VC_ROLE) {
                        if (sa.sa_value.sav_int == view_colors::VCR_ALERT_STATUS) {
                            sa.sa_value.sav_int = view_colors::VCR_INACTIVE_ALERT_STATUS;
                        } else {
                            sa.sa_value.sav_int = view_colors::VCR_NONE;
                        }
                    }
                }
            } else if (sf.is_cylon()) {
                sf.do_cylon();
            }
            if (sf.get_left_pad() > 0) {
                val.insert(0, sf.get_left_pad(), ' ');
            }

            if (sf.is_right_justified()) {
                val.right_justify(sf.get_width());

                right -= sf.get_width();
                x = right;
            }
            else {
                x = left;
                left += sf.get_width();
            }

            if (val.length() > sf.get_width()) {
                static const string ELLIPSIS = "\xE2\x8B\xAF";

                if (sf.get_width() > 11) {
                    size_t half_width = sf.get_width() / 2 - 1;

                    val.erase(half_width, val.length() - (half_width * 2));
                    val.insert(half_width, ELLIPSIS);
                } else {
                    val = val.subline(0, sf.get_width() - 1);
                    val.append(ELLIPSIS);
                }
            }

            auto default_role = sf.get_role();
            if (!this->sc_enabled) {
                if (default_role == view_colors::VCR_ALERT_STATUS) {
                    default_role = view_colors::VCR_INACTIVE_ALERT_STATUS;
                } else {
                    default_role = view_colors::VCR_INACTIVE_STATUS;
                }
            }

            mvwattrline(this->sc_window,
                        top, x,
                        val,
                        lr,
                        default_role);
        }
    }
    wmove(this->sc_window, top + 1, 0);
}

void statusview_curses::window_change()
{
    if (this->sc_source == nullptr) {
        return;
    }

    int           field_count = this->sc_source->statusview_fields();
    int           total_shares = 0;
    unsigned long width, height;
    double remaining = 0;
    vector<status_field *> resizable;

    getmaxyx(this->sc_window, height, width);
    // Silence the compiler. Remove this if height is used at a later stage.
    (void)height;
    remaining = width - 2;

    for (int field = 0; field < field_count; field++) {
        auto &sf = this->sc_source->statusview_value_for_field(field);

        remaining -= sf.get_share() ? sf.get_min_width() : sf.get_width();
        total_shares += sf.get_share();
        if (sf.get_share()) {
            resizable.emplace_back(&sf);
        }
    }

    if (remaining < 2) {
        remaining = 0;
    }

    std::stable_sort(begin(resizable), end(resizable), [](auto l, auto r) {
        return r->get_share() < l->get_share();
    });
    for (auto sf : resizable) {
        double divisor = total_shares / sf->get_share();
        int available = remaining / divisor;
        int actual_width;

        if ((sf->get_left_pad() + sf->get_value().length()) <
            (sf->get_min_width() + available)) {
            actual_width = std::max((int) sf->get_min_width(),
                                    (int) (sf->get_left_pad() +
                                           sf->get_value().length()));
        } else {
            actual_width = sf->get_min_width() + available;
        }
        remaining -= (actual_width - sf->get_min_width());
        total_shares -= sf->get_share();

        sf->set_width(actual_width);
    }

    this->sc_last_width = width;
}
