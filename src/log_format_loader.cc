/**
 * Copyright (c) 2013-2016, Timothy Stack
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
 * @file log_format_loader.cc
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <libgen.h>
#include <sys/stat.h>

#include <map>
#include <string>

#include "fmt/format.h"
#include "file_format.hh"

#include "base/paths.hh"
#include "base/string_util.hh"
#include "yajlpp/yajlpp.hh"
#include "yajlpp/yajlpp_def.hh"
#include "lnav_config.hh"
#include "log_format_ext.hh"
#include "auto_fd.hh"
#include "sql_util.hh"
#include "lnav_util.hh"
#include "builtin-scripts.h"
#include "builtin-sh-scripts.h"
#include "default-formats.h"

#include "log_format_loader.hh"
#include "bin2c.hh"

using namespace std;

static void extract_metadata(const char *contents, size_t len, struct script_metadata &meta_out);

typedef map<intern_string_t, std::shared_ptr<external_log_format>> log_formats_map_t;

static auto intern_lifetime = intern_string::get_table_lifetime();
static log_formats_map_t LOG_FORMATS;

struct userdata {
    ghc::filesystem::path ud_format_path;
    vector<intern_string_t> *ud_format_names{nullptr};
    std::vector<std::string> *ud_errors{nullptr};
};

static external_log_format *ensure_format(const yajlpp_provider_context &ypc, userdata *ud)
{
    const intern_string_t name = ypc.get_substr_i(0);
    vector<intern_string_t> *formats = ud->ud_format_names;
    external_log_format *retval;

    retval = LOG_FORMATS[name].get();
    if (retval == nullptr) {
        LOG_FORMATS[name] = std::make_shared<external_log_format>(name);
        retval = LOG_FORMATS[name].get();
        log_debug("Loading format -- %s", name.get());
    }
    retval->elf_source_path.insert(ud->ud_format_path.filename().string());

    if (find(formats->begin(), formats->end(), name) == formats->end()) {
        formats->push_back(name);
    }

    if (ud->ud_format_path.empty()) {
        retval->elf_builtin_format = true;
    }

    return retval;
}

static external_log_format::pattern *pattern_provider(const yajlpp_provider_context &ypc, external_log_format *elf)
{
    string regex_name = ypc.get_substr(0);
    auto &pat = elf->elf_patterns[regex_name];

    if (pat.get() == nullptr) {
        pat = make_shared<external_log_format::pattern>();
    }

    if (pat->p_config_path.empty()) {
        pat->p_config_path = elf->get_name().to_string() + "/regex/" + regex_name;
    }

    return pat.get();
}

static external_log_format::value_def *value_def_provider(const yajlpp_provider_context &ypc, external_log_format *elf)
{
    const intern_string_t value_name = ypc.get_substr_i(0);

    auto iter = elf->elf_value_defs.find(value_name);
    shared_ptr<external_log_format::value_def> retval;

    if (iter == elf->elf_value_defs.end()) {
        retval = make_shared<external_log_format::value_def>(
            value_name, value_kind_t::VALUE_TEXT, -1, elf);
        elf->elf_value_defs[value_name] = retval;
        elf->elf_value_def_order.emplace_back(retval);
    } else {
        retval = iter->second;
    }

    return retval.get();
}

static scaling_factor *scaling_factor_provider(const yajlpp_provider_context &ypc, external_log_format::value_def *value_def)
{
    auto scale_name = ypc.get_substr_i(0);
    scaling_factor &retval = value_def->vd_unit_scaling[scale_name];

    return &retval;
}

static external_log_format::json_format_element &
ensure_json_format_element(external_log_format *elf, int index)
{
    elf->jlf_line_format.resize(index + 1);

    return elf->jlf_line_format[index];
}

static external_log_format::json_format_element *line_format_provider(
    const yajlpp_provider_context &ypc, external_log_format *elf)
{
    auto &jfe = ensure_json_format_element(elf, ypc.ypc_index);

    jfe.jfe_type = external_log_format::JLF_VARIABLE;

    return &jfe;
}

static int read_format_bool(yajlpp_parse_context *ypc, int val)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    string field_name = ypc->get_path_fragment(1);

    if (field_name == "convert-to-local-time")
        elf->lf_date_time.dts_local_time = val;
    else if (field_name == "json") {
        if (val) {
            elf->elf_type = external_log_format::ELF_TYPE_JSON;
        }
    }
    else if (field_name == "hide-extra")
        elf->jlf_hide_extra = val;
    else if (field_name == "multiline")
        elf->lf_multiline = val;

    return 1;
}

static int read_format_double(yajlpp_parse_context *ypc, double val)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    string field_name = ypc->get_path_fragment(1);

    if (field_name == "timestamp-divisor") {
        if (val <= 0) {
            fprintf(stderr, "error:%s: timestamp-divisor cannot be less "
                "than or equal to zero\n",
                ypc->get_path_fragment(0).c_str());
            return 0;
        }
        elf->elf_timestamp_divisor = val;
    }

    return 1;
}

static int read_format_int(yajlpp_parse_context *ypc, long long val)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    string field_name = ypc->get_path_fragment(1);

    if (field_name == "timestamp-divisor") {
        if (val <= 0) {
            fprintf(stderr, "error:%s: timestamp-divisor cannot be less "
                "than or equal to zero\n",
                ypc->get_path_fragment(0).c_str());
            return 0;
        }
        elf->elf_timestamp_divisor = val;
    }

    return 1;
}

static int read_format_field(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    auto leading_slash = len > 0 && str[0] == '/';
    auto value = string((const char *) (leading_slash ? str + 1 : str),
                        leading_slash ? len - 1 : len);
    auto field_name = ypc->get_path_fragment(1);

    if (field_name == "file-pattern") {
        elf->elf_file_pattern = value;
    }
    else if (field_name == "level-field") {
        elf->elf_level_field = intern_string::lookup(value);
    }
    else if (field_name == "level-pointer") {
        auto pcre_res = pcrepp::from_str(value);

        if (pcre_res.isErr()) {
            ypc->ypc_error_reporter(
                *ypc,
                lnav_log_level_t::ERROR,
                fmt::format("error:{}:{}:invalid regular expression for level-pointer -- {}",
                            ypc->ypc_source,
                            ypc->get_line_number(),
                            pcre_res.unwrapErr().ce_msg).c_str());
        } else {
            elf->elf_level_pointer = pcre_res.unwrap();
        }
    }
    else if (field_name == "timestamp-field") {
        elf->lf_timestamp_field = intern_string::lookup(value);
    }
    else if (field_name == "body-field") {
        elf->elf_body_field = intern_string::lookup(value);
    }
    else if (field_name == "timestamp-format") {
        elf->lf_timestamp_format.push_back(intern_string::lookup(value)->get());
    }
    else if (field_name == "module-field") {
        elf->elf_module_id_field = intern_string::lookup(value);
        elf->elf_container = true;
    }
    else if (field_name == "opid-field") {
        elf->elf_opid_field = intern_string::lookup(value);
    }
    else if (field_name == "mime-types") {
        auto value_opt = ypc->ypc_current_handler->to_enum_value(value);
        if (value_opt) {
            elf->elf_mime_types.insert((file_format_t) *value_opt);
        }
    }

    return 1;
}

static int read_levels(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    string regex = string((const char *)str, len);
    string level_name_or_number = ypc->get_path_fragment(2);
    log_level_t level = string2level(level_name_or_number.c_str());
    elf->elf_level_patterns[level].lp_regex = regex;

    return 1;
}

static int read_level_int(yajlpp_parse_context *ypc, long long val)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    string level_name_or_number = ypc->get_path_fragment(2);
    log_level_t level = string2level(level_name_or_number.c_str());

    elf->elf_level_pairs.emplace_back(val, level);

    return 1;
}

static int read_action_def(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    string action_name = ypc->get_path_fragment(2);
    string field_name = ypc->get_path_fragment(3);
    string val = string((const char *)str, len);

    elf->lf_action_defs[action_name].ad_name = action_name;
    if (field_name == "label")
        elf->lf_action_defs[action_name].ad_label = val;

    return 1;
}

static int read_action_bool(yajlpp_parse_context *ypc, int val)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    string action_name = ypc->get_path_fragment(2);
    string field_name = ypc->get_path_fragment(3);

    elf->lf_action_defs[action_name].ad_capture_output = val;

    return 1;
}

static int read_action_cmd(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    string action_name = ypc->get_path_fragment(2);
    string field_name = ypc->get_path_fragment(3);
    string val = string((const char *)str, len);

    elf->lf_action_defs[action_name].ad_name = action_name;
    elf->lf_action_defs[action_name].ad_cmdline.push_back(val);

    return 1;
}

static external_log_format::sample &ensure_sample(external_log_format *elf,
                                                  int index)
{
    elf->elf_samples.resize(index + 1);

    return elf->elf_samples[index];
}

static external_log_format::sample *sample_provider(const yajlpp_provider_context &ypc, external_log_format *elf)
{
    external_log_format::sample &sample = ensure_sample(elf, ypc.ypc_index);

    return &sample;
}

static int read_json_constant(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    auto val = string((const char *) str, len);
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();

    ypc->ypc_array_index.back() += 1;
    auto &jfe = ensure_json_format_element(elf, ypc->ypc_array_index.back());
    jfe.jfe_type = external_log_format::JLF_CONSTANT;
    jfe.jfe_default_value = val;

    return 1;
}

static int create_search_table(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    auto elf = (external_log_format *) ypc->ypc_obj_stack.top();
    const intern_string_t table_name = ypc->get_path_fragment_i(2);
    string regex = string((const char *) str, len);

    elf->elf_search_tables.emplace_back(table_name, regex);

    return 1;
}


static struct json_path_container pattern_handlers = {
    yajlpp::property_handler("pattern")
        .with_synopsis("<message-regex>")
        .with_description(
            "The regular expression to match a log message and capture fields.")
        .with_min_length(1)
        .FOR_FIELD(external_log_format::pattern, p_string),
    yajlpp::property_handler("module-format")
        .with_synopsis("<bool>")
        .with_description(
            "If true, this pattern will only be used to parse message bodies "
                "of container formats, like syslog")
        .for_field(&external_log_format::pattern::p_module_format)
};

static const json_path_handler_base::enum_value_t ALIGN_ENUM[] = {
    { "left", external_log_format::json_format_element::align_t::LEFT },
    { "right", external_log_format::json_format_element::align_t::RIGHT },

    json_path_handler_base::ENUM_TERMINATOR
};

static const json_path_handler_base::enum_value_t OVERFLOW_ENUM[] = {
    { "abbrev", external_log_format::json_format_element::overflow_t::ABBREV },
    { "truncate", external_log_format::json_format_element::overflow_t::TRUNCATE },
    { "dot-dot", external_log_format::json_format_element::overflow_t::DOTDOT },

    json_path_handler_base::ENUM_TERMINATOR
};

static const json_path_handler_base::enum_value_t TRANSFORM_ENUM[] = {
    { "none", external_log_format::json_format_element::transform_t::NONE },
    { "uppercase", external_log_format::json_format_element::transform_t::UPPERCASE },
    { "lowercase", external_log_format::json_format_element::transform_t::LOWERCASE },
    { "capitalize", external_log_format::json_format_element::transform_t::CAPITALIZE },

    json_path_handler_base::ENUM_TERMINATOR
};

static struct json_path_container line_format_handlers = {
    yajlpp::property_handler("field")
        .with_synopsis("<field-name>")
        .with_description("The name of the field to substitute at this position")
        .with_min_length(1)
        .FOR_FIELD(external_log_format::json_format_element, jfe_value),

    yajlpp::property_handler("default-value")
        .with_synopsis("<string>")
        .with_description("The default value for this position if the field is null")
        .FOR_FIELD(external_log_format::json_format_element, jfe_default_value),

    yajlpp::property_handler("timestamp-format")
        .with_synopsis("<string>")
        .with_min_length(1)
        .with_description("The strftime(3) format for this field")
        .FOR_FIELD(external_log_format::json_format_element, jfe_ts_format),

    yajlpp::property_handler("min-width")
        .with_min_value(0)
        .with_synopsis("<size>")
        .with_description("The minimum width of the field")
        .FOR_FIELD(external_log_format::json_format_element, jfe_min_width),

    yajlpp::property_handler("max-width")
        .with_min_value(0)
        .with_synopsis("<size>")
        .with_description("The maximum width of the field")
        .FOR_FIELD(external_log_format::json_format_element, jfe_max_width),

    yajlpp::property_handler("align")
        .with_synopsis("left|right")
        .with_description("Align the text in the column to the left or right side")
        .with_enum_values(ALIGN_ENUM)
        .FOR_FIELD(external_log_format::json_format_element, jfe_align),

    yajlpp::property_handler("overflow")
        .with_synopsis("abbrev|truncate|dot-dot")
        .with_description("Overflow style")
        .with_enum_values(OVERFLOW_ENUM)
        .FOR_FIELD(external_log_format::json_format_element, jfe_overflow),

    yajlpp::property_handler("text-transform")
        .with_synopsis("none|uppercase|lowercase|capitalize")
        .with_description("Text transformation")
        .with_enum_values(TRANSFORM_ENUM)
        .FOR_FIELD(external_log_format::json_format_element, jfe_text_transform)
};

static const json_path_handler_base::enum_value_t KIND_ENUM[] = {
    {"string", value_kind_t::VALUE_TEXT},
    {"integer", value_kind_t::VALUE_INTEGER},
    {"float", value_kind_t::VALUE_FLOAT},
    {"boolean", value_kind_t::VALUE_BOOLEAN},
    {"json", value_kind_t::VALUE_JSON},
    {"struct", value_kind_t::VALUE_STRUCT},
    {"quoted", value_kind_t::VALUE_QUOTED},
    {"xml", value_kind_t::VALUE_XML},

    json_path_handler_base::ENUM_TERMINATOR
};

static const json_path_handler_base::enum_value_t SCALE_OP_ENUM[] = {
    {"identity", scale_op_t::SO_IDENTITY},
    {"multiply", scale_op_t::SO_MULTIPLY},
    {"divide", scale_op_t::SO_DIVIDE},

    json_path_handler_base::ENUM_TERMINATOR
};

static struct json_path_container scaling_factor_handlers = {
    yajlpp::pattern_property_handler("op")
        .with_enum_values(SCALE_OP_ENUM)
        .FOR_FIELD(scaling_factor, sf_op),

    yajlpp::pattern_property_handler("value")
        .FOR_FIELD(scaling_factor, sf_value)
};

static struct json_path_container scale_handlers = {
    yajlpp::pattern_property_handler("(?<scale>[^/]+)")
        .with_obj_provider(scaling_factor_provider)
        .with_children(scaling_factor_handlers),
};

static struct json_path_container unit_handlers = {
    yajlpp::property_handler("field")
        .with_synopsis("<field-name>")
        .with_description("The name of the field that contains the units for this field")
        .FOR_FIELD(external_log_format::value_def, vd_unit_field),

    yajlpp::property_handler("scaling-factor")
        .with_description("Transforms the numeric value by the given factor")
        .with_children(scale_handlers),
};

static struct json_path_container value_def_handlers = {
    yajlpp::property_handler("kind")
        .with_synopsis("string|integer|float|boolean|json|quoted")
        .with_description("The type of data in the field")
        .with_enum_values(KIND_ENUM)
        .for_field(&external_log_format::value_def::vd_meta,
                   &logline_value_meta::lvm_kind),

    yajlpp::property_handler("collate")
        .with_synopsis("<function>")
        .with_description("The collating function to use for this column")
        .FOR_FIELD(external_log_format::value_def, vd_collate),

    yajlpp::property_handler("unit")
        .with_description("Unit definitions for this field")
        .with_children(unit_handlers),

    yajlpp::property_handler("identifier")
        .with_synopsis("<bool>")
        .with_description("Indicates whether or not this field contains an identifier that should be highlighted")
        .for_field(&external_log_format::value_def::vd_meta,
                   &logline_value_meta::lvm_identifier),

    yajlpp::property_handler("foreign-key")
        .with_synopsis("<bool>")
        .with_description("Indicates whether or not this field should be treated as a foreign key for row in another table")
        .for_field(&external_log_format::value_def::vd_foreign_key),

    yajlpp::property_handler("hidden")
        .with_synopsis("<bool>")
        .with_description("Indicates whether or not this field should be hidden")
        .for_field(&external_log_format::value_def::vd_meta,
                   &logline_value_meta::lvm_hidden),

    yajlpp::property_handler("action-list#")
        .with_synopsis("<string>")
        .with_description("Actions to execute when this field is clicked on")
        .FOR_FIELD(external_log_format::value_def, vd_action_list),

    yajlpp::property_handler("rewriter")
        .with_synopsis("<command>")
        .with_description("A command that will rewrite this field when pretty-printing")
        .FOR_FIELD(external_log_format::value_def, vd_rewriter),

    yajlpp::property_handler("description")
        .with_synopsis("<string>")
        .with_description("A description of the field")
        .FOR_FIELD(external_log_format::value_def, vd_description)
};

static struct json_path_container highlighter_def_handlers = {
    yajlpp::property_handler("pattern")
        .with_synopsis("<regex>")
        .with_description("A regular expression to highlight in logs of this format.")
        .FOR_FIELD(external_log_format::highlighter_def, hd_pattern),

    yajlpp::property_handler("color")
        .with_synopsis("#<hex>|<name>")
        .with_description("The color to use when highlighting this pattern.")
        .FOR_FIELD(external_log_format::highlighter_def, hd_color),

    yajlpp::property_handler("background-color")
        .with_synopsis("#<hex>|<name>")
        .with_description("The background color to use when highlighting this pattern.")
        .FOR_FIELD(external_log_format::highlighter_def, hd_background_color),

    yajlpp::property_handler("underline")
        .with_synopsis("<enabled>")
        .with_description("Highlight this pattern with an underline.")
        .for_field(&external_log_format::highlighter_def::hd_underline),

    yajlpp::property_handler("blink")
        .with_synopsis("<enabled>")
        .with_description("Highlight this pattern by blinking.")
        .for_field(&external_log_format::highlighter_def::hd_blink)
};

static const json_path_handler_base::enum_value_t LEVEL_ENUM[] = {
    { level_names[LEVEL_TRACE], LEVEL_TRACE },
    { level_names[LEVEL_DEBUG5], LEVEL_DEBUG5 },
    { level_names[LEVEL_DEBUG4], LEVEL_DEBUG4 },
    { level_names[LEVEL_DEBUG3], LEVEL_DEBUG3 },
    { level_names[LEVEL_DEBUG2], LEVEL_DEBUG2 },
    { level_names[LEVEL_DEBUG], LEVEL_DEBUG },
    { level_names[LEVEL_INFO], LEVEL_INFO },
    { level_names[LEVEL_STATS], LEVEL_STATS },
    { level_names[LEVEL_NOTICE], LEVEL_NOTICE },
    { level_names[LEVEL_WARNING], LEVEL_WARNING },
    { level_names[LEVEL_ERROR], LEVEL_ERROR },
    { level_names[LEVEL_CRITICAL], LEVEL_CRITICAL },
    { level_names[LEVEL_FATAL], LEVEL_FATAL },

    json_path_handler_base::ENUM_TERMINATOR
};

static struct json_path_container sample_handlers = {
    yajlpp::property_handler("line")
        .with_synopsis("<log-line>")
        .with_description("A sample log line that should match a pattern in this format.")
        .FOR_FIELD(external_log_format::sample, s_line),

    yajlpp::property_handler("level")
        .with_enum_values(LEVEL_ENUM)
        .with_description("The expected level for this sample log line.")
        .FOR_FIELD(external_log_format::sample, s_level)
};

static const json_path_handler_base::enum_value_t TYPE_ENUM[] = {
    { "text", external_log_format::elf_type_t::ELF_TYPE_TEXT },
    { "json", external_log_format::elf_type_t::ELF_TYPE_JSON },
    { "csv", external_log_format::elf_type_t::ELF_TYPE_CSV },

    json_path_handler_base::ENUM_TERMINATOR
};

static struct json_path_container regex_handlers = {
    yajlpp::pattern_property_handler(R"((?<pattern_name>[^/]+))")
        .with_description("The set of patterns used to match log messages")
        .with_obj_provider(pattern_provider)
        .with_children(pattern_handlers),
};

static struct json_path_container level_handlers = {
    yajlpp::pattern_property_handler(
        "(?<level>trace|debug[2345]?|info|stats|notice|warning|error|critical|fatal)")
        .add_cb(read_levels)
        .add_cb(read_level_int)
        .with_synopsis("<pattern|integer>")
        .with_description("The regular expression used to match the log text for this level.  "
                          "For JSON logs with numeric levels, this should be the number for the corresponding level.")
};

static struct json_path_container value_handlers = {
    yajlpp::pattern_property_handler("(?<value_name>[^/]+)")
        .with_description("The set of values captured by the log message patterns")
        .with_obj_provider(value_def_provider)
        .with_children(value_def_handlers)
};

static struct json_path_container highlight_handlers = {
    yajlpp::pattern_property_handler(R"((?<highlight_name>[^/]+))")
        .with_description("The definition of a highlight")
        .with_obj_provider<external_log_format::highlighter_def, external_log_format>([](const yajlpp_provider_context &ypc, external_log_format *root) {
            return &(root->elf_highlighter_patterns[ypc.get_substr_i(0)]);
        })
        .with_children(highlighter_def_handlers)
};

static struct json_path_container action_def_handlers = {
    json_path_handler("label", read_action_def),
    json_path_handler("capture-output", read_action_bool),
    json_path_handler("cmd#", read_action_cmd)
};

static struct json_path_container action_handlers = {
    json_path_handler(pcrepp("(?<action_name>\\w+)"), read_action_def)
        .with_children(action_def_handlers)
};

static struct json_path_container search_table_def_handlers = {
    json_path_handler("pattern", create_search_table)
        .with_synopsis("<regex>")
        .with_description("The regular expression for this search table."),
};

static struct json_path_container search_table_handlers = {
    yajlpp::pattern_property_handler("\\w+")
        .with_description("The set of search tables to be automatically defined")
        .with_children(search_table_def_handlers)
};

static const json_path_handler_base::enum_value_t MIME_TYPE_ENUM[] = {
    { "application/vnd.tcpdump.pcap", file_format_t::FF_PCAP, },

    json_path_handler_base::ENUM_TERMINATOR
};

struct json_path_container format_handlers = {
    yajlpp::property_handler("regex")
        .with_description("The set of regular expressions used to match log messages")
        .with_children(regex_handlers),

    json_path_handler("json", read_format_bool)
        .with_description(R"(Indicates that log files are JSON-encoded (deprecated, use "file-type": "json"))"),
    json_path_handler("convert-to-local-time", read_format_bool)
        .with_description("Indicates that displayed timestamps should automatically be converted to local time"),
    json_path_handler("hide-extra", read_format_bool)
        .with_description("Specifies whether extra values in JSON logs should be displayed"),
    json_path_handler("multiline", read_format_bool)
        .with_description("Indicates that log messages can span multiple lines"),
    json_path_handler("timestamp-divisor", read_format_double)
        .add_cb(read_format_int)
        .with_synopsis("<number>")
        .with_description("The value to divide a numeric timestamp by in a JSON log."),
    json_path_handler("file-pattern", read_format_field)
        .with_description("A regular expression that restricts this format to log files with a matching name"),
    json_path_handler("mime-types#", read_format_field)
        .with_description("A list of mime-types this format should be used for")
        .with_enum_values(MIME_TYPE_ENUM),
    json_path_handler("level-field", read_format_field)
        .with_description("The name of the level field in the log message pattern"),
    json_path_handler("level-pointer", read_format_field)
        .with_description("A regular-expression that matches the JSON-pointer of the level property"),
    json_path_handler("timestamp-field", read_format_field)
        .with_description("The name of the timestamp field in the log message pattern"),
    json_path_handler("body-field", read_format_field)
        .with_description("The name of the body field in the log message pattern"),
    json_path_handler("url", pcrepp("url#?"))
        .add_cb(read_format_field)
        .with_description("A URL with more information about this log format"),
    json_path_handler("title", read_format_field)
        .with_description("The human-readable name for this log format"),
    json_path_handler("description", read_format_field)
        .with_description("A longer description of this log format"),
    json_path_handler("timestamp-format#", read_format_field)
        .with_description("An array of strptime(3)-like timestamp formats"),
    json_path_handler("module-field", read_format_field)
        .with_description("The name of the module field in the log message pattern"),
    json_path_handler("opid-field", read_format_field)
        .with_description("The name of the operation-id field in the log message pattern"),
    yajlpp::property_handler("ordered-by-time")
        .with_synopsis("<bool>")
        .with_description("Indicates that the order of messages in the file is time-based.")
        .for_field(&log_format::lf_time_ordered),
    yajlpp::property_handler("level")
        .with_description("The map of level names to patterns or integer values")
        .with_children(level_handlers),

    yajlpp::property_handler("value")
        .with_description("The set of value definitions")
        .with_children(value_handlers),

    yajlpp::property_handler("action")
        .with_children(action_handlers),
    yajlpp::property_handler("sample#")
        .with_description("An array of sample log messages to be tested against the log message patterns")
        .with_obj_provider(sample_provider)
        .with_children(sample_handlers),

    yajlpp::property_handler("line-format#")
        .with_description("The display format for JSON-encoded log messages")
        .with_obj_provider(line_format_provider)
        .add_cb(read_json_constant)
        .with_children(line_format_handlers),
    json_path_handler("search-table", create_search_table)
        .with_description("Search tables to automatically define for this log format")
        .with_children(search_table_handlers),

    yajlpp::property_handler("highlights")
        .with_description("The set of highlight definitions")
        .with_children(highlight_handlers),

    yajlpp::property_handler("file-type")
        .with_synopsis("text|json|csv")
        .with_description("The type of file that contains the log messages")
        .with_enum_values(TYPE_ENUM)
        .FOR_FIELD(external_log_format, elf_type)
};

static int read_id(yajlpp_parse_context *ypc, const unsigned char *str, size_t len)
{
    auto file_id = string((const char *) str, len);

    if (find(SUPPORTED_FORMAT_SCHEMAS.begin(),
             SUPPORTED_FORMAT_SCHEMAS.end(),
             file_id) == SUPPORTED_FORMAT_SCHEMAS.end()) {
        fprintf(stderr, "%s:%d: error: unsupported format $schema -- %s\n",
                ypc->ypc_source.c_str(), ypc->get_line_number(), file_id.c_str());
        return 0;
    }

    return 1;
}

struct json_path_container root_format_handler = json_path_container {
    json_path_handler("$schema", read_id)
        .with_synopsis("The URI of the schema for this file")
        .with_description("Specifies the type of this file"),

    yajlpp::pattern_property_handler("(?<format_name>\\w+)")
        .with_description("The definition of a log file format.")
        .with_obj_provider(ensure_format)
        .with_children(format_handlers)
}
    .with_schema_id(DEFAULT_FORMAT_SCHEMA);

static void write_sample_file()
{
    for (const auto& bsf : lnav_format_json) {
        auto sample_path = lnav::paths::dotlnav() /
            fmt::format("formats/default/{}.sample", bsf.get_name());
        auto sf = bsf.to_string_fragment();
        auto_fd sample_fd;

        if ((sample_fd = openp(sample_path,
                               O_WRONLY | O_TRUNC | O_CREAT,
                               0644)) == -1 ||
            (write(sample_fd.get(), sf.data(), sf.length()) == -1)) {
            fprintf(stderr,
                    "error:unable to write default format file: %s -- %s\n",
                    sample_path.c_str(),
                    strerror(errno));
        }
    }

    for (const auto& bsf : lnav_sh_scripts) {
        auto sh_path = lnav::paths::dotlnav() / fmt::format("formats/default/{}", bsf.get_name());
        auto sf = bsf.to_string_fragment();
        auto_fd sh_fd;

        if ((sh_fd = openp(sh_path, O_WRONLY|O_TRUNC|O_CREAT, 0755)) == -1 ||
            write(sh_fd.get(), sf.data(), sf.length()) == -1) {
            fprintf(stderr,
                    "error:unable to write default text file: %s -- %s\n",
                    sh_path.c_str(),
                    strerror(errno));
        }
    }

    for (const auto& bsf : lnav_scripts) {
        struct script_metadata meta;
        auto sf = bsf.to_string_fragment();
        auto_fd script_fd;
        char path[2048];
        struct stat st;

        extract_metadata(sf.data(), sf.length(), meta);
        snprintf(path, sizeof(path), "formats/default/%s.lnav", meta.sm_name.c_str());
        auto script_path = lnav::paths::dotlnav() / path;
        if (statp(script_path, &st) == 0 && st.st_size == sf.length()) {
            // Assume it's the right contents and move on...
            continue;
        }
        if ((script_fd = openp(script_path, O_WRONLY|O_TRUNC|O_CREAT, 0755)) == -1 ||
            write(script_fd.get(), sf.data(), sf.length()) == -1) {
            fprintf(stderr,
                    "error:unable to write default text file: %s -- %s\n",
                    script_path.c_str(),
                    strerror(errno));
        }
    }
}

static void format_error_reporter(const yajlpp_parse_context &ypc,
                                  lnav_log_level_t level,
                                  const char *msg)
{
    if (level >= lnav_log_level_t::ERROR) {
        struct userdata *ud = (userdata *) ypc.ypc_userdata;

        ud->ud_errors->push_back(msg);
    } else {
        fprintf(stderr, "warning:%s\n",  msg);
    }
}

std::vector<intern_string_t>
load_format_file(const ghc::filesystem::path &filename,
                 std::vector<string> &errors)
{
    std::vector<intern_string_t> retval;
    struct userdata ud;
    auto_fd fd;

    log_info("loading formats from file: %s", filename.c_str());
    ud.ud_format_path = filename;
    ud.ud_format_names = &retval;
    ud.ud_errors = &errors;
    yajlpp_parse_context ypc(filename, &root_format_handler);
    ypc.ypc_userdata = &ud;
    ypc.with_obj(ud);
    if ((fd = openp(filename, O_RDONLY)) == -1) {
        errors.emplace_back(fmt::format(
            "error: unable to open format file '{}' -- {}",
            filename.string(), strerror(errno)));
    }
    else {
        auto_mem<yajl_handle_t> handle(yajl_free);
        char buffer[2048];
        off_t offset = 0;
        ssize_t rc = -1;

        handle = yajl_alloc(&ypc.ypc_callbacks, nullptr, &ypc);
        ypc.with_handle(handle)
            .with_error_reporter(format_error_reporter);
        yajl_config(handle, yajl_allow_comments, 1);
        while (true) {
            rc = read(fd, buffer, sizeof(buffer));
            if (rc == 0) {
                break;
            }
            else if (rc == -1) {
                errors.push_back(fmt::format(
                    "error:{}:unable to read file -- {}",
                    filename.string(),
                    strerror(errno)));
                break;
            }
            if (offset == 0 && (rc > 2) &&
                    (buffer[0] == '#') && (buffer[1] == '!')) {
                // Turn it into a JavaScript comment.
                buffer[0] = buffer[1] = '/';
            }
            if (ypc.parse((const unsigned char *)buffer, rc) != yajl_status_ok) {
                break;
            }
            offset += rc;
        }
        if (rc == 0) {
            ypc.complete_parse();
        }
    }

    return retval;
}

static void load_from_path(const ghc::filesystem::path &path, std::vector<string> &errors)
{
    auto format_path = path / "formats/*/*.json";
    static_root_mem<glob_t, globfree> gl;

    log_info("loading formats from path: %s", format_path.c_str());
    if (glob(format_path.c_str(), 0, nullptr, gl.inout()) == 0) {
        for (int lpc = 0; lpc < (int)gl->gl_pathc; lpc++) {
            const char *base = basename(gl->gl_pathv[lpc]);

            if (startswith(base, "config.")) {
                continue;
            }

            string filename(gl->gl_pathv[lpc]);
            vector<intern_string_t> format_list;

            format_list = load_format_file(filename, errors);
            if (format_list.empty()) {
                log_warning("Empty format file: %s", filename.c_str());
            }
            else {
                for (auto iter = format_list.begin();
                     iter != format_list.end();
                     ++iter) {
                    log_info("  found format: %s", iter->get());
                }
            }
        }
    }
}

void load_formats(const std::vector<ghc::filesystem::path> &extra_paths,
                  std::vector<std::string> &errors)
{
    auto default_source = lnav::paths::dotlnav() / "default";
    yajlpp_parse_context ypc_builtin(default_source.string(), &root_format_handler);
    std::vector<intern_string_t> retval;
    struct userdata ud;
    yajl_handle handle;

    write_sample_file();

    log_debug("Loading default formats");
    for (const auto& bsf : lnav_format_json) {
        handle = yajl_alloc(&ypc_builtin.ypc_callbacks, nullptr, &ypc_builtin);
        ud.ud_format_names = &retval;
        ud.ud_errors = &errors;
        ypc_builtin
            .with_obj(ud)
            .with_handle(handle)
            .with_error_reporter(format_error_reporter)
            .ypc_userdata = &ud;
        yajl_config(handle, yajl_allow_comments, 1);
        auto sf = bsf.to_string_fragment();
        if (ypc_builtin.parse(sf) != yajl_status_ok) {
            unsigned char *msg = yajl_get_error(handle, 1,
                                                (const unsigned char *) sf.data(),
                                                sf.length());

            errors.push_back(fmt::format(
                FMT_STRING("builtin: invalid json -- {}"), msg));
            yajl_free_error(handle, msg);
        }
        ypc_builtin.complete_parse();
        yajl_free(handle);
    }

    for (const auto & extra_path : extra_paths) {
        load_from_path(extra_path, errors);
    }

    uint8_t mod_counter = 0;

    vector<std::shared_ptr<external_log_format>> alpha_ordered_formats;
    for (auto iter = LOG_FORMATS.begin(); iter != LOG_FORMATS.end(); ++iter) {
        auto& elf = iter->second;
        elf->build(errors);

        if (elf->elf_has_module_format) {
            mod_counter += 1;
            elf->lf_mod_index = mod_counter;
        }

        for (auto & check_iter : LOG_FORMATS) {
            if (iter->first == check_iter.first) {
                continue;
            }

            auto& check_elf = check_iter.second;
            if (elf->match_samples(check_elf->elf_samples)) {
                log_warning("Format collision, format '%s' matches sample from '%s'",
                        elf->get_name().get(),
                        check_elf->get_name().get());
                elf->elf_collision.push_back(check_elf->get_name());
            }
        }

        if (errors.empty()) {
            alpha_ordered_formats.push_back(elf);
        }
    }

    if (!errors.empty()) {
        return;
    }

    auto& graph_ordered_formats = external_log_format::GRAPH_ORDERED_FORMATS;

    while (!alpha_ordered_formats.empty()) {
        vector<intern_string_t> popped_formats;

        for (auto iter = alpha_ordered_formats.begin();
             iter != alpha_ordered_formats.end();) {
            auto elf = *iter;
            if (elf->elf_collision.empty()) {
                iter = alpha_ordered_formats.erase(iter);
                popped_formats.push_back(elf->get_name());
                graph_ordered_formats.push_back(elf);
            }
            else {
                ++iter;
            }
        }

        if (popped_formats.empty() && !alpha_ordered_formats.empty()) {
            bool broke_cycle = false;

            log_warning("Detected a cycle...");
            for (const auto& elf : alpha_ordered_formats) {
                if (elf->elf_builtin_format) {
                    log_warning("  Skipping builtin format -- %s",
                                elf->get_name().get());
                } else {
                    log_warning("  Breaking cycle by picking -- %s",
                                elf->get_name().get());
                    elf->elf_collision.clear();
                    broke_cycle = true;
                    break;
                }
            }
            if (!broke_cycle) {
                alpha_ordered_formats.front()->elf_collision.clear();
            }
        }

        for (const auto& elf : alpha_ordered_formats) {
            for (auto & popped_format : popped_formats) {
                elf->elf_collision.remove(popped_format);
            }
        }
    }

    log_info("Format order:")
    for (auto & graph_ordered_format : graph_ordered_formats) {
        log_info("  %s", graph_ordered_format->get_name().get());
    }

    auto &roots = log_format::get_root_formats();
    auto iter = std::find_if(roots.begin(), roots.end(), [](const auto& elem) {
        return elem->get_name() == "generic_log";
    });
    roots.insert(iter, graph_ordered_formats.begin(), graph_ordered_formats.end());
}

static void exec_sql_in_path(sqlite3 *db, const ghc::filesystem::path &path, std::vector<string> &errors)
{
    auto format_path = path / "formats/*/*.sql";
    static_root_mem<glob_t, globfree> gl;

    log_info("executing SQL files in path: %s", format_path.c_str());
    if (glob(format_path.c_str(), 0, nullptr, gl.inout()) == 0) {
        for (int lpc = 0; lpc < (int)gl->gl_pathc; lpc++) {
            auto filename = ghc::filesystem::path(gl->gl_pathv[lpc]);
            auto read_res = read_file(filename);

            if (read_res.isOk()) {
                log_info("Executing SQL file: %s", filename.c_str());
                auto content = read_res.unwrap();

                sql_execute_script(db, filename.c_str(), content.c_str(), errors);
            }
            else {
                errors.push_back(fmt::format(
                    "error:unable to read file '{}' -- {}",
                    filename.string(), read_res.unwrapErr()));
            }
        }
    }
}

void load_format_extra(sqlite3 *db,
                       const std::vector<ghc::filesystem::path> &extra_paths,
                       std::vector<std::string> &errors)
{
    for (const auto & extra_path : extra_paths) {
        exec_sql_in_path(db, extra_path, errors);
    }
}

static void extract_metadata(const char *contents, size_t len, struct script_metadata &meta_out)
{
    static const pcrepp SYNO_RE("^#\\s+@synopsis:(.*)$", PCRE_MULTILINE);
    static const pcrepp DESC_RE("^#\\s+@description:(.*)$", PCRE_MULTILINE);

    pcre_input pi(contents, 0, len);
    pcre_context_static<16> pc;

    pi.reset(contents, 0, len);
    if (SYNO_RE.match(pc, pi)) {
        meta_out.sm_synopsis = trim(pi.get_substr(pc[0]));
    }
    pi.reset(contents, 0, len);
    if (DESC_RE.match(pc, pi)) {
        meta_out.sm_description = trim(pi.get_substr(pc[0]));
    }

    if (!meta_out.sm_synopsis.empty()) {
        size_t space = meta_out.sm_synopsis.find(' ');

        if (space == string::npos) {
            space = meta_out.sm_synopsis.size();
        }
        meta_out.sm_name = meta_out.sm_synopsis.substr(0, space);
    }
}

void extract_metadata_from_file(struct script_metadata &meta_inout)
{
    char buffer[8 * 1024];
    auto_mem<FILE> fp(fclose);
    struct stat st;

    if (statp(meta_inout.sm_path, &st) == -1) {
        log_warning("unable to open script -- %s", meta_inout.sm_path.c_str());
    } else if (!S_ISREG(st.st_mode)) {
        log_warning("not a regular file -- %s", meta_inout.sm_path.c_str());
    } else if ((fp = fopen(meta_inout.sm_path.c_str(), "r")) != nullptr) {
        size_t len;

        len = fread(buffer, 1, sizeof(buffer), fp.in());
        extract_metadata(buffer, len, meta_inout);
    }
}

static void find_format_in_path(const ghc::filesystem::path &path,
                                available_scripts& scripts)
{
    auto format_path = path / "formats/*/*.lnav";
    static_root_mem<glob_t, globfree> gl;

    log_debug("Searching for script in path: %s", format_path.c_str());
    if (glob(format_path.c_str(), 0, nullptr, gl.inout()) == 0) {
        for (int lpc = 0; lpc < (int)gl->gl_pathc; lpc++) {
            const char *filename = basename(gl->gl_pathv[lpc]);
            string script_name = string(filename, strlen(filename) - 5);
            struct script_metadata meta;

            meta.sm_path = gl->gl_pathv[lpc];
            meta.sm_name = script_name;
            extract_metadata_from_file(meta);
            scripts.as_scripts[script_name].push_back(meta);

            log_debug("  found script: %s", meta.sm_path.c_str());
        }
    }
}

void find_format_scripts(const vector<ghc::filesystem::path> &extra_paths,
                         available_scripts& scripts)
{
    for (const auto &extra_path : extra_paths) {
        find_format_in_path(extra_path, scripts);
    }
}

void load_format_vtabs(log_vtab_manager *vtab_manager,
                       std::vector<std::string> &errors)
{
    auto &root_formats = LOG_FORMATS;

    for (auto & root_format : root_formats) {
        root_format.second->register_vtabs(vtab_manager, errors);
    }
}
