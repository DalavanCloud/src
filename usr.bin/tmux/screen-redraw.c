/* $OpenBSD: screen-redraw.c,v 1.58 2019/03/16 19:12:13 nicm Exp $ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

struct screen_redraw_ctx {
	struct client	*c;

	u_int		 lines;
	int		 top;

	int		 pane_status;

	u_int		 sx;
	u_int		 sy;
	u_int		 ox;
	u_int		 oy;
};

static void	screen_redraw_draw_borders(struct screen_redraw_ctx *);
static void	screen_redraw_draw_panes(struct screen_redraw_ctx *);
static void	screen_redraw_draw_status(struct screen_redraw_ctx *);
static void	screen_redraw_draw_pane(struct screen_redraw_ctx *,
		    struct window_pane *);
static void	screen_redraw_draw_number(struct screen_redraw_ctx *,
		    struct window_pane *);

#define CELL_INSIDE 0
#define CELL_LEFTRIGHT 1
#define CELL_TOPBOTTOM 2
#define CELL_TOPLEFT 3
#define CELL_TOPRIGHT 4
#define CELL_BOTTOMLEFT 5
#define CELL_BOTTOMRIGHT 6
#define CELL_TOPJOIN 7
#define CELL_BOTTOMJOIN 8
#define CELL_LEFTJOIN 9
#define CELL_RIGHTJOIN 10
#define CELL_JOIN 11
#define CELL_OUTSIDE 12

#define CELL_BORDERS " xqlkmjwvtun~"

#define CELL_STATUS_OFF 0
#define CELL_STATUS_TOP 1
#define CELL_STATUS_BOTTOM 2

/* Check if cell is on the border of a particular pane. */
static int
screen_redraw_cell_border1(struct window_pane *wp, u_int px, u_int py)
{
	/* Inside pane. */
	if (px >= wp->xoff && px < wp->xoff + wp->sx &&
	    py >= wp->yoff && py < wp->yoff + wp->sy)
		return (0);

	/* Left/right borders. */
	if ((wp->yoff == 0 || py >= wp->yoff - 1) && py <= wp->yoff + wp->sy) {
		if (wp->xoff != 0 && px == wp->xoff - 1)
			return (1);
		if (px == wp->xoff + wp->sx)
			return (2);
	}

	/* Top/bottom borders. */
	if ((wp->xoff == 0 || px >= wp->xoff - 1) && px <= wp->xoff + wp->sx) {
		if (wp->yoff != 0 && py == wp->yoff - 1)
			return (3);
		if (py == wp->yoff + wp->sy)
			return (4);
	}

	/* Outside pane. */
	return (-1);
}

/* Check if a cell is on the pane border. */
static int
screen_redraw_cell_border(struct client *c, u_int px, u_int py)
{
	struct window		*w = c->session->curw->window;
	struct window_pane	*wp;
	int			 retval;

	/* Check all the panes. */
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!window_pane_visible(wp))
			continue;
		if ((retval = screen_redraw_cell_border1(wp, px, py)) != -1)
			return (!!retval);
	}

	return (0);
}

/* Check if cell inside a pane. */
static int
screen_redraw_check_cell(struct client *c, u_int px, u_int py, int pane_status,
    struct window_pane **wpp)
{
	struct window		*w = c->session->curw->window;
	struct window_pane	*wp;
	int			 borders;
	u_int			 right, line;

	*wpp = NULL;

	if (px > w->sx || py > w->sy)
		return (CELL_OUTSIDE);

	if (pane_status != CELL_STATUS_OFF) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (!window_pane_visible(wp))
				continue;

			if (pane_status == CELL_STATUS_TOP)
				line = wp->yoff - 1;
			else
				line = wp->yoff + wp->sy;
			right = wp->xoff + 2 + wp->status_size - 1;

			if (py == line && px >= wp->xoff + 2 && px <= right)
				return (CELL_INSIDE);
		}
	}

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!window_pane_visible(wp))
			continue;
		*wpp = wp;

		/* If outside the pane and its border, skip it. */
		if ((wp->xoff != 0 && px < wp->xoff - 1) ||
		    px > wp->xoff + wp->sx ||
		    (wp->yoff != 0 && py < wp->yoff - 1) ||
		    py > wp->yoff + wp->sy)
			continue;

		/* If definitely inside, return so. */
		if (!screen_redraw_cell_border(c, px, py))
			return (CELL_INSIDE);

		/*
		 * Construct a bitmask of whether the cells to the left (bit
		 * 4), right, top, and bottom (bit 1) of this cell are borders.
		 */
		borders = 0;
		if (px == 0 || screen_redraw_cell_border(c, px - 1, py))
			borders |= 8;
		if (px <= w->sx && screen_redraw_cell_border(c, px + 1, py))
			borders |= 4;
		if (pane_status == CELL_STATUS_TOP) {
			if (py != 0 && screen_redraw_cell_border(c, px, py - 1))
				borders |= 2;
		} else {
			if (py == 0 || screen_redraw_cell_border(c, px, py - 1))
				borders |= 2;
		}
		if (py <= w->sy && screen_redraw_cell_border(c, px, py + 1))
			borders |= 1;

		/*
		 * Figure out what kind of border this cell is. Only one bit
		 * set doesn't make sense (can't have a border cell with no
		 * others connected).
		 */
		switch (borders) {
		case 15:	/* 1111, left right top bottom */
			return (CELL_JOIN);
		case 14:	/* 1110, left right top */
			return (CELL_BOTTOMJOIN);
		case 13:	/* 1101, left right bottom */
			return (CELL_TOPJOIN);
		case 12:	/* 1100, left right */
			return (CELL_TOPBOTTOM);
		case 11:	/* 1011, left top bottom */
			return (CELL_RIGHTJOIN);
		case 10:	/* 1010, left top */
			return (CELL_BOTTOMRIGHT);
		case 9:		/* 1001, left bottom */
			return (CELL_TOPRIGHT);
		case 7:		/* 0111, right top bottom */
			return (CELL_LEFTJOIN);
		case 6:		/* 0110, right top */
			return (CELL_BOTTOMLEFT);
		case 5:		/* 0101, right bottom */
			return (CELL_TOPLEFT);
		case 3:		/* 0011, top bottom */
			return (CELL_LEFTRIGHT);
		}
	}

	return (CELL_OUTSIDE);
}

/* Check if the border of a particular pane. */
static int
screen_redraw_check_is(u_int px, u_int py, int type, int pane_status,
    struct window *w, struct window_pane *wantwp, struct window_pane *wp)
{
	int	border;

	/* Is this off the active pane border? */
	border = screen_redraw_cell_border1(wantwp, px, py);
	if (border == 0 || border == -1)
		return (0);
	if (pane_status == CELL_STATUS_TOP && border == 4)
		return (0);
	if (pane_status == CELL_STATUS_BOTTOM && border == 3)
		return (0);

	/* If there are more than two panes, that's enough. */
	if (window_count_panes(w) != 2)
		return (1);

	/* Else if the cell is not a border cell, forget it. */
	if (wp == NULL || (type == CELL_OUTSIDE || type == CELL_INSIDE))
		return (1);

	/* With status lines mark the entire line. */
	if (pane_status != CELL_STATUS_OFF)
		return (1);

	/* Check if the pane covers the whole width. */
	if (wp->xoff == 0 && wp->sx == w->sx) {
		/* This can either be the top pane or the bottom pane. */
		if (wp->yoff == 0) { /* top pane */
			if (wp == wantwp)
				return (px <= wp->sx / 2);
			return (px > wp->sx / 2);
		}
		return (0);
	}

	/* Check if the pane covers the whole height. */
	if (wp->yoff == 0 && wp->sy == w->sy) {
		/* This can either be the left pane or the right pane. */
		if (wp->xoff == 0) { /* left pane */
			if (wp == wantwp)
				return (py <= wp->sy / 2);
			return (py > wp->sy / 2);
		}
		return (0);
	}

	return (1);
}

/* Update pane status. */
static int
screen_redraw_make_pane_status(struct client *c, struct window *w,
    struct window_pane *wp)
{
	struct grid_cell	 gc;
	const char		*fmt;
	struct format_tree	*ft;
	char			*out;
	size_t			 outlen;
	struct screen_write_ctx	 ctx;
	struct screen		 old;

	if (wp == w->active)
		style_apply(&gc, w->options, "pane-active-border-style");
	else
		style_apply(&gc, w->options, "pane-border-style");

	fmt = options_get_string(w->options, "pane-border-format");

	ft = format_create(c, NULL, FORMAT_PANE|wp->id, 0);
	format_defaults(ft, c, NULL, NULL, wp);

	memcpy(&old, &wp->status_screen, sizeof old);
	screen_init(&wp->status_screen, wp->sx, 1, 0);
	wp->status_screen.mode = 0;

	out = format_expand(ft, fmt);
	outlen = screen_write_cstrlen("%s", out);
	if (outlen > wp->sx - 4)
		outlen = wp->sx - 4;
	screen_resize(&wp->status_screen, outlen, 1, 0);

	screen_write_start(&ctx, NULL, &wp->status_screen);
	screen_write_cursormove(&ctx, 0, 0, 0);
	screen_write_clearline(&ctx, 8);
	screen_write_cnputs(&ctx, outlen, &gc, "%s", out);
	screen_write_stop(&ctx);

	free(out);
	format_free(ft);

	wp->status_size = outlen;

	if (grid_compare(wp->status_screen.grid, old.grid) == 0) {
		screen_free(&old);
		return (0);
	}
	screen_free(&old);
	return (1);
}

/* Draw pane status. */
static void
screen_redraw_draw_pane_status(struct screen_redraw_ctx *ctx)
{
	struct client		*c = ctx->c;
	struct window		*w = c->session->curw->window;
	struct tty		*tty = &c->tty;
	struct window_pane	*wp;
	struct screen		*s;
	u_int			 i, x, width, xoff, yoff, size;

	log_debug("%s: %s @%u", __func__, c->name, w->id);

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!window_pane_visible(wp))
			continue;
		s = &wp->status_screen;

		size = wp->status_size;
		if (ctx->pane_status == CELL_STATUS_TOP)
			yoff = wp->yoff - 1;
		else
			yoff = wp->yoff + wp->sy;
		xoff = wp->xoff + 2;

		if (xoff + size <= ctx->ox ||
		    xoff >= ctx->ox + ctx->sx ||
		    yoff < ctx->oy ||
		    yoff >= ctx->oy + ctx->sy)
			continue;

		if (xoff >= ctx->ox && xoff + size <= ctx->ox + ctx->sx) {
			/* All visible. */
			i = 0;
			x = xoff - ctx->ox;
			width = size;
		} else if (xoff < ctx->ox && xoff + size > ctx->ox + ctx->sx) {
			/* Both left and right not visible. */
			i = ctx->ox;
			x = 0;
			width = ctx->sx;
		} else if (xoff < ctx->ox) {
			/* Left not visible. */
			i = ctx->ox - xoff;
			x = 0;
			width = size - i;
		} else {
			/* Right not visible. */
			i = 0;
			x = xoff - ctx->ox;
			width = size - x;
		}

		if (ctx->top)
			yoff += ctx->lines;
		tty_draw_line(tty, NULL, s, i, 0, width, x, yoff - ctx->oy);
	}
	tty_cursor(tty, 0, 0);
}

/* Update status line and change flags if unchanged. */
static int
screen_redraw_update(struct client *c, int flags)
{
	struct window		*w = c->session->curw->window;
	struct window_pane	*wp;
	struct options		*wo = w->options;
	int			 redraw;

	if (c->message_string != NULL)
		redraw = status_message_redraw(c);
	else if (c->prompt_string != NULL)
		redraw = status_prompt_redraw(c);
	else
		redraw = status_redraw(c);
	if (!redraw && (~flags & CLIENT_REDRAWSTATUSALWAYS))
		flags &= ~CLIENT_REDRAWSTATUS;

	if (options_get_number(wo, "pane-border-status") != CELL_STATUS_OFF) {
		redraw = 0;
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (screen_redraw_make_pane_status(c, w, wp))
				redraw = 1;
		}
		if (redraw)
			flags |= CLIENT_REDRAWBORDERS;
	}
	return (flags);
}

/* Set up redraw context. */
static void
screen_redraw_set_context(struct client *c, struct screen_redraw_ctx *ctx)
{
	struct session	*s = c->session;
	struct options	*oo = s->options;
	struct window	*w = s->curw->window;
	struct options	*wo = w->options;

	memset(ctx, 0, sizeof *ctx);
	ctx->c = c;

	ctx->lines = status_line_size(c);
	if (c->message_string != NULL || c->prompt_string != NULL)
		ctx->lines = (ctx->lines == 0) ? 1 : ctx->lines;
	if (ctx->lines != 0 && options_get_number(oo, "status-position") == 0)
		ctx->top = 1;
	ctx->pane_status = options_get_number(wo, "pane-border-status");

	tty_window_offset(&c->tty, &ctx->ox, &ctx->oy, &ctx->sx, &ctx->sy);

	log_debug("%s: %s @%u ox=%u oy=%u sx=%u sy=%u %u/%d", __func__, c->name,
	    w->id, ctx->ox, ctx->oy, ctx->sx, ctx->sy, ctx->lines, ctx->top);
}

/* Redraw entire screen. */
void
screen_redraw_screen(struct client *c)
{
	struct screen_redraw_ctx	ctx;
	int				flags;

	if (c->flags & CLIENT_SUSPENDED)
		return;

	flags = screen_redraw_update(c, c->flags);
	screen_redraw_set_context(c, &ctx);

	if (flags & (CLIENT_REDRAWWINDOW|CLIENT_REDRAWBORDERS)) {
		if (ctx.pane_status != CELL_STATUS_OFF)
			screen_redraw_draw_pane_status(&ctx);
		screen_redraw_draw_borders(&ctx);
	}
	if (flags & CLIENT_REDRAWWINDOW)
		screen_redraw_draw_panes(&ctx);
	if (ctx.lines != 0 &&
	    (flags & (CLIENT_REDRAWSTATUS|CLIENT_REDRAWSTATUSALWAYS)))
		screen_redraw_draw_status(&ctx);
	tty_reset(&c->tty);
}

/* Redraw a single pane. */
void
screen_redraw_pane(struct client *c, struct window_pane *wp)
{
	struct screen_redraw_ctx	 ctx;

	if (!window_pane_visible(wp))
		return;

	screen_redraw_set_context(c, &ctx);

	screen_redraw_draw_pane(&ctx, wp);
	tty_reset(&c->tty);
}

/* Draw a border cell. */
static void
screen_redraw_draw_borders_cell(struct screen_redraw_ctx *ctx, u_int i, u_int j,
    struct grid_cell *m_active_gc, struct grid_cell *active_gc,
    struct grid_cell *m_other_gc, struct grid_cell *other_gc)
{
	struct client		*c = ctx->c;
	struct session		*s = c->session;
	struct window		*w = s->curw->window;
	struct tty		*tty = &c->tty;
	struct window_pane	*wp;
	struct window_pane	*active = w->active;
	struct window_pane	*marked = marked_pane.wp;
	u_int			 type, x = ctx->ox + i, y = ctx->oy + j;
	int			 flag, pane_status = ctx->pane_status;

	type = screen_redraw_check_cell(c, x, y, pane_status, &wp);
	if (type == CELL_INSIDE)
		return;
	flag = screen_redraw_check_is(x, y, type, pane_status, w, active, wp);

	if (server_is_marked(s, s->curw, marked_pane.wp) &&
	    screen_redraw_check_is(x, y, type, pane_status, w, marked, wp)) {
		if (flag)
			tty_attributes(tty, m_active_gc, NULL);
		else
			tty_attributes(tty, m_other_gc, NULL);
	} else if (flag)
		tty_attributes(tty, active_gc, NULL);
	else
		tty_attributes(tty, other_gc, NULL);
	if (ctx->top)
		tty_cursor(tty, i, ctx->lines + j);
	else
		tty_cursor(tty, i, j);
	tty_putc(tty, CELL_BORDERS[type]);
}

/* Draw the borders. */
static void
screen_redraw_draw_borders(struct screen_redraw_ctx *ctx)
{
	struct client		*c = ctx->c;
	struct session		*s = c->session;
	struct window		*w = s->curw->window;
	struct tty		*tty = &c->tty;
	struct options		*oo = w->options;
	struct grid_cell	 m_active_gc, active_gc, m_other_gc, other_gc;
	u_int		 	 i, j;

	log_debug("%s: %s @%u", __func__, c->name, w->id);

	style_apply(&other_gc, oo, "pane-border-style");
	style_apply(&active_gc, oo, "pane-active-border-style");
	active_gc.attr = other_gc.attr = GRID_ATTR_CHARSET;

	memcpy(&m_other_gc, &other_gc, sizeof m_other_gc);
	m_other_gc.attr ^= GRID_ATTR_REVERSE;
	memcpy(&m_active_gc, &active_gc, sizeof m_active_gc);
	m_active_gc.attr ^= GRID_ATTR_REVERSE;

	for (j = 0; j < tty->sy - ctx->lines; j++) {
		for (i = 0; i < tty->sx; i++) {
			screen_redraw_draw_borders_cell(ctx, i, j,
			    &m_active_gc, &active_gc, &m_other_gc, &other_gc);
		}
	}
}

/* Draw the panes. */
static void
screen_redraw_draw_panes(struct screen_redraw_ctx *ctx)
{
	struct client		*c = ctx->c;
	struct window		*w = c->session->curw->window;
	struct window_pane	*wp;

	log_debug("%s: %s @%u", __func__, c->name, w->id);

	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (!window_pane_visible(wp))
			continue;
		screen_redraw_draw_pane(ctx, wp);
		if (c->flags & CLIENT_IDENTIFY)
			screen_redraw_draw_number(ctx, wp);
	}
}

/* Draw the status line. */
static void
screen_redraw_draw_status(struct screen_redraw_ctx *ctx)
{
	struct client	*c = ctx->c;
	struct window	*w = c->session->curw->window;
	struct tty	*tty = &c->tty;
	struct screen	*s = c->status.active;
	u_int		 i, y;

	log_debug("%s: %s @%u", __func__, c->name, w->id);

	if (ctx->top)
		y = 0;
	else
		y = c->tty.sy - ctx->lines;
	for (i = 0; i < ctx->lines; i++)
		tty_draw_line(tty, NULL, s, 0, i, UINT_MAX, 0, y + i);
}

/* Draw one pane. */
static void
screen_redraw_draw_pane(struct screen_redraw_ctx *ctx, struct window_pane *wp)
{
	struct client	*c = ctx->c;
	struct window	*w = c->session->curw->window;
	struct tty	*tty = &c->tty;
	struct screen	*s;
	u_int		 i, j, top, x, y, width;

	log_debug("%s: %s @%u %%%u", __func__, c->name, w->id, wp->id);

	if (wp->xoff + wp->sx <= ctx->ox || wp->xoff >= ctx->ox + ctx->sx)
		return;
	if (ctx->top)
		top = ctx->lines;
	else
		top = 0;

	s = wp->screen;
	for (j = 0; j < wp->sy; j++) {
		if (wp->yoff + j < ctx->oy || wp->yoff + j >= ctx->oy + ctx->sy)
			continue;
		y = top + wp->yoff + j - ctx->oy;

		if (wp->xoff >= ctx->ox &&
		    wp->xoff + wp->sx <= ctx->ox + ctx->sx) {
			/* All visible. */
			i = 0;
			x = wp->xoff - ctx->ox;
			width = wp->sx;
		} else if (wp->xoff < ctx->ox &&
		    wp->xoff + wp->sx > ctx->ox + ctx->sx) {
			/* Both left and right not visible. */
			i = ctx->ox;
			x = 0;
			width = ctx->sx;
		} else if (wp->xoff < ctx->ox) {
			/* Left not visible. */
			i = ctx->ox - wp->xoff;
			x = 0;
			width = wp->sx - i;
		} else {
			/* Right not visible. */
			i = 0;
			x = wp->xoff - ctx->ox;
			width = ctx->sx - x;
		}
		log_debug("%s: %s %%%u line %u,%u at %u,%u, width %u",
		    __func__, c->name, wp->id, i, j, x, y, width);

		tty_draw_line(tty, wp, s, i, j, width, x, y);
	}
}

/* Draw number on a pane. */
static void
screen_redraw_draw_number(struct screen_redraw_ctx *ctx, struct window_pane *wp)
{
	struct client		*c = ctx->c;
	struct tty		*tty = &c->tty;
	struct session		*s = c->session;
	struct options		*oo = s->options;
	struct window		*w = wp->window;
	struct grid_cell	 gc;
	u_int			 idx, px, py, i, j, xoff, yoff, sx, sy;
	int			 colour, active_colour;
	char			 buf[16], *ptr;
	size_t			 len;

	if (wp->xoff + wp->sx <= ctx->ox ||
	    wp->xoff >= ctx->ox + ctx->sx ||
	    wp->yoff + wp->sy <= ctx->oy ||
	    wp->yoff >= ctx->oy + ctx->sy)
		return;

	if (wp->xoff >= ctx->ox && wp->xoff + wp->sx <= ctx->ox + ctx->sx) {
		/* All visible. */
		xoff = wp->xoff - ctx->ox;
		sx = wp->sx;
	} else if (wp->xoff < ctx->ox &&
	    wp->xoff + wp->sx > ctx->ox + ctx->sx) {
		/* Both left and right not visible. */
		xoff = 0;
		sx = ctx->sx;
	} else if (wp->xoff < ctx->ox) {
		/* Left not visible. */
		xoff = 0;
		sx = wp->sx - (ctx->ox - wp->xoff);
	} else {
		/* Right not visible. */
		xoff = wp->xoff - ctx->ox;
		sx = wp->sx - xoff;
	}
	if (wp->yoff >= ctx->oy && wp->yoff + wp->sy <= ctx->oy + ctx->sy) {
		/* All visible. */
		yoff = wp->yoff - ctx->oy;
		sy = wp->sy;
	} else if (wp->yoff < ctx->oy &&
	    wp->yoff + wp->sy > ctx->oy + ctx->sy) {
		/* Both top and bottom not visible. */
		yoff = 0;
		sy = ctx->sy;
	} else if (wp->yoff < ctx->oy) {
		/* Top not visible. */
		yoff = 0;
		sy = wp->sy - (ctx->oy - wp->yoff);
	} else {
		/* Bottom not visible. */
		yoff = wp->yoff - ctx->oy;
		sy = wp->sy - yoff;
	}

	if (ctx->top)
		yoff += ctx->lines;
	px = sx / 2;
	py = sy / 2;

	if (window_pane_index(wp, &idx) != 0)
		fatalx("index not found");
	len = xsnprintf(buf, sizeof buf, "%u", idx);

	if (sx < len)
		return;
	colour = options_get_number(oo, "display-panes-colour");
	active_colour = options_get_number(oo, "display-panes-active-colour");

	if (sx < len * 6 || sy < 5) {
		tty_cursor(tty, xoff + px - len / 2, yoff + py);
		goto draw_text;
	}

	px -= len * 3;
	py -= 2;

	memcpy(&gc, &grid_default_cell, sizeof gc);
	if (w->active == wp)
		gc.bg = active_colour;
	else
		gc.bg = colour;
	gc.flags |= GRID_FLAG_NOPALETTE;

	tty_attributes(tty, &gc, wp);
	for (ptr = buf; *ptr != '\0'; ptr++) {
		if (*ptr < '0' || *ptr > '9')
			continue;
		idx = *ptr - '0';

		for (j = 0; j < 5; j++) {
			for (i = px; i < px + 5; i++) {
				tty_cursor(tty, xoff + i, yoff + py + j);
				if (window_clock_table[idx][j][i - px])
					tty_putc(tty, ' ');
			}
		}
		px += 6;
	}

	len = xsnprintf(buf, sizeof buf, "%ux%u", wp->sx, wp->sy);
	if (sx < len || sy < 6)
		return;
	tty_cursor(tty, xoff + sx - len, yoff);

draw_text:
	memcpy(&gc, &grid_default_cell, sizeof gc);
	if (w->active == wp)
		gc.fg = active_colour;
	else
		gc.fg = colour;
	gc.flags |= GRID_FLAG_NOPALETTE;

	tty_attributes(tty, &gc, wp);
	tty_puts(tty, buf);

	tty_cursor(tty, 0, 0);
}
