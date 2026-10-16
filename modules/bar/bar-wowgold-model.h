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

#ifndef BAR_WOWGOLD_MODEL_H
#define BAR_WOWGOLD_MODEL_H

#include <glib.h>

G_BEGIN_DECLS

/*
 * The pure half of the `wow-gold' widget: everything that turns
 * tsmctl's JSON into numbers and numbers into text, and the argv that
 * asks tsmctl for them.
 *
 * Nothing here spawns, touches a plugin, a theme or the compositor,
 * so tests/test-bar-wow-gold.c compiles this file straight in and
 * feeds it the exact document that went wrong.
 *
 * Money is carried as a signed 64-bit count of COPPER throughout,
 * because that is what tsmctl's JSON gives (its `raw' form is always
 * integer copper, whatever --money says) and because a WoW gold cap of
 * ten million gold is 10^11 copper: past 32 bits, well inside 64.
 */

/* The domain of the errors bar_wowgold_build_argv() sets; there is
   one code, 0: a setting that cannot be passed to tsmctl safely. */
#define BAR_WOWGOLD_ERROR (g_quark_from_static_string("bar-wowgold-error"))

/* One gold is 100 silver is 10000 copper. */
#define BAR_WOWGOLD_COPPER_PER_GOLD   (10000)
#define BAR_WOWGOLD_COPPER_PER_SILVER (100)

/* Longest string taken from tsmctl into a label or a panel row, in
   characters.  An item name is never near it; a corrupt document can
   be arbitrarily long. */
#define BAR_WOWGOLD_TEXT_MAX (160)

/* Rows read from any one table.  `get sales' with no limit is every
   sale ever made (nearly two thousand on a modest account), and a
   panel shows ten. */
#define BAR_WOWGOLD_ROWS_MAX (4096)

/**
 * BarWowgoldMoneyFormat:
 * @BAR_WOWGOLD_MONEY_G: whole gold with thousands separators, "123,456g"
 * @BAR_WOWGOLD_MONEY_GSC: gold, silver and copper, "123,456g 12s 34c"
 *
 * How an amount is written in the panel and the tooltip.
 */
typedef enum {
	BAR_WOWGOLD_MONEY_G = 0,
	BAR_WOWGOLD_MONEY_GSC
} BarWowgoldMoneyFormat;

/**
 * BarWowgoldDirection:
 * @BAR_WOWGOLD_FLAT: no change, or no history to tell
 * @BAR_WOWGOLD_UP: more gold than yesterday
 * @BAR_WOWGOLD_DOWN: less gold than yesterday
 *
 * Which way the day went; picks the label's arrow and colour.
 */
typedef enum {
	BAR_WOWGOLD_FLAT = 0,
	BAR_WOWGOLD_UP,
	BAR_WOWGOLD_DOWN
} BarWowgoldDirection;

/**
 * BarWowgoldCommand:
 * @BAR_WOWGOLD_CMD_GOLD: `get gold' -- gold per character, and the total
 * @BAR_WOWGOLD_CMD_HISTORY: `get gold-history --by=day' -- daily totals
 * @BAR_WOWGOLD_CMD_SALES: `get sales' -- the ledger's sales, newest first
 * @BAR_WOWGOLD_CMD_PNL: `pnl --period=today' -- today's profit and loss
 * @BAR_WOWGOLD_CMD_ATTENTION: `attention' -- characters that need a login
 * @BAR_WOWGOLD_CMD_REPORT: `report' -- the full text report, for a terminal
 *
 * The tsmctl questions the widget asks.
 */
typedef enum {
	BAR_WOWGOLD_CMD_GOLD = 0,
	BAR_WOWGOLD_CMD_HISTORY,
	BAR_WOWGOLD_CMD_SALES,
	BAR_WOWGOLD_CMD_PNL,
	BAR_WOWGOLD_CMD_ATTENTION,
	BAR_WOWGOLD_CMD_REPORT
} BarWowgoldCommand;

/**
 * BarWowgoldArgs:
 * @command: (nullable): the `tsmctl-command' setting; split with shell
 *   quoting rules (never run through a shell), `~/' expanded.  %NULL or
 *   empty means "tsmctl".
 * @host: (nullable): empty for tsmctl's own config, "local" for
 *   --local, anything else for --host
 * @realm: (nullable): comma-separated realms, one --realm each
 * @character: (nullable): comma-separated characters, one --char each
 * @account: (nullable): comma-separated logins, one --account each
 * @flavor: (nullable): a game flavor directory, e.g. "_classic_"
 * @offline: pass --offline (ignored for "local", where it means nothing)
 * @period: (nullable): the gold-history window, e.g. "30d"
 * @sales_count: how many sales to ask for
 *
 * Everything a tsmctl argv is built from.
 */
typedef struct {
	const gchar *command;
	const gchar *host;
	const gchar *realm;
	const gchar *character;
	const gchar *account;
	const gchar *flavor;
	gboolean     offline;
	const gchar *period;
	guint        sales_count;
} BarWowgoldArgs;

/**
 * BarWowgoldChar:
 * @name: the owner -- a character, "Warbank", or a guild bank
 * @realm: (nullable): its realm; empty for a warbank
 * @kind: (nullable): tsmctl's kind: character, warbank, guild (log)
 * @copper: gold on hand, in copper
 */
typedef struct {
	gchar  *name;
	gchar  *realm;
	gchar  *kind;
	gint64  copper;
} BarWowgoldChar;

/**
 * BarWowgoldPoint:
 * @day: a Julian day number (GDate's), local time
 * @copper: the account-wide total at the end of that day
 */
typedef struct {
	guint32 day;
	gint64  copper;
} BarWowgoldPoint;

/**
 * BarWowgoldSale:
 * @item: the item's name (or its item string when it has none)
 * @character: (nullable): who sold it
 * @realm: (nullable): where
 * @source: (nullable): Auction, Vendor, Trade ...
 * @qty: how many
 * @price: per unit, in copper
 * @total: the whole sale, in copper
 * @when: unix seconds, 0 when the row carried no readable time
 */
typedef struct {
	gchar  *item;
	gchar  *character;
	gchar  *realm;
	gchar  *source;
	gint64  qty;
	gint64  price;
	gint64  total;
	gint64  when;
} BarWowgoldSale;

/**
 * BarWowgoldAttention:
 * @what: tsmctl's reason, e.g. "auctions expired, collect"
 * @character: (nullable): who needs the login
 * @realm: (nullable): where
 * @count: how many things
 * @value: what they are worth, in copper (0 when unknown)
 */
typedef struct {
	gchar  *what;
	gchar  *character;
	gchar  *realm;
	gint64  count;
	gint64  value;
} BarWowgoldAttention;

/**
 * BarWowgoldPnl:
 * @revenue: sales income today, after the AH cut
 * @purchases: what was bought today
 * @expenses: postage, deposits, repairs ...
 * @net: tsmctl's "Net cash flow"
 * @units: units sold
 */
typedef struct {
	gint64 revenue;
	gint64 purchases;
	gint64 expenses;
	gint64 net;
	gint64 units;
} BarWowgoldPnl;

/**
 * BarWowgoldData:
 * @have_total: whether @total was read
 * @total: every owner's gold summed, in copper
 * @chars: (element-type BarWowgoldChar): richest first
 * @points: (element-type BarWowgoldPoint): one per day, oldest first
 * @sales: (element-type BarWowgoldSale): newest first
 * @have_pnl: whether @pnl was read
 * @pnl: today's profit and loss
 * @have_attention: whether `attention' answered at all
 * @attention: (element-type BarWowgoldAttention): most urgent first
 * @sales_listed: how many sales tsmctl says there are in all
 * @fetched_at: unix seconds the data was read
 *
 * One complete reading.  The worker builds a fresh one and swaps it in
 * under the widget's lock; nothing ever edits one in place, so a panel
 * reading the old one cannot see it change underneath it.
 */
typedef struct {
	gboolean   have_total;
	gint64     total;
	GPtrArray *chars;
	GArray    *points;
	GPtrArray *sales;
	gboolean   have_pnl;
	BarWowgoldPnl pnl;
	gboolean   have_attention;
	GPtrArray *attention;
	gint64     sales_listed;
	gint64     fetched_at;
} BarWowgoldData;

/**
 * BarWowgoldTrend:
 * @have_day: whether @day_delta is known (needs today and yesterday)
 * @day_delta: today minus yesterday, in copper
 * @have_week: whether @week_delta is known
 * @week_delta: today minus seven days ago
 * @have_period: whether @period_delta is known
 * @period_delta: today minus the first day of the window
 * @have_today: whether @today is known
 * @today: the latest total in the history
 * @have_yesterday: whether @yesterday is known
 * @yesterday: yesterday's closing total
 * @n_samples: entries in @samples (the window, in days)
 * @samples: (array length=n_samples): 0.0--1.0 per day, for a graph
 *
 * What the history says, measured against one particular "today".
 */
typedef struct {
	gboolean have_day;
	gint64   day_delta;
	gboolean have_week;
	gint64   week_delta;
	gboolean have_period;
	gint64   period_delta;
	gboolean have_today;
	gint64   today;
	gboolean have_yesterday;
	gint64   yesterday;
	guint    n_samples;
	gdouble *samples;
} BarWowgoldTrend;

/* --- Money ---------------------------------------------------------- */

BarWowgoldMoneyFormat bar_wowgold_money_format_parse (const gchar *text);

gchar   *bar_wowgold_format_money   (gint64                copper,
                                     BarWowgoldMoneyFormat format);
gchar   *bar_wowgold_format_compact (gint64 copper);
gchar   *bar_wowgold_format_delta   (gint64                copper,
                                     BarWowgoldMoneyFormat format,
                                     gboolean              compact);
gboolean bar_wowgold_parse_money    (const gchar *text,
                                     gint64      *copper);

/* --- JSON ----------------------------------------------------------- */

gchar   *bar_wowgold_json_member  (const gchar *object, const gchar *key);
gboolean bar_wowgold_json_int     (const gchar *object, const gchar *key,
                                   gint64      *out);
gchar   *bar_wowgold_json_text    (const gchar *object, const gchar *key);
gchar   *bar_wowgold_json_decode  (const gchar *token);

/**
 * BarWowgoldJsonFunc:
 * @element: one array element's raw text
 * @index: its position, counting from zero
 * @user_data: what the caller passed
 *
 * Returns: %FALSE to stop
 */
typedef gboolean (*BarWowgoldJsonFunc) (const gchar *element,
                                        guint        index,
                                        gpointer     user_data);

guint    bar_wowgold_json_foreach (const gchar        *array,
                                   BarWowgoldJsonFunc  func,
                                   gpointer            user_data);

/* --- Documents ------------------------------------------------------ */

BarWowgoldData *bar_wowgold_data_new  (void);
void            bar_wowgold_data_free (BarWowgoldData *data);

gboolean bar_wowgold_parse_gold      (BarWowgoldData *data, const gchar *json);
gboolean bar_wowgold_parse_history   (BarWowgoldData *data, const gchar *json);
gboolean bar_wowgold_parse_sales     (BarWowgoldData *data, const gchar *json);
gboolean bar_wowgold_parse_pnl       (BarWowgoldData *data, const gchar *json);
gboolean bar_wowgold_parse_attention (BarWowgoldData *data, const gchar *json);

/* --- Days and trends ------------------------------------------------ */

guint32  bar_wowgold_julian      (const gchar *iso_date);
guint    bar_wowgold_period_days (const gchar *period);
guint    bar_wowgold_daily_series (const BarWowgoldPoint *points,
                                   guint                  n_points,
                                   guint32                today,
                                   guint                  days,
                                   gint64                *values,
                                   gboolean              *known);
void     bar_wowgold_normalise   (const gint64   *values,
                                  const gboolean *known,
                                  guint           n,
                                  gdouble        *out);
void     bar_wowgold_trend       (const BarWowgoldData *data,
                                  guint32               today,
                                  guint                 days,
                                  BarWowgoldTrend      *out);
void     bar_wowgold_trend_clear (BarWowgoldTrend *trend);
BarWowgoldDirection bar_wowgold_direction (const BarWowgoldTrend *trend);

/* --- Text ----------------------------------------------------------- */

gchar *bar_wowgold_format_ago (gint64 then, gint64 now);
gchar *bar_wowgold_label      (const BarWowgoldData  *data,
                               const BarWowgoldTrend *trend,
                               gboolean               compact,
                               gboolean               show_delta,
                               BarWowgoldMoneyFormat  format);
gchar *bar_wowgold_tooltip    (const BarWowgoldData  *data,
                               const BarWowgoldTrend *trend,
                               BarWowgoldMoneyFormat  format,
                               gint64                 now);
gchar *bar_wowgold_summary    (const BarWowgoldData  *data,
                               const BarWowgoldTrend *trend,
                               BarWowgoldMoneyFormat  format);
gchar *bar_wowgold_sale_title (const BarWowgoldSale *sale);

/* --- argv ----------------------------------------------------------- */

gboolean bar_wowgold_host_ok   (const gchar *host);
gboolean bar_wowgold_arg_ok    (const gchar *value);
gchar  **bar_wowgold_build_argv (const BarWowgoldArgs *args,
                                 BarWowgoldCommand     command,
                                 GError              **error);
gchar   *bar_wowgold_fetch_key (const BarWowgoldArgs *args);

G_END_DECLS

#endif /* BAR_WOWGOLD_MODEL_H */
