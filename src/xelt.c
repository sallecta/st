/* See LICENSE for license details. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>
#include <fontconfig/fontconfig.h>
#include <wchar.h>
#include <string.h>

#include "xelt.h"

/* Config.h for applying patches and the configuration. */

#include "config.h"
/* Drawing Context */
typedef struct {
	xelt_Color col[MAX(LEN(colorname), 256)];
	xelt_Font font, bfont, ifont, ibfont;
	GC gc;
} xelt_DrawingContext;




static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[Expose] = expose,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
/*
 * Uncomment if you want the selection to disappear when you select something
 * different in another window.
 */
/*	[SelectionClear] = selclear, */
	[SelectionNotify] = selnotify,
/*
 * PropertyNotify is only turned on when there is some INCR transfer happening
 * for the selection retrieval.
 */
	[PropertyNotify] = propnotify,
	[SelectionRequest] = selrequest,
};

/* Globals */
static xelt_DrawingContext dc;
static xelt_Window xelt_windowmain;
static xelt_Terminal terminal;
static xelt_CSIEscape csiescseq;
static xelt_STREscape strescseq;
static int cmdfd;
static pid_t pid;
static xelt_Selection sel;
static int iofd = 1;
static char **opt_cmd  = NULL;
static char *opt_class = NULL;
static char *opt_embed = NULL;
static char *opt_font  = NULL;
static char *opt_io    = NULL;
static char *opt_line  = NULL;
static char *opt_name  = NULL;
static char *window_title = NULL;
static int oldbutton   = 3; /* button event on startup: 3 = release */

static char *usedfont = NULL;
static double usedfontsize = 0;
static double defaultfontsize = 0;

static xelt_uchar utfbyte[XELT_SIZE_UTF + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static xelt_uchar utfmask[XELT_SIZE_UTF + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static xelt_CharCode utfmin[XELT_SIZE_UTF + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static xelt_CharCode utfmax[XELT_SIZE_UTF + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};


/* xelt_Fontcache is an array now. A new font will be appended to the array. */
static xelt_Fontcache frc[16];
static int frclen = 0;

ssize_t
xwrite(int fd, const char *s, size_t len)
{
	size_t aux = len;
	ssize_t r;

	while (len > 0) {
		r = write(fd, s, len);
		if (r < 0)
			return r;
		len -= r;
		s += r;
	}

	return aux;
}

void *
xmalloc(size_t len)
{
	void *p = malloc(len);

	if (!p)
		printAndExit("Out of memory\n");

	return p;
}

void *
xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		printAndExit("Out of memory\n");

	return p;
}

char *
xstrdup(char *s)
{
	if ((s = strdup(s)) == NULL)
		printAndExit("Out of memory\n");

	return s;
}

size_t
utf8decode(char *c, xelt_CharCode *u, size_t clen)
{
	size_t i, j, len, type;
	xelt_CharCode udecoded;

	*u = XELT_SIZE_UTF_INVALID;
	if (!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, XELT_SIZE_UTF))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type != 0)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);

	return len;
}

xelt_CharCode
utf8decodebyte(char c, size_t *i)
{
	for (*i = 0; *i < LEN(utfmask); ++(*i))
		if (((xelt_uchar)c & utfmask[*i]) == utfbyte[*i])
			return (xelt_uchar)c & ~utfmask[*i];

	return 0;
}

size_t
utf8encode(xelt_CharCode u, char *c)
{
	size_t len, i;

	len = utf8validate(&u, 0);
	if (len > XELT_SIZE_UTF)
		return 0;

	for (i = len - 1; i != 0; --i) {
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);

	return len;
}

char
utf8encodebyte(xelt_CharCode u, size_t i)
{
	return utfbyte[i] | (u & ~utfmask[i]);
}

char *
utf8strchr(char *s, xelt_CharCode u)
{
	xelt_CharCode r;
	size_t i, j, len;

	len = strlen(s);
	for (i = 0, j = 0; i < len; i += j) {
		if (!(j = utf8decode(&s[i], &r, len - i)))
			break;
		if (r == u)
			return &(s[i]);
	}

	return NULL;
}

size_t
utf8validate(xelt_CharCode *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = XELT_SIZE_UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;

	return i;
}

void
selinit(void)
{
	clock_gettime(CLOCK_MONOTONIC, &sel.tclick1);
	clock_gettime(CLOCK_MONOTONIC, &sel.tclick2);
	sel.mode = XELT_SEL_IDLE;
	sel.snap = 0;
	sel.ob.x = -1;
	sel.primary = NULL;
	sel.clipboard = NULL;
	sel.xtarget = XInternAtom(xelt_windowmain.display, "UTF8_STRING", 0);
	if (sel.xtarget == None)
		sel.xtarget = XA_STRING;
}

int
x2col(int x)
{
	x -= borderpx;
	x /= xelt_windowmain.charwidth;

	return LIMIT(x, 0, terminal.col-1);
}

int
y2row(int y)
{
	y -= borderpx;
	y /= xelt_windowmain.charheight;

	return LIMIT(y, 0, terminal.row-1);
}

int
tlinelen(int y)
{
	int i = terminal.col;

	if (terminal.line[y][i - 1].mode & XELT_ATTR_WRAP)
		return i;

	while (i > 0 && terminal.line[y][i - 1].u == ' ')
		--i;

	return i;
}

void
selnormalize(void)
{
	int i;

	if (sel.type == XELT_SEL_REGULAR && sel.ob.y != sel.oe.y) {
		sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
		sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
	} else {
		sel.nb.x = MIN(sel.ob.x, sel.oe.x);
		sel.ne.x = MAX(sel.ob.x, sel.oe.x);
	}
	sel.nb.y = MIN(sel.ob.y, sel.oe.y);
	sel.ne.y = MAX(sel.ob.y, sel.oe.y);

	selsnap(&sel.nb.x, &sel.nb.y, -1);
	selsnap(&sel.ne.x, &sel.ne.y, +1);

	/* expand selection over line breaks */
	if (sel.type == XELT_SEL_RECTANGULAR)
		return;
	i = tlinelen(sel.nb.y);
	if (i < sel.nb.x)
		sel.nb.x = i;
	if (tlinelen(sel.ne.y) <= sel.ne.x)
		sel.ne.x = terminal.col - 1;
}

int
selected(int x, int y)
{
	if (sel.mode == XELT_SEL_EMPTY)
		return 0;

	if (sel.type == XELT_SEL_RECTANGULAR)
		return BETWEEN(y, sel.nb.y, sel.ne.y)
		    && BETWEEN(x, sel.nb.x, sel.ne.x);

	return BETWEEN(y, sel.nb.y, sel.ne.y)
	    && (y != sel.nb.y || x >= sel.nb.x)
	    && (y != sel.ne.y || x <= sel.ne.x);
}

void
selsnap(int *x, int *y, int direction)
{
	int newx, newy, xt, yt;
	int delim, prevdelim;
	xelt_Glyph *gp, *prevgp;

	switch (sel.snap) {
	case XELT_SELSNAP_WORD:
		/*
		 * Snap around if the word wraps around at the end or
		 * beginning of a line.
		 */
		prevgp = &terminal.line[*y][*x];
		prevdelim = ISDELIM(prevgp->u);
		for (;;) {
			newx = *x + direction;
			newy = *y;
			if (!BETWEEN(newx, 0, terminal.col - 1)) {
				newy += direction;
				newx = (newx + terminal.col) % terminal.col;
				if (!BETWEEN(newy, 0, terminal.row - 1))
					break;

				if (direction > 0)
					yt = *y, xt = *x;
				else
					yt = newy, xt = newx;
				if (!(terminal.line[yt][xt].mode & XELT_ATTR_WRAP))
					break;
			}

			if (newx >= tlinelen(newy))
				break;

			gp = &terminal.line[newy][newx];
			delim = ISDELIM(gp->u);
			if (!(gp->mode & XELT_ATTR_WDUMMY) && (delim != prevdelim
					|| (delim && gp->u != prevgp->u)))
				break;

			*x = newx;
			*y = newy;
			prevgp = gp;
			prevdelim = delim;
		}
		break;
	case XELT_SELSNAP_LINE:
		/*
		 * Snap around if the the previous line or the current one
		 * has set XELT_ATTR_WRAP at its end. Then the whole next or
		 * previous line will be selected.
		 */
		*x = (direction < 0) ? 0 : terminal.col - 1;
		if (direction < 0) {
			for (; *y > 0; *y += direction) {
				if (!(terminal.line[*y-1][terminal.col-1].mode
						& XELT_ATTR_WRAP)) {
					break;
				}
			}
		} else if (direction > 0) {
			for (; *y < terminal.row-1; *y += direction) {
				if (!(terminal.line[*y][terminal.col-1].mode
						& XELT_ATTR_WRAP)) {
					break;
				}
			}
		}
		break;
	}
}

void
getbuttoninfo(XEvent *e)
{
	int type;
	xelt_uint state = e->xbutton.state & ~(Button1Mask | forceselmod);

	sel.alt = IS_SET(XELT_TERMINAL_ALTSCREEN);

	sel.oe.x = x2col(e->xbutton.x);
	sel.oe.y = y2row(e->xbutton.y);
	selnormalize();

	sel.type = XELT_SEL_REGULAR;
	for (type = 1; type < LEN(selmasks); ++type) {
		if (match(selmasks[type], state)) {
			sel.type = type;
			break;
		}
	}
}

void
mousereport(XEvent *e)
{
	int x = x2col(e->xbutton.x), y = y2row(e->xbutton.y),
	    button = e->xbutton.button, state = e->xbutton.state,
	    len;
	char buf[40];
	static int ox, oy;

	/* from urxvt */
	if (e->xbutton.type == MotionNotify) {
		if (x == ox && y == oy)
			return;
		if (!IS_SET(XELT_TERMINAL_MOUSEMOTION) && !IS_SET(XELT_TERMINAL_MOUSEMANY))
			return;
		/* MOUSE_MOTION: no reporting if no button is pressed */
		if (IS_SET(XELT_TERMINAL_MOUSEMOTION) && oldbutton == 3)
			return;

		button = oldbutton + 32;
		ox = x;
		oy = y;
	} else {
		if (!IS_SET(XELT_TERMINAL_MOUSESGR) && e->xbutton.type == ButtonRelease) {
			button = 3;
		} else {
			button -= Button1;
			if (button >= 3)
				button += 64 - 3;
		}
		if (e->xbutton.type == ButtonPress) {
			oldbutton = button;
			ox = x;
			oy = y;
		} else if (e->xbutton.type == ButtonRelease) {
			oldbutton = 3;
			/* XELT_TERMINAL_MOUSEX10: no button release reporting */
			if (IS_SET(XELT_TERMINAL_MOUSEX10))
				return;
			if (button == 64 || button == 65)
				return;
		}
	}

	if (!IS_SET(XELT_TERMINAL_MOUSEX10)) {
		button += ((state & ShiftMask  ) ? 4  : 0)
			+ ((state & Mod4Mask   ) ? 8  : 0)
			+ ((state & ControlMask) ? 16 : 0);
	}

	if (IS_SET(XELT_TERMINAL_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				button, x+1, y+1,
				e->xbutton.type == ButtonRelease ? 'm' : 'M');
	} else if (x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32+button, 32+x+1, 32+y+1);
	} else {
		return;
	}

	ttywrite(buf, len);
}

void
bpress(XEvent *e)
{
	struct timespec now;
	xelt_MouseShortcut *ms;

	if (IS_SET(XELT_TERMINAL_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
		if (e->xbutton.button == ms->b
				&& match(ms->mask, e->xbutton.state)) {
			ttysend(ms->s, strlen(ms->s));
			return;
		}
	}

	if (e->xbutton.button == Button1) {
		clock_gettime(CLOCK_MONOTONIC, &now);

		/* Clear previous selection, logically and visually. */
		selclear(NULL);
		sel.mode = XELT_SEL_EMPTY;
		sel.type = XELT_SEL_REGULAR;
		sel.oe.x = sel.ob.x = x2col(e->xbutton.x);
		sel.oe.y = sel.ob.y = y2row(e->xbutton.y);

		/*
		 * If the user clicks below predefined timeouts specific
		 * snapping behaviour is exposed.
		 */
		if (TIMEDIFF(now, sel.tclick2) <= tripleclicktimeout) {
			sel.snap = XELT_SELSNAP_LINE;
		} else if (TIMEDIFF(now, sel.tclick1) <= doubleclicktimeout) {
			sel.snap = XELT_SELSNAP_WORD;
		} else {
			sel.snap = 0;
		}
		selnormalize();

		if (sel.snap != 0)
			sel.mode = XELT_SEL_READY;
		tsetdirt(sel.nb.y, sel.ne.y);
		sel.tclick2 = sel.tclick1;
		sel.tclick1 = now;
	}
}

char *
getsel(void)
{
	char *str, *ptr;
	int y, bufsize, lastx, linelen;
	xelt_Glyph *gp, *last;

	if (sel.ob.x == -1)
		return NULL;

	bufsize = (terminal.col+1) * (sel.ne.y-sel.nb.y+1) * XELT_SIZE_UTF;
	ptr = str = xmalloc(bufsize);

	/* append every set & selected glyph to the selection */
	for (y = sel.nb.y; y <= sel.ne.y; y++) {
		if ((linelen = tlinelen(y)) == 0) {
			*ptr++ = '\n';
			continue;
		}

		if (sel.type == XELT_SEL_RECTANGULAR) {
			gp = &terminal.line[y][sel.nb.x];
			lastx = sel.ne.x;
		} else {
			gp = &terminal.line[y][sel.nb.y == y ? sel.nb.x : 0];
			lastx = (sel.ne.y == y) ? sel.ne.x : terminal.col-1;
		}
		last = &terminal.line[y][MIN(lastx, linelen-1)];
		while (last >= gp && last->u == ' ')
			--last;

		for ( ; gp <= last; ++gp) {
			if (gp->mode & XELT_ATTR_WDUMMY)
				continue;

			ptr += utf8encode(gp->u, ptr);
		}

		/*
		 * Copy and pasting of line endings is inconsistent
		 * in the inconsistent terminal and GUI world.
		 * The best solution seems like to produce '\n' when
		 * something is copied from st and convert '\n' to
		 * '\r', when something to be pasted is received by
		 * st.
		 * FIXME: Fix the computer world.
		 */
		if ((y < sel.ne.y || lastx >= linelen) && !(last->mode & XELT_ATTR_WRAP))
			*ptr++ = '\n';
	}
	*ptr = 0;
	return str;
}

void
selcopy(Time t)
{
	xsetsel(getsel(), t);
}

void
propnotify(XEvent *e)
{
	XPropertyEvent *xpev;
	Atom clipboard = XInternAtom(xelt_windowmain.display, "CLIPBOARD", 0);

	xpev = &e->xproperty;
	if (xpev->state == PropertyNewValue &&
			(xpev->atom == XA_PRIMARY ||
			 xpev->atom == clipboard)) {
		selnotify(e);
	}
}

void
selnotify(XEvent *e)
{
	xelt_ulong nitems, ofs, rem;
	int format;
	xelt_uchar *data, *last, *repl;
	Atom type, incratom, property;

	incratom = XInternAtom(xelt_windowmain.display, "INCR", 0);

	ofs = 0;
	if (e->type == SelectionNotify) {
		property = e->xselection.property;
	} else if(e->type == PropertyNotify) {
		property = e->xproperty.atom;
	} else {
		return;
	}
	if (property == None)
		return;

	do {
		if (XGetWindowProperty(xelt_windowmain.display, xelt_windowmain.id, property, ofs,
					BUFSIZ/4, False, AnyPropertyType,
					&type, &format, &nitems, &rem,
					&data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}

		if (e->type == PropertyNotify && nitems == 0 && rem == 0) {
			/*
			 * If there is some PropertyNotify with no data, then
			 * this is the signal of the selection owner that all
			 * data has been transferred. We won't need to receive
			 * PropertyNotify events anymore.
			 */
			MODBIT(xelt_windowmain.attrs.event_mask, 0, PropertyChangeMask);
			XChangeWindowAttributes(xelt_windowmain.display, xelt_windowmain.id, CWEventMask,
					&xelt_windowmain.attrs);
		}

		if (type == incratom) {
			/*
			 * Activate the PropertyNotify events so we receive
			 * when the selection owner does send us the next
			 * chunk of data.
			 */
			MODBIT(xelt_windowmain.attrs.event_mask, 1, PropertyChangeMask);
			XChangeWindowAttributes(xelt_windowmain.display, xelt_windowmain.id, CWEventMask,
					&xelt_windowmain.attrs);

			/*
			 * Deleting the property is the transfer start signal.
			 */
			XDeleteProperty(xelt_windowmain.display, xelt_windowmain.id, (int)property);
			continue;
		}

		/*
		 * As seen in getsel:
		 * xelt_Line endings are inconsistent in the terminal and GUI world
		 * copy and pasting. When receiving some selection data,
		 * replace all '\n' with '\r'.
		 * FIXME: Fix the computer world.
		 */
		repl = data;
		last = data + nitems * format / 8;
		while ((repl = memchr(repl, '\n', last - repl))) {
			*repl++ = '\r';
		}

		if (IS_SET(XELT_TERMINAL_BRCKTPASTE) && ofs == 0)
			ttywrite("\033[200~", 6);
		ttysend((char *)data, nitems * format / 8);
		if (IS_SET(XELT_TERMINAL_BRCKTPASTE) && rem == 0)
			ttywrite("\033[201~", 6);
		XFree(data);
		/* number of 32-bit chunks returned */
		ofs += nitems * format / 32;
	} while (rem > 0);

	/*
	 * Deleting the property again tells the selection owner to send the
	 * next data chunk in the property.
	 */
	XDeleteProperty(xelt_windowmain.display, xelt_windowmain.id, (int)property);
}

void
selpaste(const xelt_Arg *dummy)
{
	XConvertSelection(xelt_windowmain.display, XA_PRIMARY, sel.xtarget, XA_PRIMARY,
			xelt_windowmain.id, CurrentTime);
}

void
clipcopy(const xelt_Arg *dummy)
{
	Atom clipboard;

	if (sel.clipboard != NULL)
		free(sel.clipboard);

	if (sel.primary != NULL) {
		sel.clipboard = xstrdup(sel.primary);
		clipboard = XInternAtom(xelt_windowmain.display, "CLIPBOARD", 0);
		XSetSelectionOwner(xelt_windowmain.display, clipboard, xelt_windowmain.id, CurrentTime);
	}
}

void
clippaste(const xelt_Arg *dummy)
{
	Atom clipboard;

	clipboard = XInternAtom(xelt_windowmain.display, "CLIPBOARD", 0);
	XConvertSelection(xelt_windowmain.display, clipboard, sel.xtarget, clipboard,
			xelt_windowmain.id, CurrentTime);
}

void
selclear(XEvent *e)
{
	if (sel.ob.x == -1)
		return;
	sel.mode = XELT_SEL_IDLE;
	sel.ob.x = -1;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
selrequest(XEvent *e)
{
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, string, clipboard;
	char *seltext;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	if (xsre->property == None)
		xsre->property = xsre->target;

	/* reject */
	xev.property = None;

	xa_targets = XInternAtom(xelt_windowmain.display, "TARGETS", 0);
	if (xsre->target == xa_targets) {
		/* respond with the supported type */
		string = sel.xtarget;
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				XA_ATOM, 32, PropModeReplace,
				(xelt_uchar *) &string, 1);
		xev.property = xsre->property;
	} else if (xsre->target == sel.xtarget || xsre->target == XA_STRING) {
		/*
		 * xith XA_STRING non ascii characters may be incorrect in the
		 * requestor. It is not our problem, use utf8.
		 */
		clipboard = XInternAtom(xelt_windowmain.display, "CLIPBOARD", 0);
		if (xsre->selection == XA_PRIMARY) {
			seltext = sel.primary;
		} else if (xsre->selection == clipboard) {
			seltext = sel.clipboard;
		} else {
			fprintf(stderr,
				"Unhandled clipboard selection 0x%lx\n",
				xsre->selection);
			return;
		}
		if (seltext != NULL) {
			XChangeProperty(xsre->display, xsre->requestor,
					xsre->property, xsre->target,
					8, PropModeReplace,
					(xelt_uchar *)seltext, strlen(seltext));
			xev.property = xsre->property;
		}
	}

	/* all done, send a notification to the listener */
	if (!XSendEvent(xsre->display, xsre->requestor, 1, 0, (XEvent *) &xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

void
xsetsel(char *str, Time t)
{
	free(sel.primary);
	sel.primary = str;

	XSetSelectionOwner(xelt_windowmain.display, XA_PRIMARY, xelt_windowmain.id, t);
	if (XGetSelectionOwner(xelt_windowmain.display, XA_PRIMARY) != xelt_windowmain.id)
		selclear(0);
}

void
brelease(XEvent *e)
{
	if (IS_SET(XELT_TERMINAL_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	if (e->xbutton.button == Button2) {
		selpaste(NULL);
	} else if (e->xbutton.button == Button1) {
		if (sel.mode == XELT_SEL_READY) {
			getbuttoninfo(e);
			selcopy(e->xbutton.time);
		} else
			selclear(NULL);
		sel.mode = XELT_SEL_IDLE;
		tsetdirt(sel.nb.y, sel.ne.y);
	}
}

void
bmotion(XEvent *e)
{
	int oldey, oldex, oldsby, oldsey;

	if (IS_SET(XELT_TERMINAL_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	if (!sel.mode)
		return;

	sel.mode = XELT_SEL_READY;
	oldey = sel.oe.y;
	oldex = sel.oe.x;
	oldsby = sel.nb.y;
	oldsey = sel.ne.y;
	getbuttoninfo(e);

	if (oldey != sel.oe.y || oldex != sel.oe.x)
		tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));
}

void
printAndExit(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

void
execsh(void)
{
	char *me="execsh";
	xelt_log(me,"started");
	
	char **args, *sh, *prog;
	const struct passwd *pw;
	char bufWinId[sizeof(long) * 8 + 1];

	errno = 0;
	if ((pw = getpwuid(getuid())) == NULL) {
		if (errno)
			printAndExit("getpwuid:%s\n", strerror(errno));
		else
			printAndExit("who are you?\n");
	}

	if ((sh = getenv("SHELL")) == NULL) {
		sh = (pw->pw_shell[0]) ? pw->pw_shell : shell;
		xelt_log(me,"getenv(\"SHELL\") is NULL");
	}
	
	snprintf(charbuf256, 256, "%s%s", "shell is: ", sh);
	xelt_log(me,charbuf256);
	if (opt_cmd)
		prog = opt_cmd[0];
	else if (utmp)
		prog = utmp;
	else
		prog = sh;
	args = (opt_cmd) ? opt_cmd : (char *[]) {prog, NULL};

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);
	snprintf(bufWinId, sizeof(bufWinId), "%lu", xelt_windowmain.id);
	setenv("WINDOWID", bufWinId, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(prog, args);
	_exit(1);
}

void
sigchld(int a)
{
	int stat;
	pid_t p;

	if ((p = waitpid(pid, &stat, WNOHANG)) < 0)
		printAndExit("Waiting for pid %hd failed: %s\n", pid, strerror(errno));

	if (pid != p)
		return;

	if (!WIFEXITED(stat) || WEXITSTATUS(stat))
		printAndExit("child finished with error '%d'\n", stat);
	exit(0);
}


void
stty(void)
{
	char cmd[_POSIX_ARG_MAX], **p, *q, *s;
	size_t n, siz;

	if ((n = strlen(stty_args)) > sizeof(cmd)-1)
		printAndExit("incorrect stty parameters\n");
	memcpy(cmd, stty_args, n);
	q = cmd + n;
	siz = sizeof(cmd) - n;
	for (p = opt_cmd; p && (s = *p); ++p) {
		if ((n = strlen(s)) > siz-1)
			printAndExit("stty parameter length too long\n");
		*q++ = ' ';
		memcpy(q, s, n);
		q += n;
		siz -= n + 1;
	}
	*q = '\0';
	if (system(cmd) != 0)
	    perror("Couldn't call stty");
}

void
ttynew(void)
{
	char *me="ttynew";
	xelt_log(me,"started");
	int m, s;
	struct winsize w = {terminal.row, terminal.col, 0, 0};

	if (opt_io) {
		terminal.mode |= XELT_TERMINAL_PRINT;
		iofd = (!strcmp(opt_io, "-")) ?
			  1 : open(opt_io, O_WRONLY | O_CREAT, 0666);
		if (iofd < 0) {
			fprintf(stderr, "Error opening %s:%s\n",
				opt_io, strerror(errno));
		}
	}

	if (opt_line) {
		if ((cmdfd = open(opt_line, O_RDWR)) < 0)
			printAndExit("open line failed: %s\n", strerror(errno));
		dup2(cmdfd, 0);
		stty();
		return;
	}

	/* seems to work fine on linux, openbsd and freebsd */
	if (openpty(&m, &s, NULL, NULL, &w) < 0)
		printAndExit("openpty failed: %s\n", strerror(errno));
	switch (pid = fork()) {
	case -1:
		printAndExit("fork failed\n");
		break;
	case 0:
	    xelt_log(me,"fork success");
		close(iofd);
		setsid(); /* create a new process group */
        /* dup() will create the copy of file_desc, then both can be used interchangeably.  */
		dup2(s, 0); // copy stderr to s 
		dup2(s, 1); // copy stdout to s 
		xelt_log(me,"stdout copied to s ");
		dup2(s, 2);// copy stdin to s 
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			printAndExit("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		close(s);
		close(m);
		execsh();
		break;
	default:
		close(s);
		cmdfd = m;
		signal(SIGCHLD, sigchld);
		break;
	}
}

size_t
ttyread(void)
{
	static char buf[BUFSIZ];
	static int buflen = 0;
	char *ptr;
	int charsize; /* size of utf8 char in bytes */
	xelt_CharCode unicodep;
	int ret;

	/* append read bytes to unprocessed bytes */
	if ((ret = read(cmdfd, buf+buflen, LEN(buf)-buflen)) < 0)
		printAndExit("Couldn't read from shell: %s\n", strerror(errno));

	/* process every complete utf8 char */
	buflen += ret;
	ptr = buf;
	while ((charsize = utf8decode(ptr, &unicodep, buflen))) {
		tputc(unicodep);
		ptr += charsize;
		buflen -= charsize;
	}

	/* keep any uncomplete utf8 char for the next call */
	memmove(buf, ptr, buflen);

	return ret;
}

void
ttywrite(const char *s, size_t n)
{
	fd_set wfd, rfd;
	ssize_t r;
	size_t lim = 256;

	/*
	 * Remember that we are using a pty, which might be a modem line.
	 * Writing too much will clog the line. That's why we are doing this
	 * dance.
	 * FIXME: Migrate the world to Plan 9.
	 */
	while (n > 0) {
		FD_ZERO(&wfd);
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &wfd);
		FD_SET(cmdfd, &rfd);

		/* Check if we can write. */
		if (pselect(cmdfd+1, &rfd, &wfd, NULL, NULL, NULL) < 0) {
			if (errno == EINTR)
				continue;
			printAndExit("select failed: %s\n", strerror(errno));
		}
		if (FD_ISSET(cmdfd, &wfd)) {
			/*
			 * Only write the bytes written by ttywrite() or the
			 * default of 256. This seems to be a reasonable value
			 * for a serial line. Bigger values might clog the I/O.
			 */
			if ((r = write(cmdfd, s, (n < lim)? n : lim)) < 0)
				goto write_error;
			if (r < n) {
				/*
				 * We weren't able to write out everything.
				 * This means the buffer is getting full
				 * again. Empty it.
				 */
				if (n < lim)
					lim = ttyread();
				n -= r;
				s += r;
			} else {
				/* All bytes have been written. */
				break;
			}
		}
		if (FD_ISSET(cmdfd, &rfd))
			lim = ttyread();
	}
	return;

write_error:
	printAndExit("write error on tty: %s\n", strerror(errno));
}

void
ttysend(char *s, size_t n)
{
	int len;
	xelt_CharCode u;

	ttywrite(s, n);
	if (IS_SET(XELT_TERMINAL_ECHO))
		while ((len = utf8decode(s, &u, n)) > 0) {
			techo(u);
			n -= len;
			s += len;
		}
}

void
ttyresize(void)
{
	struct winsize w;

	w.ws_row = terminal.row;
	w.ws_col = terminal.col;
	w.ws_xpixel = xelt_windowmain.ttywidth;
	w.ws_ypixel = xelt_windowmain.ttyheight;
	if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

void
tsetdirt(int top, int bot)
{
	int i;

	LIMIT(top, 0, terminal.row-1);
	LIMIT(bot, 0, terminal.row-1);

	for (i = top; i <= bot; i++)
		terminal.dirty[i] = 1;
}


void
tfulldirt(void)
{
	tsetdirt(0, terminal.row-1);
}

void
tcursor(int mode)
{
	static xelt_TCursor c[2];
	int alt = IS_SET(XELT_TERMINAL_ALTSCREEN);

	if (mode == XELT_CURSOR_SAVE) {
		c[alt] = terminal.cursor;
	} else if (mode == XELT_CURSOR_LOAD) {
		terminal.cursor = c[alt];
		tmoveto(c[alt].x, c[alt].y);
	}
}

void
treset(void)
{
	xelt_uint i;

	terminal.cursor = (xelt_TCursor){{
		.mode = XELT_ATTR_NULL,
		.fg = defaultfg,
		.bg = defaultbg
	}, .x = 0, .y = 0, .state = XELT_CURSOR_DEFAULT};

	memset(terminal.tabs, 0, terminal.col * sizeof(*terminal.tabs));
	for (i = tabspaces; i < terminal.col; i += tabspaces)
		terminal.tabs[i] = 1;
	terminal.top = 0;
	terminal.bot = terminal.row - 1;
	terminal.mode = XELT_TERMINAL_WRAP;
	memset(terminal.trantbl, XELT_CHARSET_USA, sizeof(terminal.trantbl));
	terminal.charset = 0;

	for (i = 0; i < 2; i++) {
		tmoveto(0, 0);
		tcursor(XELT_CURSOR_SAVE);
		tclearregion(0, 0, terminal.col-1, terminal.row-1);
		tswapscreen();
	}
}

void
tnew(int col, int row)
{
	terminal = (xelt_Terminal){ .cursor = { .attr = { .fg = defaultfg, .bg = defaultbg } } };
	tresize(col, row);
	terminal.numlock = 1;

	treset();
}

void
tswapscreen(void)
{
	xelt_Line *tmp = terminal.line;

	terminal.line = terminal.alt;
	terminal.alt = tmp;
	terminal.mode ^= XELT_TERMINAL_ALTSCREEN;
	tfulldirt();
}

void
tscrolldown(int orig, int n)
{
	int i;
	xelt_Line temp;

	LIMIT(n, 0, terminal.bot-orig+1);

	tsetdirt(orig, terminal.bot-n);
	tclearregion(0, terminal.bot-n+1, terminal.col-1, terminal.bot);

	for (i = terminal.bot; i >= orig+n; i--) {
		temp = terminal.line[i];
		terminal.line[i] = terminal.line[i-n];
		terminal.line[i-n] = temp;
	}

	selscroll(orig, n);
}

void
tscrollup(int orig, int n)
{
	int i;
	xelt_Line temp;

	LIMIT(n, 0, terminal.bot-orig+1);

	tclearregion(0, orig, terminal.col-1, orig+n-1);
	tsetdirt(orig+n, terminal.bot);

	for (i = orig; i <= terminal.bot-n; i++) {
		temp = terminal.line[i];
		terminal.line[i] = terminal.line[i+n];
		terminal.line[i+n] = temp;
	}

	selscroll(orig, -n);
}

void
selscroll(int orig, int n)
{
	if (sel.ob.x == -1)
		return;

	if (BETWEEN(sel.ob.y, orig, terminal.bot) || BETWEEN(sel.oe.y, orig, terminal.bot)) {
		if ((sel.ob.y += n) > terminal.bot || (sel.oe.y += n) < terminal.top) {
			selclear(NULL);
			return;
		}
		if (sel.type == XELT_SEL_RECTANGULAR) {
			if (sel.ob.y < terminal.top)
				sel.ob.y = terminal.top;
			if (sel.oe.y > terminal.bot)
				sel.oe.y = terminal.bot;
		} else {
			if (sel.ob.y < terminal.top) {
				sel.ob.y = terminal.top;
				sel.ob.x = 0;
			}
			if (sel.oe.y > terminal.bot) {
				sel.oe.y = terminal.bot;
				sel.oe.x = terminal.col;
			}
		}
		selnormalize();
	}
}

void
tnewline(int first_col)
{
	int y = terminal.cursor.y;

	if (y == terminal.bot) {
		tscrollup(terminal.top, 1);
	} else {
		y++;
	}
	tmoveto(first_col ? 0 : terminal.cursor.x, y);
}

void
csiparse(void)
{
	char *p = csiescseq.buf, *np;
	long int v;

	csiescseq.narg = 0;
	if (*p == '?') {
		csiescseq.priv = 1;
		p++;
	}

	csiescseq.buf[csiescseq.len] = '\0';
	while (p < csiescseq.buf+csiescseq.len) {
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		if (v == LONG_MAX || v == LONG_MIN)
			v = -1;
		csiescseq.arg[csiescseq.narg++] = v;
		p = np;
		if (*p != ';' || csiescseq.narg == XELT_ESC_ARG_SIZ)
			break;
		p++;
	}
	csiescseq.mode[0] = *p++;
	csiescseq.mode[1] = (p < csiescseq.buf+csiescseq.len) ? *p : '\0';
}

/* for absolute user moves, when decom is set */
void
tmoveato(int x, int y)
{
	tmoveto(x, y + ((terminal.cursor.state & XELT_CURSOR_ORIGIN) ? terminal.top: 0));
}

void
tmoveto(int x, int y)
{
	int miny, maxy;

	if (terminal.cursor.state & XELT_CURSOR_ORIGIN) {
		miny = terminal.top;
		maxy = terminal.bot;
	} else {
		miny = 0;
		maxy = terminal.row - 1;
	}
	terminal.cursor.state &= ~XELT_CURSOR_WRAPNEXT;
	terminal.cursor.x = LIMIT(x, 0, terminal.col-1);
	terminal.cursor.y = LIMIT(y, miny, maxy);
}

void
tsetchar(xelt_CharCode u, xelt_Glyph *attr, int x, int y)
{
	static char *vt100_0[62] = { /* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
		0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
		0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
		0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
	};

	/*
	 * The table is proudly stolen from rxvt.
	 */
	if (terminal.trantbl[terminal.charset] == XELT_CHARSET_GRAPHIC0 &&
	   BETWEEN(u, 0x41, 0x7e) && vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, XELT_SIZE_UTF);

	if (terminal.line[y][x].mode & XELT_ATTR_WIDE) {
		if (x+1 < terminal.col) {
			terminal.line[y][x+1].u = ' ';
			terminal.line[y][x+1].mode &= ~XELT_ATTR_WDUMMY;
		}
	} else if (terminal.line[y][x].mode & XELT_ATTR_WDUMMY) {
		terminal.line[y][x-1].u = ' ';
		terminal.line[y][x-1].mode &= ~XELT_ATTR_WIDE;
	}

	terminal.dirty[y] = 1;
	terminal.line[y][x] = *attr;
	terminal.line[y][x].u = u;
}

void
tclearregion(int x1, int y1, int x2, int y2)
{
	int x, y, temp;
	xelt_Glyph *gp;

	if (x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if (y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, terminal.col-1);
	LIMIT(x2, 0, terminal.col-1);
	LIMIT(y1, 0, terminal.row-1);
	LIMIT(y2, 0, terminal.row-1);

	for (y = y1; y <= y2; y++) {
		terminal.dirty[y] = 1;
		for (x = x1; x <= x2; x++) {
			gp = &terminal.line[y][x];
			if (selected(x, y))
				selclear(NULL);
			gp->fg = terminal.cursor.attr.fg;
			gp->bg = terminal.cursor.attr.bg;
			gp->mode = 0;
			gp->u = ' ';
		}
	}
}

void
tdeletechar(int n)
{
	int dst, src, size;
	xelt_Glyph *line;

	LIMIT(n, 0, terminal.col - terminal.cursor.x);

	dst = terminal.cursor.x;
	src = terminal.cursor.x + n;
	size = terminal.col - src;
	line = terminal.line[terminal.cursor.y];

	memmove(&line[dst], &line[src], size * sizeof(xelt_Glyph));
	tclearregion(terminal.col-n, terminal.cursor.y, terminal.col-1, terminal.cursor.y);
}

void
tinsertblank(int n)
{
	int dst, src, size;
	xelt_Glyph *line;

	LIMIT(n, 0, terminal.col - terminal.cursor.x);

	dst = terminal.cursor.x + n;
	src = terminal.cursor.x;
	size = terminal.col - dst;
	line = terminal.line[terminal.cursor.y];

	memmove(&line[dst], &line[src], size * sizeof(xelt_Glyph));
	tclearregion(src, terminal.cursor.y, dst - 1, terminal.cursor.y);
}

void
tinsertblankline(int n)
{
	if (BETWEEN(terminal.cursor.y, terminal.top, terminal.bot))
		tscrolldown(terminal.cursor.y, n);
}

void
tdeleteline(int n)
{
	if (BETWEEN(terminal.cursor.y, terminal.top, terminal.bot))
		tscrollup(terminal.cursor.y, n);
}

int32_t
tdefcolor(int *attr, int *npar, int l)
{
	int32_t idx = -1;
	xelt_uint r, g, b;

	switch (attr[*npar + 1]) {
	case 2: /* direct color in RGB space */
		if (*npar + 4 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		r = attr[*npar + 2];
		g = attr[*npar + 3];
		b = attr[*npar + 4];
		*npar += 4;
		if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
			fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n",
				r, g, b);
		else
			idx = TRUECOLOR(r, g, b);
		break;
	case 5: /* indexed color */
		if (*npar + 2 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		*npar += 2;
		if (!BETWEEN(attr[*npar], 0, 255))
			fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
		else
			idx = attr[*npar];
		break;
	case 0: /* implemented defined (only foreground) */
	case 1: /* transparent */
	case 3: /* direct color in CMY space */
	case 4: /* direct color in CMYK space */
	default:
		fprintf(stderr,
		        "erresc(38): gfx attr %d unknown\n", attr[*npar]);
		break;
	}

	return idx;
}

void
tsetattr(int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			terminal.cursor.attr.mode &= ~(
				XELT_ATTR_BOLD       |
				XELT_ATTR_FAINT      |
				XELT_ATTR_ITALIC     |
				XELT_ATTR_UNDERLINE  |
				XELT_ATTR_REVERSE    |
				XELT_ATTR_INVISIBLE  |
				XELT_ATTR_STRUCK     );
			terminal.cursor.attr.fg = defaultfg;
			terminal.cursor.attr.bg = defaultbg;
			break;
		case 1:
			terminal.cursor.attr.mode |= XELT_ATTR_BOLD;
			break;
		case 2:
			terminal.cursor.attr.mode |= XELT_ATTR_FAINT;
			break;
		case 3:
			terminal.cursor.attr.mode |= XELT_ATTR_ITALIC;
			break;
		case 4:
			terminal.cursor.attr.mode |= XELT_ATTR_UNDERLINE;
			break;
		case 7:
			terminal.cursor.attr.mode |= XELT_ATTR_REVERSE;
			break;
		case 8:
			terminal.cursor.attr.mode |= XELT_ATTR_INVISIBLE;
			break;
		case 9:
			terminal.cursor.attr.mode |= XELT_ATTR_STRUCK;
			break;
		case 22:
			terminal.cursor.attr.mode &= ~(XELT_ATTR_BOLD | XELT_ATTR_FAINT);
			break;
		case 23:
			terminal.cursor.attr.mode &= ~XELT_ATTR_ITALIC;
			break;
		case 24:
			terminal.cursor.attr.mode &= ~XELT_ATTR_UNDERLINE;
			break;
		case 27:
			terminal.cursor.attr.mode &= ~XELT_ATTR_REVERSE;
			break;
		case 28:
			terminal.cursor.attr.mode &= ~XELT_ATTR_INVISIBLE;
			break;
		case 29:
			terminal.cursor.attr.mode &= ~XELT_ATTR_STRUCK;
			break;
		case 38:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				terminal.cursor.attr.fg = idx;
			break;
		case 39:
			terminal.cursor.attr.fg = defaultfg;
			break;
		case 48:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				terminal.cursor.attr.bg = idx;
			break;
		case 49:
			terminal.cursor.attr.bg = defaultbg;
			break;
		default:
			if (BETWEEN(attr[i], 30, 37)) {
				terminal.cursor.attr.fg = attr[i] - 30;
			} else if (BETWEEN(attr[i], 40, 47)) {
				terminal.cursor.attr.bg = attr[i] - 40;
			} else if (BETWEEN(attr[i], 90, 97)) {
				terminal.cursor.attr.fg = attr[i] - 90 + 8;
			} else if (BETWEEN(attr[i], 100, 107)) {
				terminal.cursor.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]), csidump();
			}
			break;
		}
	}
}

void
tsetscroll(int t, int b)
{
	int temp;

	LIMIT(t, 0, terminal.row-1);
	LIMIT(b, 0, terminal.row-1);
	if (t > b) {
		temp = t;
		t = b;
		b = temp;
	}
	terminal.top = t;
	terminal.bot = b;
}

void
tsetmode(int priv, int set, int *args, int narg)
{
	int *lim, mode;
	int alt;

	for (lim = args + narg; args < lim; ++args) {
		if (priv) {
			switch (*args) {
			case 1: /* DECCKM -- Cursor key */
				MODBIT(terminal.mode, set, XELT_TERMINAL_APPCURSOR);
				break;
			case 5: /* DECSCNM -- Reverse video */
				mode = terminal.mode;
				MODBIT(terminal.mode, set, XELT_TERMINAL_REVERSE);
				if (mode != terminal.mode)
					redraw();
				break;
			case 6: /* DECOM -- Origin */
				MODBIT(terminal.cursor.state, set, XELT_CURSOR_ORIGIN);
				tmoveato(0, 0);
				break;
			case 7: /* DECAWM -- Auto wrap */
				MODBIT(terminal.mode, set, XELT_TERMINAL_WRAP);
				break;
			case 0:  /* Error (IGNORED) */
			case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
			case 3:  /* DECCOLM -- Column  (IGNORED) */
			case 4:  /* DECSCLM -- Scroll (IGNORED) */
			case 8:  /* DECARM -- Auto repeat (IGNORED) */
			case 18: /* DECPFF -- Printer feed (IGNORED) */
			case 19: /* DECPEX -- Printer extent (IGNORED) */
			case 42: /* DECNRCM -- National characters (IGNORED) */
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25: /* DECTCEM -- Text Cursor Enable Mode */
				MODBIT(terminal.mode, !set, XELT_TERMINAL_HIDE);
				break;
			case 9:    /* X10 mouse compatibility mode */
				xsetpointermotion(0);
				MODBIT(terminal.mode, 0, XELT_TERMINAL_MOUSE);
				MODBIT(terminal.mode, set, XELT_TERMINAL_MOUSEX10);
				break;
			case 1000: /* 1000: report button press */
				xsetpointermotion(0);
				MODBIT(terminal.mode, 0, XELT_TERMINAL_MOUSE);
				MODBIT(terminal.mode, set, XELT_TERMINAL_MOUSEBTN);
				break;
			case 1002: /* 1002: report motion on button press */
				xsetpointermotion(0);
				MODBIT(terminal.mode, 0, XELT_TERMINAL_MOUSE);
				MODBIT(terminal.mode, set, XELT_TERMINAL_MOUSEMOTION);
				break;
			case 1003: /* 1003: enable all mouse motions */
				xsetpointermotion(set);
				MODBIT(terminal.mode, 0, XELT_TERMINAL_MOUSE);
				MODBIT(terminal.mode, set, XELT_TERMINAL_MOUSEMANY);
				break;
			case 1004: /* 1004: send focus events to tty */
				MODBIT(terminal.mode, set, XELT_TERMINAL_FOCUS);
				break;
			case 1006: /* 1006: extended reporting mode */
				MODBIT(terminal.mode, set, XELT_TERMINAL_MOUSESGR);
				break;
			case 1034:
				MODBIT(terminal.mode, set, XELT_TERMINAL_8BIT);
				break;
			case 1049: /* swap screen & set/restore cursor as xterm */
				if (!allowaltscreen)
					break;
				tcursor((set) ? XELT_CURSOR_SAVE : XELT_CURSOR_LOAD);
				/* FALLTHROUGH */
			case 47: /* swap screen */
			case 1047:
				if (!allowaltscreen)
					break;
				alt = IS_SET(XELT_TERMINAL_ALTSCREEN);
				if (alt) {
					tclearregion(0, 0, terminal.col-1,
							terminal.row-1);
				}
				if (set ^ alt) /* set is always 1 or 0 */
					tswapscreen();
				if (*args != 1049)
					break;
				/* FALLTHROUGH */
			case 1048:
				tcursor((set) ? XELT_CURSOR_SAVE : XELT_CURSOR_LOAD);
				break;
			case 2004: /* 2004: bracketed paste mode */
				MODBIT(terminal.mode, set, XELT_TERMINAL_BRCKTPASTE);
				break;
			/* Not implemented mouse modes. See comments there. */
			case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
			case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
			case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else {
			switch (*args) {
			case 0:  /* Error (IGNORED) */
				break;
			case 2:  /* KAM -- keyboard action */
				MODBIT(terminal.mode, set, XELT_TERMINAL_KBDLOCK);
				break;
			case 4:  /* IRM -- Insertion-replacement */
				MODBIT(terminal.mode, set, XELT_TERMINAL_INSERT);
				break;
			case 12: /* SRM -- Send/Receive */
				MODBIT(terminal.mode, !set, XELT_TERMINAL_ECHO);
				break;
			case 20: /* LNM -- Linefeed/new line */
				MODBIT(terminal.mode, set, XELT_TERMINAL_CRLF);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
}

void
csihandle(void)
{
	char buf[40];
	int len;

	switch (csiescseq.mode[0]) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump();
		/* printAndExit(""); */
		break;
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblank(csiescseq.arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(terminal.cursor.x, terminal.cursor.y-csiescseq.arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
	case 'e': /* VPR --Cursor <n> Down */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(terminal.cursor.x, terminal.cursor.y+csiescseq.arg[0]);
		break;
	case 'i': /* MC -- Media Copy */
		switch (csiescseq.arg[0]) {
		case 0:
			tdump();
			break;
		case 1:
			tdumpline(terminal.cursor.y);
			break;
		case 2:
			tdumpsel();
			break;
		case 4:
			terminal.mode &= ~XELT_TERMINAL_PRINT;
			break;
		case 5:
			terminal.mode |= XELT_TERMINAL_PRINT;
			break;
		}
		break;
	case 'c': /* DA -- Device Attributes */
		if (csiescseq.arg[0] == 0)
			ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a': /* HPR -- Cursor <n> Forward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(terminal.cursor.x+csiescseq.arg[0], terminal.cursor.y);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(terminal.cursor.x-csiescseq.arg[0], terminal.cursor.y);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, terminal.cursor.y+csiescseq.arg[0]);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, terminal.cursor.y-csiescseq.arg[0]);
		break;
	case 'g': /* TBC -- Tabulation clear */
		switch (csiescseq.arg[0]) {
		case 0: /* clear current tab stop */
			terminal.tabs[terminal.cursor.x] = 0;
			break;
		case 3: /* clear all the tabs */
			memset(terminal.tabs, 0, terminal.col * sizeof(*terminal.tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* HPA */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(csiescseq.arg[0]-1, terminal.cursor.y);
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* HVP */
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], 1);
		tmoveato(csiescseq.arg[1]-1, csiescseq.arg[0]-1);
		break;
	case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(csiescseq.arg[0]);
		break;
	case 'J': /* ED -- Clear screen */
		selclear(NULL);
		switch (csiescseq.arg[0]) {
		case 0: /* below */
			tclearregion(terminal.cursor.x, terminal.cursor.y, terminal.col-1, terminal.cursor.y);
			if (terminal.cursor.y < terminal.row-1) {
				tclearregion(0, terminal.cursor.y+1, terminal.col-1,
						terminal.row-1);
			}
			break;
		case 1: /* above */
			if (terminal.cursor.y > 1)
				tclearregion(0, 0, terminal.col-1, terminal.cursor.y-1);
			tclearregion(0, terminal.cursor.y, terminal.cursor.x, terminal.cursor.y);
			break;
		case 2: /* all */
			tclearregion(0, 0, terminal.col-1, terminal.row-1);
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- Clear line */
		switch (csiescseq.arg[0]) {
		case 0: /* right */
			tclearregion(terminal.cursor.x, terminal.cursor.y, terminal.col-1,
					terminal.cursor.y);
			break;
		case 1: /* left */
			tclearregion(0, terminal.cursor.y, terminal.cursor.x, terminal.cursor.y);
			break;
		case 2: /* all */
			tclearregion(0, terminal.cursor.y, terminal.col-1, terminal.cursor.y);
			break;
		}
		break;
	case 'S': /* SU -- Scroll <n> line up */
		DEFAULT(csiescseq.arg[0], 1);
		tscrollup(terminal.top, csiescseq.arg[0]);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(csiescseq.arg[0], 1);
		tscrolldown(terminal.top, csiescseq.arg[0]);
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblankline(csiescseq.arg[0]);
		break;
	case 'l': /* RM -- Reset Mode */
		tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
		break;
	case 'M': /* DL -- Delete <n> lines */
		DEFAULT(csiescseq.arg[0], 1);
		tdeleteline(csiescseq.arg[0]);
		break;
	case 'X': /* ECH -- Erase <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tclearregion(terminal.cursor.x, terminal.cursor.y,
				terminal.cursor.x + csiescseq.arg[0] - 1, terminal.cursor.y);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(csiescseq.arg[0]);
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(-csiescseq.arg[0]);
		break;
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveato(terminal.cursor.x, csiescseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		tsetattr(csiescseq.arg, csiescseq.narg);
		break;
	case 'n': /* DSR – Device Status Report (cursor position) */
		if (csiescseq.arg[0] == 6) {
			len = snprintf(buf, sizeof(buf),"\033[%i;%iR",
					terminal.cursor.y+1, terminal.cursor.x+1);
			ttywrite(buf, len);
		}
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if (csiescseq.priv) {
			goto unknown;
		} else {
			DEFAULT(csiescseq.arg[0], 1);
			DEFAULT(csiescseq.arg[1], terminal.row);
			tsetscroll(csiescseq.arg[0]-1, csiescseq.arg[1]-1);
			tmoveato(0, 0);
		}
		break;
	case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
		tcursor(XELT_CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
		tcursor(XELT_CURSOR_LOAD);
		break;
	case ' ':
		switch (csiescseq.mode[1]) {
		case 'q': /* DECSCUSR -- Set Cursor Style */
			DEFAULT(csiescseq.arg[0], 1);
			if (!BETWEEN(csiescseq.arg[0], 0, 6)) {
				goto unknown;
			}
			xelt_windowmain.cursorstyle = csiescseq.arg[0];
			break;
		default:
			goto unknown;
		}
		break;
	}
}

void
csidump(void)
{
	int i;
	xelt_uint c;

	printf("ESC[");
	for (i = 0; i < csiescseq.len; i++) {
		c = csiescseq.buf[i] & 0xff;
		if (isprint(c)) {
			putchar(c);
		} else if (c == '\n') {
			printf("(\\n)");
		} else if (c == '\r') {
			printf("(\\r)");
		} else if (c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	putchar('\n');
}

void
csireset(void)
{
	memset(&csiescseq, 0, sizeof(csiescseq));
}

void
strhandle(void)
{
	char *p = NULL;
	int j, narg, par;

	terminal.esc &= ~(XELT_ESC_STR_END|XELT_ESC_STR);
	strparse();
	par = (narg = strescseq.narg) ? atoi(strescseq.args[0]) : 0;

	switch (strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch (par) {
		case 0:
		case 1:
		case 2:
			if (narg > 1)
				xsettitle(strescseq.args[1]);
			return;
		case 4: /* color set */
			if (narg < 3)
				break;
			p = strescseq.args[2];
			/* FALLTHROUGH */
		case 104: /* color reset, here p = NULL */
			j = (narg > 1) ? atoi(strescseq.args[1]) : -1;
			if (xsetcolorname(j, p)) {
				fprintf(stderr, "erresc: invalid color %s\n", p);
			} else {
				/*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
				redraw();
			}
			return;
		}
		break;
	case 'k': /* old title set compatibility */
		xsettitle(strescseq.args[0]);
		return;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
		return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump();
}

void
strparse(void)
{
	int c;
	char *p = strescseq.buf;

	strescseq.narg = 0;
	strescseq.buf[strescseq.len] = '\0';

	if (*p == '\0')
		return;

	while (strescseq.narg < XELT_SIZE_STR_ARG) {
		strescseq.args[strescseq.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

void
strdump(void)
{
	int i;
	xelt_uint c;

	printf("ESC%c", strescseq.type);
	for (i = 0; i < strescseq.len; i++) {
		c = strescseq.buf[i] & 0xff;
		if (c == '\0') {
			return;
		} else if (isprint(c)) {
			putchar(c);
		} else if (c == '\n') {
			printf("(\\n)");
		} else if (c == '\r') {
			printf("(\\r)");
		} else if (c == 0x1b) {
			printf("(\\e)");
		} else {
			printf("(%02x)", c);
		}
	}
	printf("ESC\\\n");
}

void
strreset(void)
{
	memset(&strescseq, 0, sizeof(strescseq));
}

void
sendbreak(const xelt_Arg *arg)
{
	if (tcsendbreak(cmdfd, 0))
		perror("Error sending break");
}

void
tprinter(char *s, size_t len)
{
	if (iofd != -1 && xwrite(iofd, s, len) < 0) {
		fprintf(stderr, "Error writing in %s:%s\n",
			opt_io, strerror(errno));
		close(iofd);
		iofd = -1;
	}
}

void
toggleprinter(const xelt_Arg *arg)
{
	terminal.mode ^= XELT_TERMINAL_PRINT;
}

void
printscreen(const xelt_Arg *arg)
{
	tdump();
}

void
printsel(const xelt_Arg *arg)
{
	tdumpsel();
}

void
tdumpsel(void)
{
	char *ptr;

	if ((ptr = getsel())) {
		tprinter(ptr, strlen(ptr));
		free(ptr);
	}
}

void
tdumpline(int n)
{
	char buf[XELT_SIZE_UTF];
	xelt_Glyph *bp, *end;

	bp = &terminal.line[n][0];
	end = &bp[MIN(tlinelen(n), terminal.col) - 1];
	if (bp != end || bp->u != ' ') {
		for ( ;bp <= end; ++bp)
			tprinter(buf, utf8encode(bp->u, buf));
	}
	tprinter("\n", 1);
}

void
tdump(void)
{
	int i;

	for (i = 0; i < terminal.row; ++i)
		tdumpline(i);
}

void
tputtab(int n)
{
	xelt_uint x = terminal.cursor.x;

	if (n > 0) {
		while (x < terminal.col && n--)
			for (++x; x < terminal.col && !terminal.tabs[x]; ++x)
				/* nothing */ ;
	} else if (n < 0) {
		while (x > 0 && n++)
			for (--x; x > 0 && !terminal.tabs[x]; --x)
				/* nothing */ ;
	}
	terminal.cursor.x = LIMIT(x, 0, terminal.col-1);
}

void
techo(xelt_CharCode u)
{
	if (ISCONTROL(u)) { /* control code */
		if (u & 0x80) {
			u &= 0x7f;
			tputc('^');
			tputc('[');
		} else if (u != '\n' && u != '\r' && u != '\t') {
			u ^= 0x40;
			tputc('^');
		}
	}
	tputc(u);
}

void
tdeftran(char ascii)
{
	static char cs[] = "0B";
	static int vcs[] = {XELT_CHARSET_GRAPHIC0, XELT_CHARSET_USA};
	char *p;

	if ((p = strchr(cs, ascii)) == NULL) {
		fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
	} else {
		terminal.trantbl[terminal.icharset] = vcs[p - cs];
	}
}

void
tdectest(char c)
{
	int x, y;

	if (c == '8') { /* DEC screen alignment test. */
		for (x = 0; x < terminal.col; ++x) {
			for (y = 0; y < terminal.row; ++y)
				tsetchar('E', &terminal.cursor.attr, x, y);
		}
	}
}

void
tstrsequence(xelt_uchar c)
{
	switch (c) {
	case 0x90:   /* DCS -- Device Control String */
		c = 'P';
		break;
	case 0x9f:   /* APC -- Application Program Command */
		c = '_';
		break;
	case 0x9e:   /* PM -- Privacy Message */
		c = '^';
		break;
	case 0x9d:   /* OSC -- Operating System Command */
		c = ']';
		break;
	}
	strreset();
	strescseq.type = c;
	terminal.esc |= XELT_ESC_STR;
}

void
tcontrolcode(xelt_uchar ascii)
{
	switch (ascii) {
	case '\t':   /* HT */
		tputtab(1);
		return;
	case '\b':   /* BS */
		tmoveto(terminal.cursor.x-1, terminal.cursor.y);
		return;
	case '\r':   /* CR */
		tmoveto(0, terminal.cursor.y);
		return;
	case '\f':   /* LF */
	case '\v':   /* VT */
	case '\n':   /* LF */
		/* go to first col if the mode is set */
		tnewline(IS_SET(XELT_TERMINAL_CRLF));
		return;
	case '\a':   /* BEL */
		if (terminal.esc & XELT_ESC_STR_END) {
			/* backwards compatibility to xterm */
			strhandle();
		} else {
			if (!(xelt_windowmain.state & XELT_WIN_FOCUSED))
				xseturgency(1);
			if (bellvolume)
				XkbBell(xelt_windowmain.display, xelt_windowmain.id, bellvolume, (Atom)NULL);
		}
		break;
	case '\033': /* ESC */
		csireset();
		terminal.esc &= ~(XELT_ESC_CSI|XELT_ESC_ALTCHARSET|XELT_ESC_TEST);
		terminal.esc |= XELT_ESC_START;
		return;
	case '\016': /* SO (LS1 -- Locking shift 1) */
	case '\017': /* SI (LS0 -- Locking shift 0) */
		terminal.charset = 1 - (ascii - '\016');
		return;
	case '\032': /* SUB */
		tsetchar('?', &terminal.cursor.attr, terminal.cursor.x, terminal.cursor.y);
	case '\030': /* CAN */
		csireset();
		break;
	case '\005': /* ENQ (IGNORED) */
	case '\000': /* NUL (IGNORED) */
	case '\021': /* XON (IGNORED) */
	case '\023': /* XOFF (IGNORED) */
	case 0177:   /* DEL (IGNORED) */
		return;
	case 0x80:   /* TODO: PAD */
	case 0x81:   /* TODO: HOP */
	case 0x82:   /* TODO: BPH */
	case 0x83:   /* TODO: NBH */
	case 0x84:   /* TODO: IND */
		break;
	case 0x85:   /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 0x86:   /* TODO: SSA */
	case 0x87:   /* TODO: ESA */
		break;
	case 0x88:   /* HTS -- Horizontal tab stop */
		terminal.tabs[terminal.cursor.x] = 1;
		break;
	case 0x89:   /* TODO: HTJ */
	case 0x8a:   /* TODO: VTS */
	case 0x8b:   /* TODO: PLD */
	case 0x8c:   /* TODO: PLU */
	case 0x8d:   /* TODO: RI */
	case 0x8e:   /* TODO: SS2 */
	case 0x8f:   /* TODO: SS3 */
	case 0x91:   /* TODO: PU1 */
	case 0x92:   /* TODO: PU2 */
	case 0x93:   /* TODO: STS */
	case 0x94:   /* TODO: CCH */
	case 0x95:   /* TODO: MW */
	case 0x96:   /* TODO: SPA */
	case 0x97:   /* TODO: EPA */
	case 0x98:   /* TODO: SOS */
	case 0x99:   /* TODO: SGCI */
		break;
	case 0x9a:   /* DECID -- Identify Terminal */
		ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 0x9b:   /* TODO: CSI */
	case 0x9c:   /* TODO: ST */
		break;
	case 0x90:   /* DCS -- Device Control String */
	case 0x9d:   /* OSC -- Operating System Command */
	case 0x9e:   /* PM -- Privacy Message */
	case 0x9f:   /* APC -- Application Program Command */
		tstrsequence(ascii);
		return;
	}
	/* only CAN, SUB, \a and C1 chars interrupt a sequence */
	terminal.esc &= ~(XELT_ESC_STR_END|XELT_ESC_STR);
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int
eschandle(xelt_uchar ascii)
{
	switch (ascii) {
	case '[':
		terminal.esc |= XELT_ESC_CSI;
		return 0;
	case '#':
		terminal.esc |= XELT_ESC_TEST;
		return 0;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	case ']': /* OSC -- Operating System Command */
	case 'k': /* old title set compatibility */
		tstrsequence(ascii);
		return 0;
	case 'n': /* LS2 -- Locking shift 2 */
	case 'o': /* LS3 -- Locking shift 3 */
		terminal.charset = 2 + (ascii - 'n');
		break;
	case '(': /* GZD4 -- set primary charset G0 */
	case ')': /* G1D4 -- set secondary charset G1 */
	case '*': /* G2D4 -- set tertiary charset G2 */
	case '+': /* G3D4 -- set quaternary charset G3 */
		terminal.icharset = ascii - '(';
		terminal.esc |= XELT_ESC_ALTCHARSET;
		return 0;
	case 'D': /* IND -- Linefeed */
		if (terminal.cursor.y == terminal.bot) {
			tscrollup(terminal.top, 1);
		} else {
			tmoveto(terminal.cursor.x, terminal.cursor.y+1);
		}
		break;
	case 'E': /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 'H': /* HTS -- Horizontal tab stop */
		terminal.tabs[terminal.cursor.x] = 1;
		break;
	case 'M': /* RI -- Reverse index */
		if (terminal.cursor.y == terminal.top) {
			tscrolldown(terminal.top, 1);
		} else {
			tmoveto(terminal.cursor.x, terminal.cursor.y-1);
		}
		break;
	case 'Z': /* DECID -- Identify Terminal */
		ttywrite(vtiden, sizeof(vtiden) - 1);
		break;
	case 'c': /* RIS -- Reset to inital state */
		treset();
		window_title_set();
		xloadcols();
		break;
	case '=': /* DECPAM -- Application keypad */
		terminal.mode |= XELT_TERMINAL_APPKEYPAD;
		break;
	case '>': /* DECPNM -- Normal keypad */
		terminal.mode &= ~XELT_TERMINAL_APPKEYPAD;
		break;
	case '7': /* DECSC -- Save Cursor */
		tcursor(XELT_CURSOR_SAVE);
		break;
	case '8': /* DECRC -- Restore Cursor */
		tcursor(XELT_CURSOR_LOAD);
		break;
	case '\\': /* ST -- String Terminator */
		if (terminal.esc & XELT_ESC_STR_END)
			strhandle();
		break;
	default:
		fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
			(xelt_uchar) ascii, isprint(ascii)? ascii:'.');
		break;
	}
	return 1;
}

void
tputc(xelt_CharCode u)
{
	char c[XELT_SIZE_UTF];
	int control;
	int width, len;
	xelt_Glyph *gp;

	control = ISCONTROL(u);
	len = utf8encode(u, c);
	if (!control && (width = wcwidth(u)) == -1) {
		memcpy(c, "\357\277\275", 4); /* XELT_SIZE_UTF_INVALID */
		width = 1;
	}

	if (IS_SET(XELT_TERMINAL_PRINT))
		tprinter(c, len);

	/*
	 * STR sequence must be checked before anything else
	 * because it uses all following characters until it
	 * receives a ESC, a SUB, a ST or any other C1 control
	 * character.
	 */
	if (terminal.esc & XELT_ESC_STR) {
		if (u == '\a' || u == 030 || u == 032 || u == 033 ||
		   ISCONTROLC1(u)) {
			terminal.esc &= ~(XELT_ESC_START|XELT_ESC_STR);
			terminal.esc |= XELT_ESC_STR_END;
		} else if (strescseq.len + len < sizeof(strescseq.buf) - 1) {
			memmove(&strescseq.buf[strescseq.len], c, len);
			strescseq.len += len;
			return;
		} else {
		/*
		 * Here is a bug in terminals. If the user never sends
		 * some code to stop the str or esc command, then st
		 * will stop responding. But this is better than
		 * silently failing with unknown characters. At least
		 * then users will report back.
		 *
		 * In the case users ever get fixed, here is the code:
		 */
		/*
		 * terminal.esc = 0;
		 * strhandle();
		 */
			return;
		}
	}

	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if (control) {
		tcontrolcode(u);
		/*
		 * control codes are not shown ever
		 */
		return;
	} else if (terminal.esc & XELT_ESC_START) {
		if (terminal.esc & XELT_ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = u;
			if (BETWEEN(u, 0x40, 0x7E)
					|| csiescseq.len >= \
					sizeof(csiescseq.buf)-1) {
				terminal.esc = 0;
				csiparse();
				csihandle();
			}
			return;
		} else if (terminal.esc & XELT_ESC_ALTCHARSET) {
			tdeftran(u);
		} else if (terminal.esc & XELT_ESC_TEST) {
			tdectest(u);
		} else {
			if (!eschandle(u))
				return;
			/* sequence already finished */
		}
		terminal.esc = 0;
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}
	if (sel.ob.x != -1 && BETWEEN(terminal.cursor.y, sel.ob.y, sel.oe.y))
		selclear(NULL);

	gp = &terminal.line[terminal.cursor.y][terminal.cursor.x];
	if (IS_SET(XELT_TERMINAL_WRAP) && (terminal.cursor.state & XELT_CURSOR_WRAPNEXT)) {
		gp->mode |= XELT_ATTR_WRAP;
		tnewline(1);
		gp = &terminal.line[terminal.cursor.y][terminal.cursor.x];
	}

	if (IS_SET(XELT_TERMINAL_INSERT) && terminal.cursor.x+width < terminal.col)
		memmove(gp+width, gp, (terminal.col - terminal.cursor.x - width) * sizeof(xelt_Glyph));

	if (terminal.cursor.x+width > terminal.col) {
		tnewline(1);
		gp = &terminal.line[terminal.cursor.y][terminal.cursor.x];
	}

	tsetchar(u, &terminal.cursor.attr, terminal.cursor.x, terminal.cursor.y);

	if (width == 2) {
		gp->mode |= XELT_ATTR_WIDE;
		if (terminal.cursor.x+1 < terminal.col) {
			gp[1].u = '\0';
			gp[1].mode = XELT_ATTR_WDUMMY;
		}
	}
	if (terminal.cursor.x+width < terminal.col) {
		tmoveto(terminal.cursor.x+width, terminal.cursor.y);
	} else {
		terminal.cursor.state |= XELT_CURSOR_WRAPNEXT;
	}
}

void
tresize(int col, int row)
{
	int i;
	int minrow = MIN(row, terminal.row);
	int mincol = MIN(col, terminal.col);
	int *bp;
	xelt_TCursor c;

	if (col < 1 || row < 1) {
		fprintf(stderr,
		        "tresize: error resizing to %dx%d\n", col, row);
		return;
	}

	/*
	 * slide screen to keep cursor where we expect it -
	 * tscrollup would work here, but we can optimize to
	 * memmove because we're freeing the earlier lines
	 */
	for (i = 0; i <= terminal.cursor.y - row; i++) {
		free(terminal.line[i]);
		free(terminal.alt[i]);
	}
	/* ensure that both src and dst are not NULL */
	if (i > 0) {
		memmove(terminal.line, terminal.line + i, row * sizeof(xelt_Line));
		memmove(terminal.alt, terminal.alt + i, row * sizeof(xelt_Line));
	}
	for (i += row; i < terminal.row; i++) {
		free(terminal.line[i]);
		free(terminal.alt[i]);
	}

	/* resize to new width */
	terminal.specbuf = xrealloc(terminal.specbuf, col * sizeof(XftGlyphFontSpec));

	/* resize to new height */
	terminal.line = xrealloc(terminal.line, row * sizeof(xelt_Line));
	terminal.alt  = xrealloc(terminal.alt,  row * sizeof(xelt_Line));
	terminal.dirty = xrealloc(terminal.dirty, row * sizeof(*terminal.dirty));
	terminal.tabs = xrealloc(terminal.tabs, col * sizeof(*terminal.tabs));

	/* resize each row to new width, zero-pad if needed */
	for (i = 0; i < minrow; i++) {
		terminal.line[i] = xrealloc(terminal.line[i], col * sizeof(xelt_Glyph));
		terminal.alt[i]  = xrealloc(terminal.alt[i],  col * sizeof(xelt_Glyph));
	}

	/* allocate any new rows */
	for (/* i == minrow */; i < row; i++) {
		terminal.line[i] = xmalloc(col * sizeof(xelt_Glyph));
		terminal.alt[i] = xmalloc(col * sizeof(xelt_Glyph));
	}
	if (col > terminal.col) {
		bp = terminal.tabs + terminal.col;

		memset(bp, 0, sizeof(*terminal.tabs) * (col - terminal.col));
		while (--bp > terminal.tabs && !*bp)
			/* nothing */ ;
		for (bp += tabspaces; bp < terminal.tabs + col; bp += tabspaces)
			*bp = 1;
	}
	/* update terminal size */
	terminal.col = col;
	terminal.row = row;
	/* reset scrolling region */
	tsetscroll(0, row-1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(terminal.cursor.x, terminal.cursor.y);
	/* Clearing both screens (it makes dirty all lines) */
	c = terminal.cursor;
	for (i = 0; i < 2; i++) {
		if (mincol < col && 0 < minrow) {
			tclearregion(mincol, 0, col - 1, minrow - 1);
		}
		if (0 < col && minrow < row) {
			tclearregion(0, minrow, col - 1, row - 1);
		}
		tswapscreen();
		tcursor(XELT_CURSOR_LOAD);
	}
	terminal.cursor = c;
}

void
xresize(int col, int row)
{
	xelt_windowmain.ttywidth = MAX(1, col * xelt_windowmain.charwidth);
	xelt_windowmain.ttyheight = MAX(1, row * xelt_windowmain.charheight);

	XFreePixmap(xelt_windowmain.display, xelt_windowmain.drawbuf);
	xelt_windowmain.drawbuf = XCreatePixmap(xelt_windowmain.display, xelt_windowmain.id, xelt_windowmain.width, xelt_windowmain.height, xelt_windowmain.depth);
	XftDrawChange(xelt_windowmain.draw, xelt_windowmain.drawbuf);
	xclear(0, 0, xelt_windowmain.width, xelt_windowmain.height);
}

xelt_ushort
sixd_to_16bit(int x)
{
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

int
xloadcolor(int i, const char *name, xelt_Color *ncolor)
{
	XRenderColor color = { .alpha = 0xffff };

	if (!name) {
		if (BETWEEN(i, 16, 255)) { /* 256 color */
			if (i < 6*6*6+16) { /* same colors as xterm */
				color.red   = sixd_to_16bit( ((i-16)/36)%6 );
				color.green = sixd_to_16bit( ((i-16)/6) %6 );
				color.blue  = sixd_to_16bit( ((i-16)/1) %6 );
			} else { /* greyscale */
				color.red = 0x0808 + 0x0a0a * (i - (6*6*6+16));
				color.green = color.blue = color.red;
			}
			return XftColorAllocValue(xelt_windowmain.display, xelt_windowmain.vis,
			                          xelt_windowmain.colormap, &color, ncolor);
		} else
			name = colorname[i];
	}

	return XftColorAllocName(xelt_windowmain.display, xelt_windowmain.vis, xelt_windowmain.colormap, name, ncolor);
}

void
xloadcols(void)
{
	int i;
	static int loaded;
	xelt_Color *cp;

	if (loaded) {
		for (cp = dc.col; cp < &dc.col[LEN(dc.col)]; ++cp)
			XftColorFree(xelt_windowmain.display, xelt_windowmain.vis, xelt_windowmain.colormap, cp);
	}

	for (i = 0; i < LEN(dc.col); i++)
		if (!xloadcolor(i, NULL, &dc.col[i])) {
			if (colorname[i])
				printAndExit("Could not allocate color '%s'\n", colorname[i]);
			else
				printAndExit("Could not allocate color %d\n", i);
		}

    /* set alpha value of bg color */
	if (USE_ARGB) {
		dc.col[defaultbg].color.alpha = (0xffff * alpha) / XELT_SIZE_OPAQUE; //0xcccc;
		dc.col[defaultbg].pixel &= 0x00111111;
		dc.col[defaultbg].pixel |= alpha << 24; // 0xcc000000;
	}

	loaded = 1;
}

int
xsetcolorname(int x, const char *name)
{
	xelt_Color ncolor;

	if (!BETWEEN(x, 0, LEN(dc.col)))
		return 1;


	if (!xloadcolor(x, name, &ncolor))
		return 1;

	XftColorFree(xelt_windowmain.display, xelt_windowmain.vis, xelt_windowmain.colormap, &dc.col[x]);
	dc.col[x] = ncolor;

	return 0;
}

/*
 * Absolute coordinates.
 */
void
xclear(int x1, int y1, int x2, int y2)
{
	XftDrawRect(xelt_windowmain.draw,
			&dc.col[IS_SET(XELT_TERMINAL_REVERSE)? defaultfg : defaultbg],
			x1, y1, x2-x1, y2-y1);
}

void
xhints(void)
{
	XClassHint class = {opt_name ? opt_name : termname,
	                    opt_class ? opt_class : termname};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints *sizeh = NULL;

	sizeh = XAllocSizeHints();

	sizeh->flags = PSize | PResizeInc | PBaseSize;
	sizeh->height = xelt_windowmain.height;
	sizeh->width = xelt_windowmain.width;
	sizeh->height_inc = xelt_windowmain.charheight;
	sizeh->width_inc = xelt_windowmain.charwidth;
	sizeh->base_height = 2 * borderpx;
	sizeh->base_width = 2 * borderpx;
	if (xelt_windowmain.isfixed) {
		sizeh->flags |= PMaxSize | PMinSize;
		sizeh->min_width = sizeh->max_width = xelt_windowmain.width;
		sizeh->min_height = sizeh->max_height = xelt_windowmain.height;
	}
	if (xelt_windowmain.gmask & (XValue|YValue)) {
		sizeh->flags |= USPosition | PWinGravity;
		sizeh->x = xelt_windowmain.left;
		sizeh->y = xelt_windowmain.top;
		sizeh->win_gravity = xgeommasktogravity(xelt_windowmain.gmask);
	}

	XSetWMProperties(xelt_windowmain.display, xelt_windowmain.id, NULL, NULL, NULL, 0, sizeh, &wm,
			&class);
	XFree(sizeh);
}

int
xgeommasktogravity(int mask)
{
	switch (mask & (XNegative|YNegative)) {
	case 0:
		return NorthWestGravity;
	case XNegative:
		return NorthEastGravity;
	case YNegative:
		return SouthWestGravity;
	}

	return SouthEastGravity;
}

int
xloadfont(xelt_Font *f, FcPattern *pattern)
{
	FcPattern *match;
	FcResult result;
	XGlyphInfo extents;

	match = XftFontMatch(xelt_windowmain.display, xelt_windowmain.scr, pattern, &result);
	if (!match)
		return 1;

	if (!(f->match = XftFontOpenPattern(xelt_windowmain.display, match))) {
		FcPatternDestroy(match);
		return 1;
	}

	XftTextExtentsUtf8(xelt_windowmain.display, f->match,
		(const FcChar8 *) ascii_printable,
		strlen(ascii_printable), &extents);

	f->set = NULL;
	f->pattern = FcPatternDuplicate(pattern);

	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance_width;

	f->height = f->ascent + f->descent;
	f->width = DIVCEIL(extents.xOff, strlen(ascii_printable));

	return 0;
}

void
xloadfonts(char *fontstr, double fontsize)
{
	FcPattern *pattern;
	double fontval;
	float ceilf(float);

	if (fontstr[0] == '-') {
		pattern = XftXlfdParse(fontstr, False, False);
	} else {
		pattern = FcNameParse((FcChar8 *)fontstr);
	}

	if (!pattern)
		printAndExit("st: can't open font %s\n", fontstr);

	if (fontsize > 1) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = fontval;
		} else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = -1;
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
		defaultfontsize = usedfontsize;
	}

	if (xloadfont(&dc.font, pattern))
		printAndExit("st: can't open font %s\n", fontstr);

	if (usedfontsize < 0) {
		FcPatternGetDouble(dc.font.match->pattern,
		                   FC_PIXEL_SIZE, 0, &fontval);
		usedfontsize = fontval;
		if (fontsize == 0)
			defaultfontsize = fontval;
	}

	/* Setting character width and height. */
	xelt_windowmain.charwidth = ceilf(dc.font.width * cwscale);
	xelt_windowmain.charheight = ceilf(dc.font.height * chscale);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if (xloadfont(&dc.ifont, pattern))
		printAndExit("st: can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (xloadfont(&dc.ibfont, pattern))
		printAndExit("st: can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if (xloadfont(&dc.bfont, pattern))
		printAndExit("st: can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);
}

void
xunloadfont(xelt_Font *f)
{
	XftFontClose(xelt_windowmain.display, f->match);
	FcPatternDestroy(f->pattern);
	if (f->set)
		FcFontSetDestroy(f->set);
}

void
xunloadfonts(void)
{
	/* Free the loaded fonts in the font cache.  */
	while (frclen > 0)
		XftFontClose(xelt_windowmain.display, frc[--frclen].font);

	xunloadfont(&dc.font);
	xunloadfont(&dc.bfont);
	xunloadfont(&dc.ifont);
	xunloadfont(&dc.ibfont);
}

void
xzoom(const xelt_Arg *arg)
{
	xelt_Arg larg;

	larg.f = usedfontsize + arg->f;
	xzoomabs(&larg);
}

void
xzoomabs(const xelt_Arg *arg)
{
	xunloadfonts();
	xloadfonts(usedfont, arg->f);
	cresize(0, 0);
	ttyresize();
	redraw();
	xhints();
}

void
xzoomreset(const xelt_Arg *arg)
{
	xelt_Arg larg;

	if (defaultfontsize > 0) {
		larg.f = defaultfontsize;
		xzoomabs(&larg);
	}
}

void
xinit(void)
{
	XGCValues gcvalues;
	Cursor cursor;
	Window parent;
	pid_t thispid = getpid();
	XColor xmousefg, xmousebg;

	if (!(xelt_windowmain.display = XOpenDisplay(NULL)))
		printAndExit("Can't open display\n");
	xelt_windowmain.scr = XDefaultScreen(xelt_windowmain.display);
	xelt_windowmain.depth = (USE_ARGB)? 32: XDefaultDepth(xelt_windowmain.display, xelt_windowmain.scr);
	if (! USE_ARGB)
		xelt_windowmain.vis = XDefaultVisual(xelt_windowmain.display, xelt_windowmain.scr);
	else {
		XVisualInfo *vis;
		XRenderPictFormat *fmt;
		int nvi;
		int i;

		XVisualInfo tpl = {
			.screen = xelt_windowmain.scr,
			.depth = 32,
			.class = TrueColor
		};

		vis = XGetVisualInfo(xelt_windowmain.display, VisualScreenMask | VisualDepthMask | VisualClassMask, &tpl, &nvi);
		xelt_windowmain.vis = NULL;
		for(i = 0; i < nvi; i ++) {
			fmt = XRenderFindVisualFormat(xelt_windowmain.display, vis[i].visual);
			if (fmt->type == PictTypeDirect && fmt->direct.alphaMask) {
				xelt_windowmain.vis = vis[i].visual;
				break;
			}
		}

		XFree(vis);

		if (! xelt_windowmain.vis) {
			fprintf(stderr, "Couldn't find ARGB visual.\n");
			exit(1);
		}
	}

	/* font */
	if (!FcInit())
		printAndExit("Could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	xloadfonts(usedfont, 0);

	/* colors */
	if (! USE_ARGB)
		xelt_windowmain.colormap = XDefaultColormap(xelt_windowmain.display, xelt_windowmain.scr);
	else
		xelt_windowmain.colormap = XCreateColormap(xelt_windowmain.display, XRootWindow(xelt_windowmain.display, xelt_windowmain.scr), xelt_windowmain.vis, None);
	xloadcols();

	/* adjust fixed window geometry */
	xelt_windowmain.width = 2 * borderpx + terminal.col * xelt_windowmain.charwidth;
	xelt_windowmain.height = 2 * borderpx + terminal.row * xelt_windowmain.charheight;
	if (xelt_windowmain.gmask & XNegative)
		xelt_windowmain.left += DisplayWidth(xelt_windowmain.display, xelt_windowmain.scr) - xelt_windowmain.width - 2;
	if (xelt_windowmain.gmask & YNegative)
		xelt_windowmain.top += DisplayHeight(xelt_windowmain.display, xelt_windowmain.scr) - xelt_windowmain.height - 2;

	/* Events */
	xelt_windowmain.attrs.background_pixel = dc.col[defaultbg].pixel;
	xelt_windowmain.attrs.border_pixel = dc.col[defaultbg].pixel;
	xelt_windowmain.attrs.bit_gravity = NorthWestGravity;
	xelt_windowmain.attrs.event_mask = FocusChangeMask | KeyPressMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	xelt_windowmain.attrs.colormap = xelt_windowmain.colormap;

	if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
		parent = XRootWindow(xelt_windowmain.display, xelt_windowmain.scr);
	xelt_windowmain.id = XCreateWindow(xelt_windowmain.display, parent, xelt_windowmain.left, xelt_windowmain.top,
			xelt_windowmain.width, xelt_windowmain.height, 0, xelt_windowmain.depth, InputOutput,
			xelt_windowmain.vis, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xelt_windowmain.attrs);

	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	xelt_windowmain.drawbuf = XCreatePixmap(xelt_windowmain.display, xelt_windowmain.id, xelt_windowmain.width, xelt_windowmain.height, xelt_windowmain.depth);
	dc.gc = XCreateGC(xelt_windowmain.display,
			(USE_ARGB)? xelt_windowmain.drawbuf: parent,
			GCGraphicsExposures,
			&gcvalues);
	XSetForeground(xelt_windowmain.display, dc.gc, dc.col[defaultbg].pixel);
	XFillRectangle(xelt_windowmain.display, xelt_windowmain.drawbuf, dc.gc, 0, 0, xelt_windowmain.width, xelt_windowmain.height);

	/* Xft rendering context */
	xelt_windowmain.draw = XftDrawCreate(xelt_windowmain.display, xelt_windowmain.drawbuf, xelt_windowmain.vis, xelt_windowmain.colormap);

	/* input methods */
	// open input method information
	if ((xelt_windowmain.inputmethod = XOpenIM(xelt_windowmain.display, NULL, NULL, NULL)) == NULL) {
		XSetLocaleModifiers("@im=local");
		if ((xelt_windowmain.inputmethod =  XOpenIM(xelt_windowmain.display, NULL, NULL, NULL)) == NULL) {
			XSetLocaleModifiers("@im=");
			if ((xelt_windowmain.inputmethod = XOpenIM(xelt_windowmain.display,
					NULL, NULL, NULL)) == NULL) {
				printAndExit("XOpenIM failed. Could not open input"
					" device.\n");
			}
		}
	}
	xelt_windowmain.inputcontext = XCreateIC(xelt_windowmain.inputmethod, XNInputStyle, XIMPreeditNothing
					   | XIMStatusNothing, XNClientWindow, xelt_windowmain.id,
					   XNFocusWindow, xelt_windowmain.id, NULL);
	if (xelt_windowmain.inputcontext == NULL)
		printAndExit("XCreateIC failed. Could not obtain input method.\n");

	/* white cursor, black outline */
	cursor = XCreateFontCursor(xelt_windowmain.display, mouseshape);
	XDefineCursor(xelt_windowmain.display, xelt_windowmain.id, cursor);

	if (XParseColor(xelt_windowmain.display, xelt_windowmain.colormap, colorname[mousefg], &xmousefg) == 0) {
		xmousefg.red   = 0xffff;
		xmousefg.green = 0xffff;
		xmousefg.blue  = 0xffff;
	}

	if (XParseColor(xelt_windowmain.display, xelt_windowmain.colormap, colorname[mousebg], &xmousebg) == 0) {
		xmousebg.red   = 0x0000;
		xmousebg.green = 0x0000;
		xmousebg.blue  = 0x0000;
	}

	XRecolorCursor(xelt_windowmain.display, cursor, &xmousefg, &xmousebg);

	xelt_windowmain.xembed = XInternAtom(xelt_windowmain.display, "_XEMBED", False);
	xelt_windowmain.wmdeletewin = XInternAtom(xelt_windowmain.display, "WM_DELETE_WINDOW", False);
	xelt_windowmain.netwmname = XInternAtom(xelt_windowmain.display, "_NET_WM_NAME", False);
	XSetWMProtocols(xelt_windowmain.display, xelt_windowmain.id, &xelt_windowmain.wmdeletewin, 1);

    	xelt_windowmain.netwmstate = XInternAtom(xelt_windowmain.display, "_NET_WM_STATE", False);
    	xelt_windowmain.netwmfullscreen = XInternAtom(xelt_windowmain.display, "_NET_WM_STATE_FULLSCREEN", False);

	xelt_windowmain.netwmpid = XInternAtom(xelt_windowmain.display, "_NET_WM_PID", False);
	XChangeProperty(xelt_windowmain.display, xelt_windowmain.id, xelt_windowmain.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (xelt_uchar *)&thispid, 1);

	window_title_set();
	XMapWindow(xelt_windowmain.display, xelt_windowmain.id);
	xhints();
	XSync(xelt_windowmain.display, False);
}

int
xmakeglyphfontspecs(XftGlyphFontSpec *specs, const xelt_Glyph *glyphs, int len, int x, int y)
{
	float winx = borderpx + x * xelt_windowmain.charwidth, winy = borderpx + y * xelt_windowmain.charheight, xp, yp;
	xelt_ushort mode, prevmode = USHRT_MAX;
	xelt_Font *font = &dc.font;
	int frcflags = XELT_FONTCACHE_NORMAL;
	float runewidth = xelt_windowmain.charwidth;
	xelt_CharCode rune;
	FT_UInt glyphidx;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	int i, f, numspecs = 0;

	for (i = 0, xp = winx, yp = winy + font->ascent; i < len; ++i) {
		/* Fetch rune and mode for current glyph. */
		rune = glyphs[i].u;
		mode = glyphs[i].mode;

		/* Skip dummy wide-character spacing. */
		if (mode == XELT_ATTR_WDUMMY)
			continue;

		/* Determine font for glyph if different from previous glyph. */
		if (prevmode != mode) {
			prevmode = mode;
			font = &dc.font;
			frcflags = XELT_FONTCACHE_NORMAL;
			runewidth = xelt_windowmain.charwidth * ((mode & XELT_ATTR_WIDE) ? 2.0f : 1.0f);
			if ((mode & XELT_ATTR_ITALIC) && (mode & XELT_ATTR_BOLD)) {
				font = &dc.ibfont;
				frcflags = XELT_FONTCACHE_ITALICBOLD;
			} else if (mode & XELT_ATTR_ITALIC) {
				font = &dc.ifont;
				frcflags = XELT_FONTCACHE_ITALIC;
			} else if (mode & XELT_ATTR_BOLD) {
				font = &dc.bfont;
				frcflags = XELT_FONTCACHE_BOLD;
			}
			yp = winy + font->ascent;
		}

		/* Lookup character index with default font. */
		glyphidx = XftCharIndex(xelt_windowmain.display, font->match, rune);
		if (glyphidx) {
			specs[numspecs].font = font->match;
			specs[numspecs].glyph = glyphidx;
			specs[numspecs].x = (short)xp;
			specs[numspecs].y = (short)yp;
			xp += runewidth;
			numspecs++;
			continue;
		}

		/* Fallback on font cache, search the font cache for match. */
		for (f = 0; f < frclen; f++) {
			glyphidx = XftCharIndex(xelt_windowmain.display, frc[f].font, rune);
			/* Everything correct. */
			if (glyphidx && frc[f].flags == frcflags)
				break;
			/* We got a default font for a not found glyph. */
			if (!glyphidx && frc[f].flags == frcflags
					&& frc[f].unicodep == rune) {
				break;
			}
		}

		/* Nothing was found. Use fontconfig to find matching font. */
		if (f >= frclen) {
			if (!font->set)
				font->set = FcFontSort(0, font->pattern,
				                       1, 0, &fcres);
			fcsets[0] = font->set;

			/*
			 * Nothing was found in the cache. Now use
			 * some dozen of Fontconfig calls to get the
			 * font for one single character.
			 *
			 * Xft and fontconfig are design failures.
			 */
			fcpattern = FcPatternDuplicate(font->pattern);
			fccharset = FcCharSetCreate();

			FcCharSetAddChar(fccharset, rune);
			FcPatternAddCharSet(fcpattern, FC_CHARSET,
					fccharset);
			FcPatternAddBool(fcpattern, FC_SCALABLE, 1);

			FcConfigSubstitute(0, fcpattern,
					FcMatchPattern);
			FcDefaultSubstitute(fcpattern);

			fontpattern = FcFontSetMatch(0, fcsets, 1,
					fcpattern, &fcres);

			/*
			 * Overwrite or create the new cache entry.
			 */
			if (frclen >= LEN(frc)) {
				frclen = LEN(frc) - 1;
				XftFontClose(xelt_windowmain.display, frc[frclen].font);
				frc[frclen].unicodep = 0;
			}

			frc[frclen].font = XftFontOpenPattern(xelt_windowmain.display,
					fontpattern);
			frc[frclen].flags = frcflags;
			frc[frclen].unicodep = rune;

			glyphidx = XftCharIndex(xelt_windowmain.display, frc[frclen].font, rune);

			f = frclen;
			frclen++;

			FcPatternDestroy(fcpattern);
			FcCharSetDestroy(fccharset);
		}

		specs[numspecs].font = frc[f].font;
		specs[numspecs].glyph = glyphidx;
		specs[numspecs].x = (short)xp;
		specs[numspecs].y = (short)yp;
		xp += runewidth;
		numspecs++;
	}

	return numspecs;
}

void
xdrawglyphfontspecs(const XftGlyphFontSpec *specs, xelt_Glyph base, int len, int x, int y)
{
	int charlen = len * ((base.mode & XELT_ATTR_WIDE) ? 2 : 1);
	int winx = borderpx + x * xelt_windowmain.charwidth, winy = borderpx + y * xelt_windowmain.charheight,
	    width = charlen * xelt_windowmain.charwidth;
	xelt_Color *fg, *bg, *temp, revfg, revbg, truefg, truebg;
	XRenderColor colfg, colbg;
	XRectangle r;

	/* Determine foreground and background colors based on mode. */
	if (base.fg == defaultfg) {
		if (base.mode & XELT_ATTR_ITALIC)
			base.fg = defaultitalic;
		else if ((base.mode & XELT_ATTR_ITALIC) && (base.mode & XELT_ATTR_BOLD))
			base.fg = defaultitalic;
		else if (base.mode & XELT_ATTR_UNDERLINE)
			base.fg = defaultunderline;
	}

	if (IS_TRUECOL(base.fg)) {
		colfg.alpha = 0xffff;
		colfg.red = TRUERED(base.fg);
		colfg.green = TRUEGREEN(base.fg);
		colfg.blue = TRUEBLUE(base.fg);
		XftColorAllocValue(xelt_windowmain.display, xelt_windowmain.vis, xelt_windowmain.colormap, &colfg, &truefg);
		fg = &truefg;
	} else {
		fg = &dc.col[base.fg];
	}

	if (IS_TRUECOL(base.bg)) {
		colbg.alpha = 0xffff;
		colbg.green = TRUEGREEN(base.bg);
		colbg.red = TRUERED(base.bg);
		colbg.blue = TRUEBLUE(base.bg);
		XftColorAllocValue(xelt_windowmain.display, xelt_windowmain.vis, xelt_windowmain.colormap, &colbg, &truebg);
		bg = &truebg;
	} else {
		bg = &dc.col[base.bg];
	}

	/* Change basic system colors [0-7] to bright system colors [8-15] */
	if ((base.mode & XELT_ATTR_BOLD_FAINT) == XELT_ATTR_BOLD && BETWEEN(base.fg, 0, 7))
		fg = &dc.col[base.fg];

	if (IS_SET(XELT_TERMINAL_REVERSE)) {
		if (fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xelt_windowmain.display, xelt_windowmain.vis, xelt_windowmain.colormap, &colfg,
					&revfg);
			fg = &revfg;
		}

		if (bg == &dc.col[defaultbg]) {
			bg = &dc.col[defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xelt_windowmain.display, xelt_windowmain.vis, xelt_windowmain.colormap, &colbg,
					&revbg);
			bg = &revbg;
		}
	}

	if (base.mode & XELT_ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

	if ((base.mode & XELT_ATTR_BOLD_FAINT) == XELT_ATTR_FAINT) {
		colfg.red = fg->color.red / 2;
		colfg.green = fg->color.green / 2;
		colfg.blue = fg->color.blue / 2;
		XftColorAllocValue(xelt_windowmain.display, xelt_windowmain.vis, xelt_windowmain.colormap, &colfg, &revfg);
		fg = &revfg;
	}

	if (base.mode & XELT_ATTR_INVISIBLE)
		fg = bg;

	/* Intelligent cleaning up of the borders. */
	if (x == 0) {
		xclear(0, (y == 0)? 0 : winy, borderpx,
			winy + xelt_windowmain.charheight + ((y >= terminal.row-1)? xelt_windowmain.height : 0));
	}
	if (x + charlen >= terminal.col) {
		xclear(winx + width, (y == 0)? 0 : winy, xelt_windowmain.width,
			((y >= terminal.row-1)? xelt_windowmain.height : (winy + xelt_windowmain.charheight)));
	}
	if (y == 0)
		xclear(winx, 0, winx + width, borderpx);
	if (y == terminal.row-1)
		xclear(winx, winy + xelt_windowmain.charheight, winx + width, xelt_windowmain.height);

	/* Clean up the region we want to draw to. */
	XftDrawRect(xelt_windowmain.draw, bg, winx, winy, width, xelt_windowmain.charheight);

	/* Set the clip region because Xft is sometimes dirty. */
	r.x = 0;
	r.y = 0;
	r.height = xelt_windowmain.charheight;
	r.width = width;
	XftDrawSetClipRectangles(xelt_windowmain.draw, winx, winy, &r, 1);

	/* Render the glyphs. */
	XftDrawGlyphFontSpec(xelt_windowmain.draw, fg, specs, len);

	/* Render underline and strikethrough. */
	if (base.mode & XELT_ATTR_UNDERLINE) {
		XftDrawRect(xelt_windowmain.draw, fg, winx, winy + dc.font.ascent + 1,
				width, 1);
	}

	if (base.mode & XELT_ATTR_STRUCK) {
		XftDrawRect(xelt_windowmain.draw, fg, winx, winy + 2 * dc.font.ascent / 3,
				width, 1);
	}

	/* Reset clip to none. */
	XftDrawSetClip(xelt_windowmain.draw, 0);
}

void
xdrawglyph(xelt_Glyph g, int x, int y)
{
	int numspecs;
	XftGlyphFontSpec spec;

	numspecs = xmakeglyphfontspecs(&spec, &g, 1, x, y);
	xdrawglyphfontspecs(&spec, g, numspecs, x, y);
}

void
xdrawcursor(void)
{
	static int oldx = 0, oldy = 0;
	int curx;
	xelt_Glyph g = {' ', XELT_ATTR_NULL, defaultbg, defaultcs}, og;
	int ena_sel = sel.ob.x != -1 && sel.alt == IS_SET(XELT_TERMINAL_ALTSCREEN);
	xelt_Color drawcol;

	LIMIT(oldx, 0, terminal.col-1);
	LIMIT(oldy, 0, terminal.row-1);

	curx = terminal.cursor.x;

	/* adjust position if in dummy */
	if (terminal.line[oldy][oldx].mode & XELT_ATTR_WDUMMY)
		oldx--;
	if (terminal.line[terminal.cursor.y][curx].mode & XELT_ATTR_WDUMMY)
		curx--;

	/* remove the old cursor */
	og = terminal.line[oldy][oldx];
	if (ena_sel && selected(oldx, oldy))
		og.mode ^= XELT_ATTR_REVERSE;
	xdrawglyph(og, oldx, oldy);

	g.u = terminal.line[terminal.cursor.y][terminal.cursor.x].u;

	/*
	 * Select the right color for the right mode.
	 */
	if (IS_SET(XELT_TERMINAL_REVERSE)) {
		g.mode |= XELT_ATTR_REVERSE;
		g.bg = defaultfg;
		if (ena_sel && selected(terminal.cursor.x, terminal.cursor.y)) {
			drawcol = dc.col[defaultcs];
			g.fg = defaultrcs;
		} else {
			drawcol = dc.col[defaultrcs];
			g.fg = defaultcs;
		}
	} else {
		if (ena_sel && selected(terminal.cursor.x, terminal.cursor.y)) {
			drawcol = dc.col[defaultrcs];
			g.fg = defaultfg;
			g.bg = defaultrcs;
		} else {
			drawcol = dc.col[defaultcs];
		}
	}

	if (IS_SET(XELT_TERMINAL_HIDE))
		return;

	/* draw the new one */
	if (xelt_windowmain.state & XELT_WIN_FOCUSED) {
		switch (xelt_windowmain.cursorstyle) {
		case 7: /* st extension: snowman */
			utf8decode("☃", &g.u, XELT_SIZE_UTF);
		case 0: /* Blinking Block */
		case 1: /* Blinking Block (Default) */
		case 2: /* Steady Block */
			g.mode |= terminal.line[terminal.cursor.y][curx].mode & XELT_ATTR_WIDE;
			xdrawglyph(g, terminal.cursor.x, terminal.cursor.y);
			break;
		case 3: /* Blinking Underline */
		case 4: /* Steady Underline */
			XftDrawRect(xelt_windowmain.draw, &drawcol,
					borderpx + curx * xelt_windowmain.charwidth,
					borderpx + (terminal.cursor.y + 1) * xelt_windowmain.charheight - \
						cursorthickness,
					xelt_windowmain.charwidth, cursorthickness);
			break;
		case 5: /* Blinking bar */
		case 6: /* Steady bar */
			XftDrawRect(xelt_windowmain.draw, &drawcol,
					borderpx + curx * xelt_windowmain.charwidth,
					borderpx + terminal.cursor.y * xelt_windowmain.charheight,
					cursorthickness, xelt_windowmain.charheight);
			break;
		}
	} else {
		XftDrawRect(xelt_windowmain.draw, &drawcol,
				borderpx + curx * xelt_windowmain.charwidth,
				borderpx + terminal.cursor.y * xelt_windowmain.charheight,
				xelt_windowmain.charwidth - 1, 1);
		XftDrawRect(xelt_windowmain.draw, &drawcol,
				borderpx + curx * xelt_windowmain.charwidth,
				borderpx + terminal.cursor.y * xelt_windowmain.charheight,
				1, xelt_windowmain.charheight - 1);
		XftDrawRect(xelt_windowmain.draw, &drawcol,
				borderpx + (curx + 1) * xelt_windowmain.charwidth - 1,
				borderpx + terminal.cursor.y * xelt_windowmain.charheight,
				1, xelt_windowmain.charheight - 1);
		XftDrawRect(xelt_windowmain.draw, &drawcol,
				borderpx + curx * xelt_windowmain.charwidth,
				borderpx + (terminal.cursor.y + 1) * xelt_windowmain.charheight - 1,
				xelt_windowmain.charwidth, 1);
	}
	oldx = curx, oldy = terminal.cursor.y;
}


void
xsettitle(char *p)
{
	XTextProperty prop;

	Xutf8TextListToTextProperty(xelt_windowmain.display, &p, 1, XUTF8StringStyle,
			&prop);
	XSetWMName(xelt_windowmain.display, xelt_windowmain.id, &prop);
	XSetTextProperty(xelt_windowmain.display, xelt_windowmain.id, &prop, xelt_windowmain.netwmname);
	XFree(prop.value);
}

void
window_title_set(void)
{
	xsettitle(window_title ? window_title : "noWinTitle");
}

void
redraw(void)
{
	tfulldirt();
	draw();
}

void
draw(void)
{
	drawregion(0, 0, terminal.col, terminal.row);
	XCopyArea(xelt_windowmain.display, xelt_windowmain.drawbuf, xelt_windowmain.id, dc.gc, 0, 0, xelt_windowmain.width,
			xelt_windowmain.height, 0, 0);
	XSetForeground(xelt_windowmain.display, dc.gc,
			dc.col[IS_SET(XELT_TERMINAL_REVERSE)?
				defaultfg : defaultbg].pixel);
}

void
drawregion(int x1, int y1, int x2, int y2)
{
	int i, x, y, ox, numspecs;
	xelt_Glyph base, new;
	XftGlyphFontSpec *specs;
	int ena_sel = sel.ob.x != -1 && sel.alt == IS_SET(XELT_TERMINAL_ALTSCREEN);

	if (!(xelt_windowmain.state & XELT_WIN_VISIBLE))
		return;

	for (y = y1; y < y2; y++) {
		if (!terminal.dirty[y])
			continue;

		terminal.dirty[y] = 0;

		specs = terminal.specbuf;
		numspecs = xmakeglyphfontspecs(specs, &terminal.line[y][x1], x2 - x1, x1, y);

		i = ox = 0;
		for (x = x1; x < x2 && i < numspecs; x++) {
			new = terminal.line[y][x];
			if (new.mode == XELT_ATTR_WDUMMY)
				continue;
			if (ena_sel && selected(x, y))
				new.mode ^= XELT_ATTR_REVERSE;
			if (i > 0 && ATTRCMP(base, new)) {
				xdrawglyphfontspecs(specs, base, i, ox, y);
				specs += i;
				numspecs -= i;
				i = 0;
			}
			if (i == 0) {
				ox = x;
				base = new;
			}
			i++;
		}
		if (i > 0)
			xdrawglyphfontspecs(specs, base, i, ox, y);
	}
	xdrawcursor();
}

void
expose(XEvent *ev)
{
	redraw();
}

void
visibility(XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;

	MODBIT(xelt_windowmain.state, e->state != VisibilityFullyObscured, XELT_WIN_VISIBLE);
}

void
unmap(XEvent *ev)
{
	xelt_windowmain.state &= ~XELT_WIN_VISIBLE;
}

void
xsetpointermotion(int set)
{
	MODBIT(xelt_windowmain.attrs.event_mask, set, PointerMotionMask);
	XChangeWindowAttributes(xelt_windowmain.display, xelt_windowmain.id, CWEventMask, &xelt_windowmain.attrs);
}

void
xseturgency(int add)
{
	XWMHints *h = XGetWMHints(xelt_windowmain.display, xelt_windowmain.id);

	MODBIT(h->flags, add, XUrgencyHint);
	XSetWMHints(xelt_windowmain.display, xelt_windowmain.id, h);
	XFree(h);
}

void
focus(XEvent *ev)
{
	XFocusChangeEvent *e = &ev->xfocus;

	if (e->mode == NotifyGrab)
		return;

	if (ev->type == FocusIn) {
		XSetICFocus(xelt_windowmain.inputcontext);
		xelt_windowmain.state |= XELT_WIN_FOCUSED;
		xseturgency(0);
		if (IS_SET(XELT_TERMINAL_FOCUS))
			ttywrite("\033[I", 3);
	} else {
		XUnsetICFocus(xelt_windowmain.inputcontext);
		xelt_windowmain.state &= ~XELT_WIN_FOCUSED;
		if (IS_SET(XELT_TERMINAL_FOCUS))
			ttywrite("\033[O", 3);
	}
}

int
match(xelt_uint mask, xelt_uint state)
{
	return mask == XELT_SIZE_XK_ANY_MOD || mask == (state & ~ignoremod);
}

void
numlock(const xelt_Arg *dummy)
{
	terminal.numlock ^= 1;
}

char*
kmap(KeySym k, xelt_uint state)
{
	xelt_Key *kp;
	int i;

	/* Check for mapped keys out of X11 function keys. */
	for (i = 0; i < LEN(mappedkeys); i++) {
		if (mappedkeys[i] == k)
			break;
	}
	if (i == LEN(mappedkeys)) {
		if ((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for (kp = key; kp < key + LEN(key); kp++) {
		if (kp->k != k)
			continue;

		if (!match(kp->mask, state))
			continue;

		if (IS_SET(XELT_TERMINAL_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
			continue;
		if (terminal.numlock && kp->appkey == 2)
			continue;

		if (IS_SET(XELT_TERMINAL_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
			continue;

		if (IS_SET(XELT_TERMINAL_CRLF) ? kp->crlf < 0 : kp->crlf > 0)
			continue;

		return kp->s;
	}

	return NULL;
}

void
kpress(XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	KeySym ksym;
	char buf[32], *customkey;
	int len;
	xelt_CharCode c;
	Status status;
	xelt_Shortcut *bp;

	if (IS_SET(XELT_TERMINAL_KBDLOCK))
		return;

	len = XmbLookupString(xelt_windowmain.inputcontext, e, buf, sizeof buf, &ksym, &status);
	/* 1. shortcuts */
	for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (ksym == bp->keysym && match(bp->mod, e->state)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* 2. custom keys from config.h */
	if ((customkey = kmap(ksym, e->state))) {
		ttysend(customkey, strlen(customkey));
		return;
	}

	/* 3. composed string from input method */
	if (len == 0)
		return;
	if (len == 1 && e->state & Mod1Mask) {
		if (IS_SET(XELT_TERMINAL_8BIT)) {
			if (*buf < 0177) {
				c = *buf | 0x80;
				len = utf8encode(c, buf);
			}
		} else {
			buf[1] = buf[0];
			buf[0] = '\033';
			len = 2;
		}
	}
	ttysend(buf, len);
}


void
cmessage(XEvent *e)
{
	/*
	 * See xembed specs
	 *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
	 */
	if (e->xclient.message_type == xelt_windowmain.xembed && e->xclient.format == 32) {
		if (e->xclient.data.l[1] == XELT_XEMBED_FOCUS_IN) {
			xelt_windowmain.state |= XELT_WIN_FOCUSED;
			xseturgency(0);
		} else if (e->xclient.data.l[1] == XELT_XEMBED_FOCUS_OUT) {
			xelt_windowmain.state &= ~XELT_WIN_FOCUSED;
		}
	} else if (e->xclient.data.l[0] == xelt_windowmain.wmdeletewin) {
		/* Send SIGHUP to shell */
		kill(pid, SIGHUP);
		exit(0);
	}
}

void
cresize(int width, int height)
{
	int col, row;

	if (width != 0)
		xelt_windowmain.width = width;
	if (height != 0)
		xelt_windowmain.height = height;

	col = (xelt_windowmain.width - 2 * borderpx) / xelt_windowmain.charwidth;
	row = (xelt_windowmain.height - 2 * borderpx) / xelt_windowmain.charheight;

	tresize(col, row);
	xresize(col, row);
}

void 
togglefullscreen(const xelt_Arg *arg)
{
    Atom type;
    int format, status;
    unsigned long nItem, bytesAfter;
    unsigned char *properties = NULL;

    Atom wmstateremove = XInternAtom(xelt_windowmain.display,"_NET_WM_STATE_REMOVE",False);


    status = XGetWindowProperty(xelt_windowmain.display, xelt_windowmain.id, xelt_windowmain.netwmstate, 0, (~0L), 
            False, AnyPropertyType, &type, &format, &nItem, &bytesAfter, &properties);
    if (status == Success && properties)
	{
		Atom prop = ((Atom *)properties)[0];
        XEvent e;
        memset( &e, 0, sizeof(e) );
        e.type = ClientMessage;
        e.xclient.window = xelt_windowmain.id;
        e.xclient.message_type = xelt_windowmain.netwmstate;
        e.xclient.format = 32;
        e.xclient.data.l[0] = (prop != xelt_windowmain.netwmfullscreen) ? 1: wmstateremove;
        e.xclient.data.l[1] = xelt_windowmain.netwmfullscreen;
        e.xclient.data.l[2] = 0;
        e.xclient.data.l[3] = 0;
        e.xclient.data.l[4] = 0;
        XSendEvent(xelt_windowmain.display, DefaultRootWindow(xelt_windowmain.display), 0,
                SubstructureNotifyMask|SubstructureRedirectMask, &e);
	}

}

void
resize(XEvent *e)
{
	if (e->xconfigure.width == xelt_windowmain.width && e->xconfigure.height == xelt_windowmain.height)
		return;

	cresize(e->xconfigure.width, e->xconfigure.height);
	ttyresize();
}

void
run(void)
{
	char *me="run";
	xelt_log(me,"started");
	selinit();
	XEvent ev;
	int w = xelt_windowmain.width, h = xelt_windowmain.height;
	fd_set rfd;  //add rfd file descriptor to monitor it.
	int xfd = XConnectionNumber(xelt_windowmain.display);
	int xev, dodraw = 0;
	long deltatime;

	/* Waiting for window mapping */
	do {
		XNextEvent(xelt_windowmain.display, &ev);
		/*
		 * This XFilterEvent call is required because of XOpenIM. It
		 * does filter out the key event and some client message for
		 * the input method too.
		 */
		if (XFilterEvent(&ev, None))
			continue;
		if (ev.type == ConfigureNotify) {
			w = ev.xconfigure.width;
			h = ev.xconfigure.height;
		}
	} while (ev.type != MapNotify);
	cresize(w, h);
	ttynew();
	ttyresize();
	clock_gettime(CLOCK_MONOTONIC, &last);
	
	for (xev = actionfps;;) {
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &rfd);
		FD_SET(xfd, &rfd);

		if (pselect(MAX(xfd, cmdfd)+1, &rfd, NULL, NULL, tv, NULL) < 0) {
			if (errno == EINTR)
				continue;
			printAndExit("select failed: %s\n", strerror(errno));
		}
		if (FD_ISSET(cmdfd, &rfd)) {
			ttyread();
		}

		if (FD_ISSET(xfd, &rfd))
			xev = actionfps;

		clock_gettime(CLOCK_MONOTONIC, &now);
		drawtimeout.tv_sec = 0;
		drawtimeout.tv_nsec =  (1000 * 1E6)/ xfps;
		tv = &drawtimeout;

		dodraw = 0;
		
		deltatime = TIMEDIFF(now, last);
		if (deltatime > 1000 / (xev ? xfps : actionfps)) {
			dodraw = 1;
			last = now;
		}

		if (dodraw) {
			while (XPending(xelt_windowmain.display)) {
				XNextEvent(xelt_windowmain.display, &ev);
				if (XFilterEvent(&ev, None))
					continue;
				if (handler[ev.type])
					(handler[ev.type])(&ev);
			}

			draw();
			XFlush(xelt_windowmain.display);//flushes the output buffer

			if (xev && !FD_ISSET(xfd, &rfd))
				xev--;
			if (!FD_ISSET(cmdfd, &rfd) && !FD_ISSET(xfd, &rfd)) {
				tv = NULL;
			}
		}
	}
}


int
main(int argc, char *argv[])
{
	xelt_uint cols = 80, rows = 24;

	xelt_windowmain.left = xelt_windowmain.top = 0;
	xelt_windowmain.isfixed = False;
	xelt_windowmain.cursorstyle = cursorshape;

	window_title = basename(xstrdup(argv[0]));
	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");//determine locale support and configure locale modifiers
	tnew(MAX(cols, 1), MAX(rows, 1)); // create new xelt_Terminal and store it in terminal global var
	xinit();
	run();

	return 0;
}

