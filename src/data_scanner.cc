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
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "data_scanner.hh"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "config.h"
#include "pcrepp/pcrepp.hh"

static struct {
    const char* name;
    pcrepp pcre;
} MATCHERS[DT_TERMINAL_MAX] = {
    {
        "quot",
        pcrepp("\\A(?:(?:u|r)?\"((?:\\\\.|[^\"])+)\"|"
               "(?:u|r)?'((?:\\\\.|[^'])+)')"),
    },
    {
        "url",
        pcrepp("\\A([\\w]+://[^\\s'\"\\[\\](){}]+[/a-zA-Z0-9\\-=&])"),
    },
    {
        "path",
        pcrepp("\\A((?:/|\\./|\\.\\./)[\\w\\.\\-_\\~/]*)"),
    },
    {
        "mac",
        pcrepp(
            "\\A([0-9a-fA-F][0-9a-fA-F](?::[0-9a-fA-F][0-9a-fA-F]){5})(?!:)"),
    },
    {
        "date",
        pcrepp("\\A("
               "\\d{4}/\\d{1,2}/\\d{1,2}|"
               "\\d{4}-\\d{1,2}-\\d{1,2}|"
               "\\d{2}/\\w{3}/\\d{4}"
               ")T?"),
    },
    {
        "time",
        pcrepp("\\A([\\s\\d]\\d:\\d\\d(?:(?!:\\d)|:\\d\\d(?:[\\.,]\\d{3,6})?Z?)"
               ")\\b"),
    },
    /* { "qual", pcrepp("\\A([^\\s:=]+:[^\\s:=,]+(?!,)(?::[^\\s:=,]+)*)"), }, */
    {
        "ipv6",
        pcrepp("\\A(::|[:\\da-fA-F\\.]+[a-fA-F\\d](?:%\\w+)?)"),
    },
    {
        "hexd",
        pcrepp("\\A([0-9a-fA-F][0-9a-fA-F](?::[0-9a-fA-F][0-9a-fA-F])+)"),
    },

    {
        "xmld",
        pcrepp("\\A(<!\\??[\\w:]+\\s*(?:[\\w:]+(?:\\s*=\\s*"
               "(?:\"((?:\\\\.|[^\"])+)\"|'((?:\\\\.|[^'])+)'|[^>]+)"
               "))*\\s*>)"),
    },
    {
        "xmlt",
        pcrepp("\\A(<\\??[\\w:]+\\s*(?:[\\w:]+(?:\\s*=\\s*"
               "(?:\"((?:\\\\.|[^\"])+)\"|'((?:\\\\.|[^'])+)'|[^>]+)"
               "))*\\s*(?:/|\\?)>)"),
    },
    {
        "xmlo",
        pcrepp("\\A(<[\\w:]+\\s*(?:[\\w:]+(?:\\s*=\\s*"
               "(?:\"((?:\\\\.|[^\"])+)\"|'((?:\\\\.|[^'])+)'|[^>]+)"
               "))*\\s*>)"),
    },

    {
        "xmlc",
        pcrepp("\\A(</[\\w:]+\\s*>)"),
    },

    {
        "h1",
        pcrepp("\\A([A-Z \\-])"),
    },
    {
        "h2",
        pcrepp("\\A([A-Z \\-])"),
    },
    {
        "h3",
        pcrepp("\\A([A-Z \\-])"),
    },

    {
        "coln",
        pcrepp("\\A(:)"),
    },
    {
        "eq",
        pcrepp("\\A(=)"),
    },
    {
        "comm",
        pcrepp("\\A(,)"),
    },
    {
        "semi",
        pcrepp("\\A(;)"),
    },

    {
        "empt",
        pcrepp("\\A(\\(\\)|\\{\\}|\\[\\])"),
    },

    {
        "lcurly",
        pcrepp("\\A({)"),
    },
    {
        "rcurly",
        pcrepp("\\A(})"),
    },

    {
        "lsquare",
        pcrepp("\\A(\\[)"),
    },
    {
        "rsquare",
        pcrepp("\\A(\\])"),
    },

    {
        "lparen",
        pcrepp("\\A(\\()"),
    },
    {
        "rparen",
        pcrepp("\\A(\\))"),
    },

    {
        "langle",
        pcrepp("\\A(\\<)"),
    },
    {
        "rangle",
        pcrepp("\\A(\\>)"),
    },

    {
        "ipv4",
        pcrepp("\\A("
               "(?:(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])\\.){3}"
               "(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])(?![\\d]))"),
    },

    {
        "uuid",
        pcrepp("\\A([0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})"),
    },

    {
        "vers",
        pcrepp("\\A("
               "[0-9]+(?:\\.[0-9]+\\w*){2,}(?:-\\w+)?|"
               "[0-9]+(?:\\.[0-9]+\\w*)+(?<!\\d[eE])-\\w+?"
               ")\\b"),
    },
    {
        "oct",
        pcrepp("\\A(-?0[0-7]+\\b)"),
    },
    {
        "pcnt",
        pcrepp("\\A(-?[0-9]+(\\.[0-9]+)?[ ]*%\\b)"),
    },
    {
        "num",
        pcrepp("\\A(-?[0-9]+(\\.[0-9]+)?([eE][\\-+][0-9]+)?)"
               "\\b(?![\\._\\-][a-zA-Z])"),
    },
    {
        "hex",
        pcrepp("\\A(-?(?:0x|[0-9])[0-9a-fA-F]+)"
               "\\b(?![\\._\\-][a-zA-Z])"),
    },

    {
        "mail",
        pcrepp("\\A([a-zA-Z0-9\\._%+-]+@[a-zA-Z0-9\\.-]+\\.[a-zA-Z]+)\\b"),
    },
    {"cnst", pcrepp("\\A(true|True|TRUE|false|False|FALSE|None|null)\\b")},
    {
        "word",
        pcrepp("\\A([a-zA-Z][a-z']+(?=[\\s\\(\\)!\\*:;'\\\"\\?,]|[\\.\\!,\\?]"
               "\\s|$))"),
    },
    {
        "sym",
        pcrepp(
            "\\A([^\";\\s:=,\\(\\)\\{\\}\\[\\]\\+#!@%\\^&\\*'\\?<>\\~`\\|\\\\]+"
            "(?:::[^\";\\s:=,\\(\\)\\{\\}\\[\\]\\+#!@%\\^&\\*'\\?<>\\~`\\|\\\\]"
            "+)*)"),
    },
    {
        "line",
        pcrepp("\\A(\r?\n|\r|;)"),
    },
    {
        "wspc",
        pcrepp("\\A([ \\r\\t\\n]+)"),
    },
    {
        "dot",
        pcrepp("\\A(\\.)"),
    },
    {
        "escc",
        pcrepp("\\A(\\\\\\.)"),
    },

    {
        "gbg",
        pcrepp("\\A(.)"),
    },
};

const char* DNT_NAMES[DNT_MAX - DNT_KEY] = {
    "key",
    "pair",
    "val",
    "row",
    "unit",
    "meas",
    "var",
    "rang",
    "dt",
    "grp",
};

const char*
data_scanner::token2name(data_token_t token)
{
    if (token < 0) {
        return "inv";
    } else if (token < DT_TERMINAL_MAX) {
        return MATCHERS[token].name;
    } else if (token == DT_ANY) {
        return "any";
    } else {
        return DNT_NAMES[token - DNT_KEY];
    }
}
