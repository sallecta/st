void evhandler_btnpress(XEvent *e)
{
	char *me="evhandler_btnpress";

	if (e->xbutton.button == XELT_MOUSSE_LEFT) {
				xelt_log(me,"pressed XELT_MOUSSE_LEFT");
	}	else if (e->xbutton.button == XELT_MOUSSE_MIDDLE) {
		xelt_log(me,"pressed XELT_MOUSSE_MIDDLE");
	}	else if (e->xbutton.button == XELT_MOUSSE_RIGHT) {
		xelt_log(me,"pressed XELT_MOUSSE_RIGHT");
	}
	
	
	struct timespec now;
	xelt_MouseShortcut *ms;

	if (IS_SET(XELT_TERMINAL_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
		if (e->xbutton.button == ms->b
				&& match(ms->mask, e->xbutton.state)) {
			ttywrite1(ms->s, strlen(ms->s));
			return;
		}
	}

	if (e->xbutton.button == XELT_MOUSSE_LEFT) {
		clock_gettime(CLOCK_MONOTONIC, &now);

		/* Clear previous selection, logically and visually. */
		evhandler_selclear(NULL);
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

void evhandler_btnrelease(XEvent *e)
{
	char *me="evhandler_btnrelease";
	xelt_log(me,"started");
	if (IS_SET(XELT_TERMINAL_MOUSE) && !(e->xbutton.state & forceselmod)) {
		mousereport(e);
		return;
	}

	if (e->xbutton.button == XELT_MOUSSE_MIDDLE) {
		selpaste(NULL);
	} else if (e->xbutton.button == XELT_MOUSSE_LEFT) {
		if (sel.mode == XELT_SEL_READY) {
			getbuttoninfo(e);
			selcopy(e->xbutton.time);
		} else
			evhandler_selclear(NULL);
		sel.mode = XELT_SEL_IDLE;
		tsetdirt(sel.nb.y, sel.ne.y);
	}
}

void evhandler_clientmsg(XEvent *e)
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

void evhandler_configure(XEvent *e)
{
	if (e->xconfigure.width == xelt_windowmain.width && e->xconfigure.height == xelt_windowmain.height)
		return;

	cresize(e->xconfigure.width, e->xconfigure.height);
	ttyresize();
}


void evhandler_expose(XEvent *ev)
{
	redraw();
}

 
void evhandler_focus(XEvent *ev)
{
	XFocusChangeEvent *e = &ev->xfocus;

	if (e->mode == NotifyGrab)
		return;

	if (ev->type == FocusIn) {
		XSetICFocus(xelt_windowmain.inputcontext);
		xelt_windowmain.state |= XELT_WIN_FOCUSED;
		xseturgency(0);
		if (IS_SET(XELT_TERMINAL_FOCUS))
			ttywrite1("\033[I", 3);
	} else {
		XUnsetICFocus(xelt_windowmain.inputcontext);
		xelt_windowmain.state &= ~XELT_WIN_FOCUSED;
		if (IS_SET(XELT_TERMINAL_FOCUS))
			ttywrite1("\033[O", 3);
	}
}

void evhandler_keypress(XEvent *ev)
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
		ttywrite1(customkey, strlen(customkey));
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
	ttywrite1(buf, len);
}



void evhandler_motion(XEvent *e)
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


void evhandler_unmap(XEvent *ev)
{
	xelt_windowmain.state &= ~XELT_WIN_VISIBLE;
}

void evhandler_visibility(XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;

	MODBIT(xelt_windowmain.state, e->state != VisibilityFullyObscured, XELT_WIN_VISIBLE);
}

void
evhandler_selclear(XEvent *e)
{
	if (sel.ob.x == -1)
		return;
	sel.mode = XELT_SEL_IDLE;
	sel.ob.x = -1;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void evhandler_selnotify(XEvent *e)
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
			ttywrite1("\033[200~", 6);
		ttywrite1((char *)data, nitems * format / 8);
		if (IS_SET(XELT_TERMINAL_BRCKTPASTE) && rem == 0)
			ttywrite1("\033[201~", 6);
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
evhandler_propnotify(XEvent *e)
{
	XPropertyEvent *xpev;
	Atom clipboard = XInternAtom(xelt_windowmain.display, "CLIPBOARD", 0);

	xpev = &e->xproperty;
	if (xpev->state == PropertyNewValue &&
			(xpev->atom == XA_PRIMARY ||
			 xpev->atom == clipboard)) {
		evhandler_selnotify(e);
	}
}


void evhandler_selrequest(XEvent *e)
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
