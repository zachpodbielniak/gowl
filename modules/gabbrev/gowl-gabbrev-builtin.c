/*
 * gowl - GObject Wayland Compositor
 * Copyright (C) 2026  Zach Podbielniak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * gowl-gabbrev-builtin.c - The matches gabbrev ships.
 *
 * Loaded first, so a user's match with the same trigger replaces one of
 * these, and `builtin: false' in the module settings drops them all.
 * `gowl-msg gabbrev-builtin' prints this text: it doubles as an example
 * of the format.
 *
 * Every trigger starts with `;' -- rare at the start of a word in
 * prose, and never followed by a letter in code -- and the set is
 * PREFIX-FREE: no trigger is the start of another, because a trigger
 * fires the moment its last character is typed and `;le' would make
 * `;lenny' impossible to reach.  test-gabbrev-model checks that.
 *
 * Dates use `%o', gabbrev's addition to strftime: the ordinal suffix
 * of the day (st, nd, rd, th).
 */

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "gowl-gabbrev"

#include "gowl-gabbrev-model.h"

static const gchar builtin_yaml[] =
"# gabbrev built-in matches.  Override one by giving its trigger again.\n"
"matches:\n"
"  # --- dates and times -------------------------------------------\n"
"  - triggers: [\";today\", \";date\"]\n"
"    label: \"Today's date, ISO 8601 (2026-10-19)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%Y-%m-%d\"}}]\n"
"  - trigger: \";htoday\"\n"
"    label: \"Today's date for humans (October 19th, 2026)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%B %-d%o, %Y\"}}]\n"
"  - trigger: \";mtoday\"\n"
"    label: \"Today's date for machines (20261019)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%Y%m%d\"}}]\n"
"  - trigger: \";now\"\n"
"    label: \"Date and time (2026-10-19 14:05:09)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%Y-%m-%d %H:%M:%S\"}}]\n"
"  - trigger: \";hnow\"\n"
"    label: \"Date and time for humans (October 19th, 2026 at 2:05 PM)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%B %-d%o, %Y at %-I:%M %p\"}}]\n"
"  - trigger: \";mnow\"\n"
"    label: \"Date and time for machines (20261019140509)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%Y%m%d%H%M%S\"}}]\n"
"  - trigger: \";iso\"\n"
"    label: \"ISO 8601 timestamp with offset (2026-10-19T14:05:09-04:00)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%Y-%m-%dT%H:%M:%S%:z\"}}]\n"
"  - trigger: \";time\"\n"
"    label: \"The time, 24-hour (14:05)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%H:%M\"}}]\n"
"  - trigger: \";htime\"\n"
"    label: \"The time, 12-hour (2:05 PM)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%-I:%M %p\"}}]\n"
"  - trigger: \";yesterday\"\n"
"    label: \"Yesterday's date (2026-10-18)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%Y-%m-%d\", offset: -86400}}]\n"
"  - trigger: \";tomorrow\"\n"
"    label: \"Tomorrow's date (2026-10-20)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%Y-%m-%d\", offset: 86400}}]\n"
"  - trigger: \";week\"\n"
"    label: \"ISO week (2026-W43)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%G-W%V\"}}]\n"
"  - trigger: \";dow\"\n"
"    label: \"Day of the week (Monday)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%A\"}}]\n"
"  - trigger: \";month\"\n"
"    label: \"Month and year (October 2026)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%B %Y\"}}]\n"
"  - trigger: \";epoch\"\n"
"    label: \"Unix time (1792432509)\"\n"
"    replace: \"{{d}}\"\n"
"    vars: [{name: d, type: date, params: {format: \"%s\"}}]\n"
"\n"
"  # --- faces ------------------------------------------------------\n"
"  - {trigger: \";shrug\", replace: '\xc2\xaf\\_(\xe3\x83\x84)_/\xc2\xaf'}\n"
"  - {trigger: \";tableflip\", replace: \"(\xe2\x95\xaf\xc2\xb0\xe2\x96\xa1\xc2\xb0)\xe2\x95\xaf\xef\xb8\xb5 \xe2\x94\xbb\xe2\x94\x81\xe2\x94\xbb\"}\n"
"  - {trigger: \";unflip\", replace: \"\xe2\x94\xac\xe2\x94\x80\xe2\x94\xac\xe3\x83\x8e( \xc2\xba _ \xc2\xba\xe3\x83\x8e)\"}\n"
"  - {trigger: \";lenny\", replace: \"( \xcd\xa1\xc2\xb0 \xcd\x9c\xca\x96 \xcd\xa1\xc2\xb0)\"}\n"
"  - {trigger: \";disapprove\", replace: \"\xe0\xb2\xa0_\xe0\xb2\xa0\"}\n"
"\n"
"  # --- symbols ----------------------------------------------------\n"
"  - {trigger: \";->\", replace: \"\xe2\x86\x92\"}\n"
"  - {trigger: \";<-\", replace: \"\xe2\x86\x90\"}\n"
"  - {trigger: \";=>\", replace: \"\xe2\x87\x92\"}\n"
"  - {trigger: \";<=\", replace: \"\xe2\x89\xa4\"}\n"
"  - {trigger: \";>=\", replace: \"\xe2\x89\xa5\"}\n"
"  - {trigger: \";!=\", replace: \"\xe2\x89\xa0\"}\n"
"  - {trigger: \";+-\", replace: \"\xc2\xb1\"}\n"
"  - {trigger: \";inf\", replace: \"\xe2\x88\x9e\"}\n"
"  - {trigger: \";deg\", replace: \"\xc2\xb0\"}\n"
"  - {trigger: \";em\", replace: \"\xe2\x80\x94\"}\n"
"  - {trigger: \";en\", replace: \"\xe2\x80\x93\"}\n"
"  - {trigger: \";...\", replace: \"\xe2\x80\xa6\"}\n"
"  - {trigger: \";bullet\", replace: \"\xe2\x80\xa2\"}\n"
"  - {trigger: \";check\", replace: \"\xe2\x9c\x93\"}\n"
"  - {trigger: \";cross\", replace: \"\xe2\x9c\x97\"}\n"
"  - {trigger: \";copy\", replace: \"\xc2\xa9\"}\n"
"  - {trigger: \";reg\", replace: \"\xc2\xae\"}\n"
"  - {trigger: \";tm\", replace: \"\xe2\x84\xa2\"}\n"
"  - {trigger: \";euro\", replace: \"\xe2\x82\xac\"}\n"
"\n"
"  # --- the machine and the session --------------------------------\n"
"  - trigger: \";uuid\"\n"
"    label: \"A random UUID\"\n"
"    replace: \"{{u}}\"\n"
"    vars: [{name: u, type: uuid}]\n"
"  - trigger: \";user\"\n"
"    label: \"Your login name\"\n"
"    replace: \"{{u}}\"\n"
"    vars: [{name: u, type: env, params: {name: USER}}]\n"
"  - trigger: \";host\"\n"
"    label: \"This machine's host name\"\n"
"    replace: \"{{h}}\"\n"
"    vars: [{name: h, type: shell, params: {cmd: \"hostname\"}}]\n"
"  - trigger: \";clip\"\n"
"    label: \"The clipboard, as plain text\"\n"
"    replace: \"{{c}}\"\n"
"    vars: [{name: c, type: clipboard}]\n"
"  - trigger: \";lorem\"\n"
"    label: \"Lorem ipsum\"\n"
"    replace: \"Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed "
"do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad "
"minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex "
"ea commodo consequat.\"\n"
"\n"
"  # --- grab, rewrite, put back ------------------------------------\n"
"  # The line before the cursor is selected and read (`selection'),\n"
"  # handed to a language model, and pasted over itself.  Standalone\n"
"  # gowl runs the `ai' CLI from ai-glib; cmacs --gowl replaces both\n"
"  # with its own AI (see cmacs-gowl-gabbrev.el).\n"
"  - trigger: \";reword\"\n"
"    label: \"Reword the current line with AI\"\n"
"    delete_first: true\n"
"    force_mode: clipboard\n"
"    timeout: 120000\n"
"    replace: \"{{out}}\"\n"
"    vars:\n"
"      - {name: text, type: selection, params: {scope: line}}\n"
"      - name: out\n"
"        type: shell\n"
"        params:\n"
"          timeout: 110000\n"
"          cmd: >-\n"
"            printf '%s' \"$GABBREV_TEXT\" | ai --no-expand --system\n"
"            'Reword the text you are given. Keep its meaning, language,\n"
"            register and formatting. Reply with the reworded text only.'\n"
"  - trigger: \";fix\"\n"
"    label: \"Fix spelling and grammar on the current line with AI\"\n"
"    delete_first: true\n"
"    force_mode: clipboard\n"
"    timeout: 120000\n"
"    replace: \"{{out}}\"\n"
"    vars:\n"
"      - {name: text, type: selection, params: {scope: line}}\n"
"      - name: out\n"
"        type: shell\n"
"        params:\n"
"          timeout: 110000\n"
"          cmd: >-\n"
"            printf '%s' \"$GABBREV_TEXT\" | ai --no-expand --system\n"
"            'Correct the spelling, grammar and punctuation of the text you\n"
"            are given and change nothing else. Reply with the corrected\n"
"            text only.'\n";

/**
 * gabbrev_builtin_yaml: (skip)
 *
 * Returns: (transfer none): the built-in matches, as a match file
 */
const gchar *
gabbrev_builtin_yaml(void)
{
	return builtin_yaml;
}
