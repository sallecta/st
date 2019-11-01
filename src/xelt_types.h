#include <math.h>


typedef unsigned char xelt_uchar;
typedef unsigned int xelt_uint;
typedef unsigned long xelt_ulong;
typedef unsigned short xelt_ushort;

typedef uint_least32_t xelt_CharCode;

// X11
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
typedef XftDraw *xelt_Draw;
typedef struct {
	Display *display;
	Colormap colormap;
	Window id;
	Drawable drawbuf;
	Atom xembed, wmdeletewin, netwmname, netwmpid, netwmstate, netwmfullscreen;
	XIM inputmethod;
	XIC inputcontext;
	xelt_Draw draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int isfixed; /* is fixed geometry? */
	int left, top; /* left and top offset */
	int gmask; /* geometry mask */
	int ttywidth, ttyheight; /* tty width and height */
	int width, height; /* window width and height */
	int charheight; /* char height */
	int charwidth; /* char width  */
	int depth; /* bit depth */
	char state; /* focus, redraw, visible */
	int cursorstyle; 
} xelt_Window;

/* Font structure */
typedef struct {
	int height;
	int width;
	int ascent;
	int descent;
	short lbearing;
	short rbearing;
	XftFont *match;
	FcFontSet *set;
	FcPattern *pattern;
} xelt_Font;

typedef XftColor xelt_Color;

typedef struct {
	xelt_CharCode u;           /* character code */
	xelt_ushort mode;      /* attribute flags */
	uint32_t fg;      /* foreground  */
	uint32_t bg;      /* background  */
} xelt_Glyph;


typedef xelt_Glyph *xelt_Line;


typedef struct {
	xelt_Glyph attr; /* current char attributes */
	int x;
	int y;
	char state;
} xelt_TCursor;


/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct {
	char buf[XELT_SIZE_ESC_BUF]; /* raw string */
	int len;               /* raw string length */
	char priv;
	int arg[XELT_ESC_ARG_SIZ];
	int narg;              /* nb of args */
	char mode[2];
} xelt_CSIEscape;


/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
	char type;             /* ESC type ... */
	char buf[XELT_SIZE_STR_BUF]; /* raw string */
	int len;               /* raw string length */
	char *args[XELT_SIZE_STR_ARG];
	int narg;              /* nb of args */
} xelt_STREscape;


/* Internal representation of the screen */
typedef struct {
	int row;      /* nb row */
	int col;      /* nb col */
	xelt_Line *line;   /* screen */
	xelt_Line *alt;    /* alternate screen */
	int *dirty;  /* dirtyness of lines */
	XftGlyphFontSpec *specbuf; /* font spec buffer used for rendering */
	xelt_TCursor cursor;    /* cursor */
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
	int numlock; /* lock numbers in keyboard */
	int *tabs;
} xelt_Terminal;


/* Purely graphic info */
typedef struct {
	xelt_uint b;
	xelt_uint mask;
	char *s;
} xelt_MouseShortcut;


typedef struct {
	KeySym k;
	xelt_uint mask;
	char *s;
	/* three valued logic variables: 0 indifferent, 1 on, -1 off */
	signed char appkey;    /* application keypad */
	signed char appcursor; /* application cursor */
	signed char crlf;      /* crlf mode          */
} xelt_Key;


typedef struct {
	int mode;
	int type;
	int snap;
	/*
	 * xelt_Selection variables:
	 * nb – normalized coordinates of the beginning of the selection
	 * ne – normalized coordinates of the end of the selection
	 * ob – original coordinates of the beginning of the selection
	 * oe – original coordinates of the end of the selection
	 */
	struct {
		int x, y;
	} nb, ne, ob, oe;

	char *primary, *clipboard;
	Atom xtarget;
	int alt;
	struct timespec tclick1;
	struct timespec tclick2;
} xelt_Selection;

typedef union {
	int i;
	xelt_uint ui;
	float f;
	const void *v;
} xelt_Arg;

typedef struct {
	xelt_uint mod;
	KeySym keysym;
	void (*func)(const xelt_Arg *);
	const xelt_Arg arg;
} xelt_Shortcut;

typedef struct {
	XftFont *font;
	int flags;
	xelt_CharCode unicodep;
} xelt_Fontcache;

/* 
for reference only
typedef struct {
	time_t tv_sec ;
	long tv_nsec;
} timespec;
 */