

/* original macroses */
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define DEFAULT(a, b)		(a) = (a) ? (a) : (b)
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define DIVCEIL(n, d)		(((n) + ((d) - 1)) / (d))
#define ISCONTROLC0(c)		(BETWEEN(c, 0, 0x1f) || (c) == '\177')
#define ISCONTROLC1(c)		(BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c)		(ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u)		(utf8strchr(worddelimiters, u) != NULL)
#define LIMIT(x, a, b)		(x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define USE_ARGB (alpha != XELT_SIZE_OPAQUE && opt_embed == NULL)
#define ATTRCMP(a, b)		((a).mode != (b).mode || (a).fg != (b).fg || \
				(a).bg != (b).bg)
#define IS_SET(flag)		((terminal.mode & (flag)) != 0)
#define TIMEDIFF(t1, t2)	((t1.tv_sec-t2.tv_sec)*1000 + \
				(t1.tv_nsec-t2.tv_nsec)/1E6)
#define MODBIT(x, set, bit)	((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r,g,b)	(1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)		(1 << 24 & (x))
#define TRUERED(x)		(((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)		(((x) & 0xff00))
#define TRUEBLUE(x)		(((x) & 0xff) << 8)

/* logger */
/*
usage:
char *me="sender_name";
xelt_log(me,"started");
or
char charbuf256[256];
snprintf(charbuf256, sizeof charbuf256, "%s%s", "text1: ", "text2");
snprintf(charbuf256, 256 "%s%s", "text1: ", "text2");
xelt_log(me,charbuf256);
*/
#if   defined(xelt_log_messages)
	void xelt_log(char *argStr1,char *argStr2)
	{
		printf("%s: %s\n",argStr1,argStr2);
	}
#else
	void xelt_log(char *argStr1,char *argStr2)
	{}	
#endif