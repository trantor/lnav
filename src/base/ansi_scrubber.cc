/**
 * Copyright (c) 2013, Timothy Stack
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
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file ansi_scrubber.cc
 */

#include <algorithm>

#include "ansi_scrubber.hh"

#include "base/opt_util.hh"
#include "config.h"
#include "pcrepp/pcrepp.hh"
#include "scn/scn.h"
#include "view_curses.hh"

static const pcrepp&
ansi_regex()
{
    static const pcrepp retval("\x1b\\[([\\d=;\\?]*)([a-zA-Z])|(?:\\X\x08\\X)+",
                               PCRE_UTF8);

    return retval;
}

void
scrub_ansi_string(std::string& str, string_attrs_t* sa)
{
    pcre_context_static<60> context;
    const auto& regex = ansi_regex();
    pcre_input pi(str);
    int64_t origin_offset = 0;
    int last_origin_offset_end = 0;

    replace(str.begin(), str.end(), '\0', ' ');
    while (regex.match(context, pi, PCRE_NO_UTF8_CHECK)) {
        auto* caps = context.all();
        const auto sf = pi.get_string_fragment(caps);
        auto bs_index_res = sf.codepoint_to_byte_index(1);

        if (sf.length() >= 3 && bs_index_res.isOk()
            && sf[bs_index_res.unwrap()] == '\b')
        {
            ssize_t fill_index = sf.sf_begin;
            line_range bold_range;
            line_range ul_range;
            auto sub_sf = sf;

            while (!sub_sf.empty()) {
                auto lhs_opt = sub_sf.consume_codepoint();
                if (!lhs_opt) {
                    return;
                }
                auto lhs_pair = lhs_opt.value();
                auto mid_opt = lhs_pair.second.consume_codepoint();
                if (!mid_opt) {
                    return;
                }
                auto mid_pair = mid_opt.value();
                auto rhs_opt = mid_pair.second.consume_codepoint();
                if (!rhs_opt) {
                    return;
                }
                auto rhs_pair = rhs_opt.value();
                sub_sf = rhs_pair.second;

                if (lhs_pair.first == '_' || rhs_pair.first == '_') {
                    if (sa != nullptr && bold_range.is_valid()) {
                        sa->emplace_back(bold_range,
                                         VC_STYLE.value(text_attrs{A_BOLD}));
                        bold_range.clear();
                    }
                    if (ul_range.is_valid()) {
                        ul_range.lr_end += 1;
                    } else {
                        ul_range.lr_start = fill_index;
                        ul_range.lr_end = fill_index + 1;
                    }
                    auto cp = lhs_pair.first == '_' ? rhs_pair.first
                                                    : lhs_pair.first;
                    ww898::utf::utf8::write(cp, [&str, &fill_index](auto ch) {
                        str[fill_index++] = ch;
                    });
                } else {
                    if (sa != nullptr && ul_range.is_valid()) {
                        sa->emplace_back(
                            ul_range, VC_STYLE.value(text_attrs{A_UNDERLINE}));
                        ul_range.clear();
                    }
                    if (bold_range.is_valid()) {
                        bold_range.lr_end += 1;
                    } else {
                        bold_range.lr_start = fill_index;
                        bold_range.lr_end = fill_index + 1;
                    }
                    try {
                        ww898::utf::utf8::write(lhs_pair.first,
                                                [&str, &fill_index](auto ch) {
                                                    str[fill_index++] = ch;
                                                });
                    } catch (const std::runtime_error& e) {
                        log_error("invalid UTF-8 at %d", sf.sf_begin);
                        return;
                    }
                }
            }

            auto output_size = fill_index - sf.sf_begin;
            auto erased_size = sf.length() - output_size;

            if (sa != nullptr) {
#if 0
                shift_string_attrs(
                    *sa, caps->c_begin + sf.length() / 3, -erased_size);
#endif
                sa->emplace_back(line_range{last_origin_offset_end,
                                            caps->c_begin + (int) output_size},
                                 SA_ORIGIN_OFFSET.value(origin_offset));
            }

            if (sa != nullptr && ul_range.is_valid()) {
                sa->emplace_back(ul_range,
                                 VC_STYLE.value(text_attrs{A_UNDERLINE}));
                ul_range.clear();
            }
            if (sa != nullptr && bold_range.is_valid()) {
                sa->emplace_back(bold_range,
                                 VC_STYLE.value(text_attrs{A_BOLD}));
                bold_range.clear();
            }

            str.erase(str.begin() + fill_index, str.begin() + caps->c_end);
            last_origin_offset_end = caps->c_begin + output_size;
            origin_offset += erased_size;
            pi.reset(str);
            pi.pi_next_offset = last_origin_offset_end;
            continue;
        }

        struct line_range lr;
        bool has_attrs = false;
        text_attrs attrs;
        auto role = nonstd::optional<role_t>();
        size_t lpc;

        switch (pi.get_substr_start(&caps[2])[0]) {
            case 'm':
                for (lpc = caps[1].c_begin;
                     lpc != std::string::npos && lpc < (size_t) caps[1].c_end;)
                {
                    auto ansi_code_res = scn::scan_value<int>(
                        scn::string_view{&str[lpc], &str[caps[1].c_end]});

                    if (ansi_code_res) {
                        auto ansi_code = ansi_code_res.value();
                        if (90 <= ansi_code && ansi_code <= 97) {
                            ansi_code -= 60;
                            attrs.ta_attrs |= A_STANDOUT;
                        }
                        if (30 <= ansi_code && ansi_code <= 37) {
                            attrs.ta_fg_color = ansi_code - 30;
                        }
                        if (40 <= ansi_code && ansi_code <= 47) {
                            attrs.ta_bg_color = ansi_code - 40;
                        }
                        switch (ansi_code) {
                            case 1:
                                attrs.ta_attrs |= A_BOLD;
                                break;

                            case 2:
                                attrs.ta_attrs |= A_DIM;
                                break;

                            case 4:
                                attrs.ta_attrs |= A_UNDERLINE;
                                break;

                            case 7:
                                attrs.ta_attrs |= A_REVERSE;
                                break;
                        }
                    }
                    lpc = str.find(';', lpc);
                    if (lpc != std::string::npos) {
                        lpc += 1;
                    }
                }
                has_attrs = true;
                break;

            case 'C': {
                auto spaces_res = scn::scan_value<unsigned int>(
                    pi.to_string_view(&caps[1]));

                if (spaces_res && spaces_res.value() > 0) {
                    str.insert((std::string::size_type) caps[0].c_end,
                               spaces_res.value(),
                               ' ');
                }
                break;
            }

            case 'H': {
                unsigned int row = 0, spaces = 0;

                if (scn::scan(pi.to_string_view(&caps[1]), "{};{}", row, spaces)
                    && spaces > 1)
                {
                    int ispaces = spaces - 1;
                    if (ispaces > caps[0].c_begin) {
                        str.insert((unsigned long) caps[0].c_end,
                                   ispaces - caps[0].c_begin,
                                   ' ');
                    }
                }
                break;
            }

            case 'O': {
                auto role_res
                    = scn::scan_value<int>(pi.to_string_view(&caps[1]));

                if (role_res) {
                    role_t role_tmp = (role_t) role_res.value();
                    if (role_tmp > role_t::VCR_NONE
                        && role_tmp < role_t::VCR__MAX)
                    {
                        role = role_tmp;
                        has_attrs = true;
                    }
                }
                break;
            }
        }
        str.erase(str.begin() + caps[0].c_begin, str.begin() + caps[0].c_end);
        if (sa != nullptr) {
            shift_string_attrs(*sa, caps[0].c_begin, -caps[0].length());

            if (has_attrs) {
                for (auto rit = sa->rbegin(); rit != sa->rend(); rit++) {
                    if (rit->sa_range.lr_end != -1) {
                        continue;
                    }
                    rit->sa_range.lr_end = caps[0].c_begin;
                }
                lr.lr_start = caps[0].c_begin;
                lr.lr_end = -1;
                if (attrs.ta_attrs || attrs.ta_fg_color || attrs.ta_bg_color) {
                    sa->emplace_back(lr, VC_STYLE.value(attrs));
                }
                role | [&lr, &sa](role_t r) {
                    sa->emplace_back(lr, VC_ROLE.value(r));
                };
            }
            sa->emplace_back(line_range{last_origin_offset_end, caps->c_begin},
                             SA_ORIGIN_OFFSET.value(origin_offset));
            last_origin_offset_end = caps->c_begin;
            origin_offset += caps->length();
        }

        pi.reset(str);
        pi.pi_next_offset = caps->c_begin;
    }

    if (sa != nullptr && last_origin_offset_end > 0) {
        sa->emplace_back(line_range{last_origin_offset_end, (int) str.size()},
                         SA_ORIGIN_OFFSET.value(origin_offset));
    }
}

void
add_ansi_vars(std::map<std::string, scoped_value_t>& vars)
{
    vars["ansi_csi"] = ANSI_CSI;
    vars["ansi_norm"] = ANSI_NORM;
    vars["ansi_bold"] = ANSI_BOLD_START;
    vars["ansi_underline"] = ANSI_UNDERLINE_START;
    vars["ansi_black"] = ANSI_COLOR(COLOR_BLACK);
    vars["ansi_red"] = ANSI_COLOR(COLOR_RED);
    vars["ansi_green"] = ANSI_COLOR(COLOR_GREEN);
    vars["ansi_yellow"] = ANSI_COLOR(COLOR_YELLOW);
    vars["ansi_blue"] = ANSI_COLOR(COLOR_BLUE);
    vars["ansi_magenta"] = ANSI_COLOR(COLOR_MAGENTA);
    vars["ansi_cyan"] = ANSI_COLOR(COLOR_CYAN);
    vars["ansi_white"] = ANSI_COLOR(COLOR_WHITE);
}
