//
// session.cc for Blackbox - an X11 Window manager
// Copyright (c) 1997, 1998 by Brad Hughes, bhughes@arn.net
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//
// (See the included file COPYING / GPL-2.0)
//

#include "session.hh"
#include "window.hh"
#include "blackbox.hh"
#include "workspace.hh"
#include "icon.hh"

#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <string.h>
#include <sys/time.h>


static int anotherWMRunning(Display *, XErrorEvent *) {
  fprintf(stderr,
	  "blackbox: a fatal error has occurred while querying the X server\n"
	  "          make sure there is not another window manager running\n");
  exit(3);
  return(-1);
}


// *************************************************************************
// Session startup code
// *************************************************************************
//
// allocations:
// Display *display
// XFontStruct *resource.font.{menu,icon,title}
// SessionMenu *rootmenu
// GC opGC
// WorkspaceManager *ws_manager
// char *resource.menuFile
// XColors *colors_8bpp  (for 8bpp displays) %% need to free at session end %%
//
// *************************************************************************

BlackboxSession::BlackboxSession(char *display_name) {
  b1Pressed = False;
  b2Pressed = False;
  b3Pressed = False;
  reconfigure = False;
  shutdown = False;
  startup = True;
  focus_window_number = -1;

  rootmenu = 0;
  resource.font.menu = resource.font.icon = resource.font.title = 0;

  if ((display = XOpenDisplay(display_name)) == NULL) {
    fprintf(stderr, "connection to X server failed\n");
    exit(2);
  }
  
  screen = DefaultScreen(display);
  root = RootWindow(display, screen);
  v = DefaultVisual(display, screen);
  depth = DefaultDepth(display, screen);

#ifdef SHAPE
  shape.extensions = XShapeQueryExtension(display, &shape.event_basep,
					  &shape.error_basep);
#else
  shape.extensions = False;
#endif

  window_search_list = new llist<WindowSearch>;
  menu_search_list = new llist<MenuSearch>;
  icon_search_list = new llist<IconSearch>;
  wsmanager_search_list = new llist<WSManagerSearch>;

  InitScreen();
}


BlackboxSession::~BlackboxSession() {
  XSelectInput(display, root, NoEventMask);

  delete ReconfigureDialog.dialog;
  XDestroyWindow(display, ReconfigureDialog.yes_button);
  XDestroyWindow(display, ReconfigureDialog.no_button);
  XDestroyWindow(display, ReconfigureDialog.text_window);
  XDestroyWindow(display, ReconfigureDialog.window);
  XFreeGC(display, ReconfigureDialog.dialogGC);

  delete [] resource.menuFile;
  delete rootmenu;
  delete ws_manager;

  delete window_search_list;
  delete icon_search_list;
  delete menu_search_list;
  delete wsmanager_search_list;

  if (resource.font.title) XFreeFont(display, resource.font.title);
  if (resource.font.menu) XFreeFont(display, resource.font.menu);
  if (resource.font.icon) XFreeFont(display, resource.font.icon);
  XFreeGC(display, opGC);
  
  XSync(display, False);
  XCloseDisplay(display);
}


void BlackboxSession::InitScreen(void) {
  _XA_WM_COLORMAP_WINDOWS = XInternAtom(display, "WM_COLORMAP_WINDOWS", False);
  _XA_WM_PROTOCOLS = XInternAtom(display, "WM_PROTOCOLS", False);
  _XA_WM_STATE = XInternAtom(display, "WM_STATE", False);
  _XA_WM_DELETE_WINDOW = XInternAtom(display, "WM_DELETE_WINDOW", False);
  _XA_WM_TAKE_FOCUS = XInternAtom(display, "WM_TAKE_FOCUS", False);

  event_mask = LeaveWindowMask | EnterWindowMask | PropertyChangeMask |
    SubstructureNotifyMask | PointerMotionMask | SubstructureRedirectMask |
    ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask;
  
  XSetErrorHandler((XErrorHandler) anotherWMRunning);
  XSelectInput(display, root, event_mask);
  XSync(display, False);
  XSetErrorHandler((XErrorHandler) NULL);

  cursor.session = XCreateFontCursor(display, XC_left_ptr);
  cursor.move = XCreateFontCursor(display, XC_fleur);
  XDefineCursor(display, root, cursor.session);

  xres = WidthOfScreen(ScreenOfDisplay(display, screen));
  yres = HeightOfScreen(ScreenOfDisplay(display, screen));

  InitColor();
  XrmInitialize();
  LoadDefaults();

  XGCValues gcv;
  gcv.foreground = getColor("grey");
  gcv.function = GXxor;
  gcv.line_width = 2;
  gcv.subwindow_mode = IncludeInferiors;
  gcv.font = resource.font.title->fid;
  opGC = XCreateGC(display, root, GCForeground|GCFunction|GCSubwindowMode|
		   GCFont, &gcv);

  InitMenu();
  ws_manager = new WorkspaceManager(this, resource.workspaces);
  ws_manager->stackWindows(0, 0);

  createAutoConfigDialog();
  ReconfigureDialog.dialog->withdrawWindow();
  ws_manager->workspace(0)->removeWindow(ReconfigureDialog.dialog);
  ReconfigureDialog.visible = False;

  unsigned int nchild;
  Window r, p, *children;
  XGrabServer(display);
  XQueryTree(display, root, &r, &p, &children, &nchild);

  for (int i = 0; i < (int) nchild; ++i) {
    if (children[i] == None) continue;

    XWindowAttributes attrib;
    if (XGetWindowAttributes(display, children[i], &attrib)) {
      if ((! attrib.override_redirect) && (attrib.map_state != IsUnmapped)) {
	XSync(display, False);
	BlackboxWindow *nWin = new BlackboxWindow(this, children[i]);
		
	Atom atom;
	int foo;
	unsigned long ulfoo, nitems;
	unsigned char *state;
	XGetWindowProperty(display, nWin->clientWindow(), _XA_WM_STATE,
			   0, 3, False, _XA_WM_STATE, &atom, &foo, &nitems,
			   &ulfoo, &state);

	if (state != NULL) {
	  switch (*((unsigned long *) state)) {
	  case WithdrawnState:
	    nWin->deiconifyWindow();
            nWin->setFocusFlag(False);
	    break;
	    
	  case IconicState:
	    nWin->iconifyWindow();
	    break;
	    
	  case NormalState:
	  default:
	    nWin->deiconifyWindow();
	    nWin->setFocusFlag(False);
	    break;
	  }
	} else {
	  nWin->deiconifyWindow();
	  nWin->setFocusFlag(False);
	} 
      }
    }
  }

  while (XPending(display)) {
    XEvent foo;
    XNextEvent(display, &foo);
  }

  XUngrabServer(display);
}


// *************************************************************************
// Event handling/dispatching methods
// *************************************************************************

void BlackboxSession::EventLoop(void) {
  shutdown = False;
  startup = False;

  int xfd = ConnectionNumber(display);

  for (; (! shutdown);) {
    if (XPending(display)) {
      if (! reconfigure) {
	XEvent e;
	XNextEvent(display, &e);
	ProcessEvent(&e);
      }
    } else {
      // put a wait on the network file descriptor for the X connection...
      // this saves blackbox from eating all available cpu
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(xfd, &rfds);
      
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 500;
      
      select(xfd + 1, &rfds, 0, 0, &tv);
      ws_manager->checkClock();
    }
  }
    
  Dissociate();
}


void BlackboxSession::ProcessEvent(XEvent *e) {
  switch (e->type) {
  case ButtonPress: {
    BlackboxWindow *bWin = NULL;
    BlackboxIcon *bIcon = NULL;
    BlackboxMenu *bMenu = NULL;
    WorkspaceManager *wsMan = NULL;

    switch (e->xbutton.button) {
    case 1: b1Pressed = True; break;
    case 2: b2Pressed = True; break;
    case 3: b3Pressed = True; break;
    }
    
    if (e->xbutton.window == ReconfigureDialog.yes_button) {
      ReconfigureDialog.dialog->withdrawWindow();
      ws_manager->workspace(ReconfigureDialog.dialog->workspace())->
	removeWindow(ReconfigureDialog.dialog);
      ReconfigureDialog.visible = False;
      Reconfigure();
    } else if (e->xbutton.window == ReconfigureDialog.no_button) {
      ReconfigureDialog.dialog->withdrawWindow();
      ws_manager->workspace(ReconfigureDialog.dialog->workspace())->
	removeWindow(ReconfigureDialog.dialog);
      ReconfigureDialog.visible = False;
    } else if ((bWin = searchWindow(e->xbutton.window)) != NULL) {
      bWin->buttonPressEvent(&e->xbutton);
    } else if ((bIcon = searchIcon(e->xbutton.window)) != NULL) {
      bIcon->buttonPressEvent(&e->xbutton);
    } else if ((bMenu = searchMenu(e->xbutton.window)) != NULL) {
      bMenu->buttonPressEvent(&e->xbutton);
    } else if ((wsMan = searchWSManager(e->xbutton.window)) != NULL) {
      wsMan->buttonPressEvent(&e->xbutton);
    } else if (e->xbutton.window == root && e->xbutton.button == 3) {
      rootmenu->moveMenu(e->xbutton.x_root - (rootmenu->Width() / 2),
			 e->xbutton.y_root -
			 (rootmenu->titleHeight() / 2));
      
      if (! rootmenu->menuVisible())
	rootmenu->showMenu();
    }
    
    break;
  }
  
  case ButtonRelease: {
    BlackboxWindow *bWin = NULL;
    BlackboxIcon *bIcon = NULL;
    BlackboxMenu *bMenu = NULL;
    WorkspaceManager *wsMan = NULL;

    switch (e->xbutton.button) {
    case 1: b1Pressed = False; break;
    case 2: b2Pressed = False; break;
    case 3: b3Pressed = False; break;
    }

    if ((bWin = searchWindow(e->xbutton.window)) != NULL)
      bWin->buttonReleaseEvent(&e->xbutton);
    else if ((bIcon = searchIcon(e->xbutton.window)) != NULL)
      bIcon->buttonReleaseEvent(&e->xbutton);
    else if ((bMenu = searchMenu(e->xbutton.window)) != NULL)
      bMenu->buttonReleaseEvent(&e->xbutton);
    else if ((wsMan = searchWSManager(e->xbutton.window)) != NULL)
      wsMan->buttonReleaseEvent(&e->xbutton);
    
    break;
  }
  
  case ConfigureRequest: {
    BlackboxWindow *cWin = searchWindow(e->xconfigurerequest.window);
    if (cWin != NULL)
      cWin->configureRequestEvent(&e->xconfigurerequest);
    else {
      /* configure a window we haven't mapped yet */
      XWindowChanges xwc;
      
      xwc.x = e->xconfigurerequest.x;
      xwc.y = e->xconfigurerequest.y;	
      xwc.width = e->xconfigurerequest.width;
      xwc.height = e->xconfigurerequest.height;
      xwc.border_width = 0;
      xwc.sibling = e->xconfigurerequest.above;
      xwc.stack_mode = e->xconfigurerequest.detail;
      
      XConfigureWindow(display, e->xconfigurerequest.window,
		       e->xconfigurerequest.value_mask, &xwc);
    }
    
    break;
  }
    
  case MapRequest: {
    BlackboxWindow *rWin = searchWindow(e->xmaprequest.window);
    if (rWin == NULL)
	rWin = new BlackboxWindow(this, e->xmaprequest.window);
    
    rWin->mapRequestEvent(&e->xmaprequest);
    
      break;
  }
  
  case MapNotify: {
    BlackboxWindow *mWin = searchWindow(e->xmap.window);
    if (mWin != NULL)
      mWin->mapNotifyEvent(&e->xmap);
    
    break;
  }
  
  case UnmapNotify: {
    BlackboxWindow *uWin = searchWindow(e->xunmap.window);
    if (uWin != NULL)
      uWin->unmapNotifyEvent(&e->xunmap);
    
    break;
  }
  
  case DestroyNotify: {
    BlackboxWindow *dWin = searchWindow(e->xdestroywindow.window);
    if (dWin != NULL)
      dWin->destroyNotifyEvent(&e->xdestroywindow);
    
    break;
  }
  
  case MotionNotify: {
    BlackboxWindow *mWin = NULL;
    BlackboxMenu *mMenu = NULL;
    if ((mWin = searchWindow(e->xmotion.window)) != NULL)
      mWin->motionNotifyEvent(&e->xmotion);
    else if ((mMenu = searchMenu(e->xmotion.window)) != NULL)
      mMenu->motionNotifyEvent(&e->xmotion);
    
    break;
  }
  
  case PropertyNotify: {
    if (e->xproperty.state != PropertyDelete) {
      if (e->xproperty.atom == XA_RESOURCE_MANAGER &&
	  e->xproperty.window == root) {
	if (! ReconfigureDialog.visible) {
	  ws_manager->currentWorkspace()->addWindow(ReconfigureDialog.dialog);
	  ReconfigureDialog.dialog->deiconifyWindow();
	  raiseWindow(ReconfigureDialog.dialog);
	  ReconfigureDialog.visible = True;
	} else {
	  ws_manager->changeWorkspaceID(ReconfigureDialog.dialog->workspace());
	  raiseWindow(ReconfigureDialog.dialog);
	}
      } else {
	BlackboxWindow *pWin = searchWindow(e->xproperty.window);
	if (pWin != NULL)
	  pWin->propertyNotifyEvent(e->xproperty.atom);
      }
    }

    break;
  }
  
  case EnterNotify: {
    BlackboxWindow *fWin = NULL;
    BlackboxIcon *fIcon = NULL;
    
    if ((fWin = searchWindow(e->xcrossing.window)) != NULL) {
      XGrabServer(display);
      
      XSync(display, False);
      XEvent foo;
      if (XCheckTypedWindowEvent(display, fWin->clientWindow(),
				 UnmapNotify, &foo)) {
	ProcessEvent(&foo);
      } else if ((! fWin->isFocused()) && fWin->isVisible()) {
	fWin->setInputFocus();
      }
      
      XUngrabServer(display);
    } else if ((fIcon = searchIcon(e->xcrossing.window)) != NULL)
      fIcon->enterNotifyEvent(&e->xcrossing);
    
    break;
  }
  
  case LeaveNotify: {
    BlackboxIcon *lIcon = NULL;
    if ((lIcon = searchIcon(e->xcrossing.window)) != NULL)
      lIcon->leaveNotifyEvent(&e->xcrossing);
    
    break;
  }
  
  case Expose: {
    BlackboxWindow *eWin = NULL;
    BlackboxIcon *eIcon = NULL;
    BlackboxMenu *eMenu = NULL;
    WorkspaceManager *wsMan = NULL;
    if ((eWin = searchWindow(e->xexpose.window)) != NULL)
      eWin->exposeEvent(&e->xexpose);
    else if ((eIcon = searchIcon(e->xexpose.window)) != NULL)
      eIcon->exposeEvent(&e->xexpose);
    else if ((eMenu = searchMenu(e->xexpose.window)) != NULL)
      eMenu->exposeEvent(&e->xexpose);
    else if ((wsMan = searchWSManager(e->xexpose.window)) != NULL)
      wsMan->exposeEvent(&e->xexpose);
    else if (e->xexpose.window == ReconfigureDialog.text_window)
      for (int i = 0; i < 6; i++)
	XDrawString(display, ReconfigureDialog.text_window,
		    ReconfigureDialog.dialogGC, 3,
		    (ReconfigureDialog.line_h - 3) * (i + 1),
		    ReconfigureDialog.DialogText[i],
		    strlen(ReconfigureDialog.DialogText[i]));
    else if (e->xexpose.window == ReconfigureDialog.yes_button)
      XDrawString(display, ReconfigureDialog.yes_button,
		  ReconfigureDialog.dialogGC, 3, ReconfigureDialog.line_h - 3,
		  "Yes", 3);
    else if (e->xexpose.window == ReconfigureDialog.no_button)
      XDrawString(display, ReconfigureDialog.no_button,
		  ReconfigureDialog.dialogGC, 3, ReconfigureDialog.line_h - 3,
		  "No", 2);
    
    break;
  } 
  
  case FocusIn: {
    BlackboxWindow *iWin = searchWindow(e->xfocus.window);
    if (iWin != NULL) {
      iWin->setFocusFlag(True);
      focus_window_number = iWin->windowNumber();
    }

    break;
  }
  
  case FocusOut: {
    BlackboxWindow *oWin = searchWindow(e->xfocus.window);
    if (oWin != NULL && e->xfocus.mode == NotifyNormal)
      oWin->setFocusFlag(False);

    if ((e->xfocus.mode == NotifyNormal) &&
        (e->xfocus.detail == NotifyAncestor)) {
      XSetInputFocus(display, PointerRoot, RevertToParent, CurrentTime);
      focus_window_number = -1;
    }

    break;
  }
  
  case KeyPress: {
    if (e->xkey.state & Mod1Mask) {
      if (XKeycodeToKeysym(display, e->xkey.keycode, 0) == XK_Tab) {
	if ((ws_manager->currentWorkspace()->count() > 1) &&
	    (focus_window_number >= 0)) {
	  BlackboxWindow *next, *current =
	    ws_manager->currentWorkspace()->window(focus_window_number);
	  
	  int next_window_number, level = 0;
	  do {
	    do {
	      next_window_number =
		((focus_window_number + (++level)) <
		 ws_manager->currentWorkspace()->count())
		? focus_window_number + level : 0;
	      next =
		ws_manager->currentWorkspace()->window(next_window_number);
	    } while (next->isIconic());
	  } while ((! next->setInputFocus()) &&
		   (next_window_number != focus_window_number));
	  
	  if (next_window_number != focus_window_number) {
	    current->setFocusFlag(False);
	    ws_manager->currentWorkspace()->raiseWindow(next);
	  }
	} else if (ws_manager->currentWorkspace()->count() >= 1) {
	  ws_manager->currentWorkspace()->window(0)->setInputFocus();
	}
      }
    } else if (e->xkey.state & ControlMask) {
      if (XKeycodeToKeysym(display, e->xkey.keycode, 0) == XK_Left){
	if (ws_manager->currentWorkspaceID() > 0)
	  ws_manager->
	    changeWorkspaceID(ws_manager->currentWorkspaceID() - 1);
	else
	  ws_manager->changeWorkspaceID(ws_manager->count() - 1);
      } else if (XKeycodeToKeysym(display, e->xkey.keycode, 0) == XK_Right){
	if (ws_manager->currentWorkspaceID() != ws_manager->count() - 1)
	  ws_manager->
	    changeWorkspaceID(ws_manager->currentWorkspaceID() + 1);
	else
	  ws_manager->changeWorkspaceID(0);
      }
    }

    break;
  }

  case KeyRelease: {
    break;
  }

  default:
#ifdef SHAPE
    if (e->type == shape.event_basep) {
      XShapeEvent *shape_event = (XShapeEvent *) e;

      BlackboxWindow *eWin = NULL;
      if (((eWin = searchWindow(e->xany.window)) != NULL) ||
	  (shape_event->kind != ShapeBounding))
	eWin->shapeEvent(shape_event);
    }
#endif
    break;
  }
}


// *************************************************************************
// Linked list lookup/save/remove methods 
// *************************************************************************

BlackboxWindow *BlackboxSession::searchWindow(Window window) {
  XEvent foo;

  if (XCheckTypedWindowEvent(display, window, DestroyNotify, &foo))
    ProcessEvent(&foo);
  else {
    BlackboxWindow *win = NULL;
    llist_iterator<WindowSearch> it(window_search_list);

    for (; it.current(); it++) {
      WindowSearch *tmp = it.current();
      if (tmp->window == window) {
	win = tmp->data;
	break;
      }
    }

    return win;
  }
  
  return NULL;
}


BlackboxMenu *BlackboxSession::searchMenu(Window window) {
  XEvent foo;

  if (XCheckTypedWindowEvent(display, window, DestroyNotify, &foo))
    ProcessEvent(&foo);
  else {
    BlackboxMenu *menu = NULL;
    llist_iterator<MenuSearch> it(menu_search_list);

    for (; it.current(); it++) {
      MenuSearch *tmp = it.current();
      if (tmp->window == window) {
	menu = tmp->data;
	break;
      }
    }

    return menu;
  }
  
  return NULL;
}


BlackboxIcon *BlackboxSession::searchIcon(Window window) {
  XEvent foo;

  if (XCheckTypedWindowEvent(display, window, DestroyNotify, &foo))
    ProcessEvent(&foo);
  else {
    BlackboxIcon *icon = NULL;
    llist_iterator<IconSearch> it(icon_search_list);

    for (; it.current(); it++) {
      IconSearch *tmp = it.current();
      if (tmp->window == window) {
	icon = tmp->data;
	break;
      }
    }

    return icon;
  }
  
  return NULL;
}


WorkspaceManager *BlackboxSession::searchWSManager(Window window) {
  XEvent foo;

  if (XCheckTypedWindowEvent(display, window, DestroyNotify, &foo))
    ProcessEvent(&foo);
  else {
    WorkspaceManager *wsm = NULL;
    llist_iterator<WSManagerSearch> it(wsmanager_search_list);

    for (; it.current(); it++) {
      WSManagerSearch *tmp = it.current();
      if (tmp->window == window) {
	wsm = tmp->data;
	break;
      }
    }

    return wsm;
  }
  
  return NULL;
}


void BlackboxSession::saveWindowSearch(Window window, BlackboxWindow *data) {
  WindowSearch *tmp = new WindowSearch;
  tmp->window = window;
  tmp->data = data;
  window_search_list->insert(tmp);
}


void BlackboxSession::saveMenuSearch(Window window, BlackboxMenu *data) {
  MenuSearch *tmp = new MenuSearch;
  tmp->window = window;
  tmp->data = data;
  menu_search_list->insert(tmp);
}


void BlackboxSession::saveIconSearch(Window window, BlackboxIcon *data) {
  IconSearch *tmp = new IconSearch;
  tmp->window = window;
  tmp->data = data;
  icon_search_list->insert(tmp);
}


void BlackboxSession::saveWSManagerSearch(Window window,
					  WorkspaceManager *data) {
  WSManagerSearch *tmp = new WSManagerSearch;
  tmp->window = window;
  tmp->data = data;
  wsmanager_search_list->insert(tmp);
}


void BlackboxSession::removeWindowSearch(Window window) {
  llist_iterator<WindowSearch> it(window_search_list);
  for (; it.current(); it++) {
    WindowSearch *tmp = it.current();
    if (tmp->window == window) {
      window_search_list->remove(tmp);
      delete tmp;
      break;
    }
  }
}


void BlackboxSession::removeMenuSearch(Window window) {
  llist_iterator<MenuSearch> it(menu_search_list);
  for (; it.current(); it++) {
    MenuSearch *tmp = it.current();
    if (tmp->window == window) {
      menu_search_list->remove(tmp);
      delete tmp;
      break;
    }
  }
}


void BlackboxSession::removeIconSearch(Window window) {
  llist_iterator<IconSearch> it(icon_search_list);
  for (; it.current(); it++) {
    IconSearch *tmp = it.current();
    if (tmp->window == window) {
      icon_search_list->remove(tmp);
      delete tmp;
      break;
    }
  }
}


void BlackboxSession::removeWSManagerSearch(Window window) {
  llist_iterator<WSManagerSearch> it(wsmanager_search_list);
  for (; it.current(); it++) {
    WSManagerSearch *tmp = it.current();
    if (tmp->window == window) {
      wsmanager_search_list->remove(tmp);
      delete tmp;
      break;
    }
  }
}


// *************************************************************************
// Exit, Shutdown and Restart methods
// *************************************************************************

void BlackboxSession::Dissociate(void) {
  ws_manager->DissociateAll();
}


void BlackboxSession::Restart(void) {
  Dissociate();
  XSetInputFocus(display, PointerRoot, RevertToParent, CurrentTime);
  blackbox->Restart();
}


void BlackboxSession::Exit(void) {
  XSetInputFocus(display, PointerRoot, RevertToParent, CurrentTime);
  shutdown = True;
}


// *************************************************************************
// Session utility and maintainence
// *************************************************************************

void BlackboxSession::addWindow(BlackboxWindow *w)
{ ws_manager->currentWorkspace()->addWindow(w); }


void BlackboxSession::removeWindow(BlackboxWindow *w)
{ ws_manager->workspace(w->workspace())->removeWindow(w); }


void BlackboxSession::reassociateWindow(BlackboxWindow *w) {
  if (w->workspace() != ws_manager->currentWorkspaceID()) {
    ws_manager->workspace(w->workspace())->removeWindow(w);
    ws_manager->currentWorkspace()->addWindow(w);
  }
}


void BlackboxSession::raiseWindow(BlackboxWindow *w) {
  if (w->workspace() == ws_manager->currentWorkspaceID())
    ws_manager->currentWorkspace()->raiseWindow(w);
}


void BlackboxSession::lowerWindow(BlackboxWindow *w) {
  if (w->workspace() == ws_manager->currentWorkspaceID())
    ws_manager->currentWorkspace()->lowerWindow(w);
}


// *************************************************************************
// Resource loading
// *************************************************************************

void BlackboxSession::LoadDefaults(void) {
  XGrabServer(display);
  
#define BLACKBOXAD XAPPLOADDIR##"/Blackbox.ad"
  XrmDatabase blackbox_database = NULL, resource_database = NULL;
  
  XTextProperty resource_manager_string;
  if (XGetTextProperty(display, root, &resource_manager_string,
		       XA_RESOURCE_MANAGER)) {
    resource_database =
      XrmGetStringDatabase((const char *) resource_manager_string.value);
  }
  
  if (resource_database != NULL) {
    XrmCombineDatabase(resource_database, &blackbox_database, True);
  } else {
    blackbox_database = XrmGetFileDatabase(BLACKBOXAD);
  }

  XrmValue value;
  char *value_type;

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.toolboxTexture",
		     "Blackbox.Session.ToolboxTexture", &value_type, &value)) {
    if ((! strcasecmp(value.addr, "solid")) ||
	(! strcasecmp(value.addr, "solidraised")))
      resource.texture.toolbox = B_TextureRSolid;
    else if (! strcasecmp(value.addr, "solidsunken"))
      resource.texture.toolbox = B_TextureSSolid;
    else if (! strcasecmp(value.addr, "solidflat"))
      resource.texture.toolbox = B_TextureFSolid;
    else if ((! strcasecmp(value.addr, "dgradient")) ||
	     (! strcasecmp(value.addr, "dgradientraised")))
      resource.texture.toolbox = B_TextureRDGradient;
    else if (! strcasecmp(value.addr, "dgradientsunken"))
      resource.texture.toolbox = B_TextureSDGradient;
    else if (! strcasecmp(value.addr, "dgradientflat"))
      resource.texture.toolbox = B_TextureFDGradient;
    else if ((! strcasecmp(value.addr, "hgradient")) ||
	     (! strcasecmp(value.addr, "hgradientraised")))
      resource.texture.toolbox = B_TextureRHGradient;
    else if (! strcasecmp(value.addr, "hgradientsunken"))
      resource.texture.toolbox = B_TextureSHGradient;
    else if (! strcasecmp(value.addr, "hgradientflat"))
      resource.texture.toolbox = B_TextureFHGradient;
    else if ((! strcasecmp(value.addr, "vgradient")) ||
	     (! strcasecmp(value.addr, "vgradientraised")))
      resource.texture.toolbox = B_TextureRVGradient;
    else if (! strcasecmp(value.addr, "vgradientsunken"))
      resource.texture.toolbox = B_TextureSVGradient;
    else if (! strcasecmp(value.addr, "vgradientflat"))
      resource.texture.toolbox = B_TextureFVGradient;
    else
      resource.texture.toolbox = B_TextureRSolid;
  } else
    resource.texture.toolbox = B_TextureRSolid;

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.windowTexture",
		     "Blackbox.Session.WindowTexture", &value_type, &value)) {
    if ((! strcasecmp(value.addr, "solid")) ||
	(! strcasecmp(value.addr, "solidraised")))
      resource.texture.window = B_TextureRSolid;
    else if (! strcasecmp(value.addr, "solidsunken"))
      resource.texture.window = B_TextureSSolid;
    else if (! strcasecmp(value.addr, "solidflat"))
      resource.texture.window = B_TextureFSolid;
    else if ((! strcasecmp(value.addr, "dgradient")) ||
	     (! strcasecmp(value.addr, "dgradientraised")))
      resource.texture.window = B_TextureRDGradient;
    else if (! strcasecmp(value.addr, "dgradientsunken"))
      resource.texture.window = B_TextureSDGradient;
    else if (! strcasecmp(value.addr, "dgradientflat"))
      resource.texture.window = B_TextureFDGradient;
    else if ((! strcasecmp(value.addr, "hgradient")) ||
	     (! strcasecmp(value.addr, "hgradientraised")))
      resource.texture.window = B_TextureRHGradient;
    else if (! strcasecmp(value.addr, "hgradientsunken"))
      resource.texture.window = B_TextureSHGradient;
    else if (! strcasecmp(value.addr, "hgradientflat"))
      resource.texture.window = B_TextureFHGradient;
    else if ((! strcasecmp(value.addr, "vgradient")) ||
	     (! strcasecmp(value.addr, "vgradientraised")))
      resource.texture.window = B_TextureRVGradient;
    else if (! strcasecmp(value.addr, "vgradientsunken"))
      resource.texture.window = B_TextureSVGradient;
    else if (! strcasecmp(value.addr, "vgradientflat"))
      resource.texture.window = B_TextureFVGradient;
    else
      resource.texture.window = B_TextureRSolid;
  } else
    resource.texture.window = B_TextureRSolid;

  if (XrmGetResource(blackbox_database,
		     "blackbox.window.buttonTexture",
		     "Blackbox.Window.ButtonTexture", &value_type, &value)) {
    if ((! strcasecmp(value.addr, "solid")) ||
	(! strcasecmp(value.addr, "solidraised")))
      resource.texture.button = B_TextureRSolid;
    else if (! strcasecmp(value.addr, "solidsunken"))
      resource.texture.button = B_TextureSSolid;
    else if (! strcasecmp(value.addr, "solidflat"))
      resource.texture.button = B_TextureFSolid;
    else if ((! strcasecmp(value.addr, "dgradient")) ||
	     (! strcasecmp(value.addr, "dgradientraised")))
      resource.texture.button = B_TextureRDGradient;
    else if (! strcasecmp(value.addr, "dgradientsunken"))
      resource.texture.button = B_TextureSDGradient;
    else if (! strcasecmp(value.addr, "dgradientflat"))
      resource.texture.button = B_TextureFDGradient;
    else if ((! strcasecmp(value.addr, "hgradient")) ||
	     (! strcasecmp(value.addr, "hgradientraised")))
      resource.texture.button = B_TextureRHGradient;
    else if (! strcasecmp(value.addr, "hgradientsunken"))
      resource.texture.button = B_TextureSHGradient;
    else if (! strcasecmp(value.addr, "hgradientflat"))
      resource.texture.button = B_TextureFHGradient;
    else if ((! strcasecmp(value.addr, "vgradient")) ||
	     (! strcasecmp(value.addr, "vgradientraised")))
      resource.texture.button = B_TextureRVGradient;
    else if (! strcasecmp(value.addr, "vgradientsunken"))
      resource.texture.button = B_TextureSVGradient;
    else if (! strcasecmp(value.addr, "vgradientflat"))
      resource.texture.button = B_TextureFVGradient;
    else
      resource.texture.button = B_TextureRSolid;
  } else
    resource.texture.button = B_TextureRSolid;

  if (XrmGetResource(blackbox_database,
		     "blackbox.menu.menuTexture",
		     "Blackbox.Menu.MenuTexture", &value_type, &value)) {
    if ((! strcasecmp(value.addr, "solid")) ||
	(! strcasecmp(value.addr, "solidraised")))
      resource.texture.menu = B_TextureRSolid;
    else if (! strcasecmp(value.addr, "solidsunken"))
      resource.texture.menu = B_TextureSSolid;
    else if (! strcasecmp(value.addr, "solidflat"))
      resource.texture.menu = B_TextureFSolid;
    else if ((! strcasecmp(value.addr, "dgradient")) ||
	     (! strcasecmp(value.addr, "dgradientraised")))
      resource.texture.menu = B_TextureRDGradient;
    else if (! strcasecmp(value.addr, "dgradientsunken"))
      resource.texture.menu = B_TextureSDGradient;
    else if (! strcasecmp(value.addr, "dgradientflat"))
      resource.texture.menu = B_TextureFDGradient;
    else if ((! strcasecmp(value.addr, "hgradient")) ||
	     (! strcasecmp(value.addr, "hgradientraised")))
      resource.texture.menu = B_TextureRHGradient;
    else if (! strcasecmp(value.addr, "hgradientsunken"))
      resource.texture.menu = B_TextureSHGradient;
    else if (! strcasecmp(value.addr, "hgradientflat"))
      resource.texture.menu = B_TextureFHGradient;
    else if ((! strcasecmp(value.addr, "vgradient")) ||
	     (! strcasecmp(value.addr, "vgradientraised")))
      resource.texture.menu = B_TextureRVGradient;
    else if (! strcasecmp(value.addr, "vgradientsunken"))
      resource.texture.menu = B_TextureSVGradient;
    else if (! strcasecmp(value.addr, "vgradientflat"))
      resource.texture.menu = B_TextureFVGradient;
    else
      resource.texture.menu = B_TextureRSolid;
  } else
    resource.texture.menu = B_TextureRSolid;

  if (XrmGetResource(blackbox_database,
		     "blackbox.menu.menuItemPressedTexture",
		     "Blackbox.Menu.MenuItemPressedTexture", &value_type,
		     &value)) {
    if ((! strcasecmp(value.addr, "solid")) ||
	(! strcasecmp(value.addr, "solidraised")))
      resource.texture.pimenu = B_TextureRSolid;
    else if (! strcasecmp(value.addr, "solidsunken"))
      resource.texture.pimenu = B_TextureSSolid;
    else if (! strcasecmp(value.addr, "solidflat"))
      resource.texture.pimenu = B_TextureFSolid;
    else if ((! strcasecmp(value.addr, "dgradient")) ||
	     (! strcasecmp(value.addr, "dgradientraised")))
      resource.texture.pimenu = B_TextureRDGradient;
    else if (! strcasecmp(value.addr, "dgradientsunken"))
      resource.texture.pimenu = B_TextureSDGradient;
    else if (! strcasecmp(value.addr, "dgradientflat"))
      resource.texture.pimenu = B_TextureFDGradient;
    else if ((! strcasecmp(value.addr, "hgradient")) ||
	     (! strcasecmp(value.addr, "hgradientraised")))
      resource.texture.pimenu = B_TextureRHGradient;
    else if (! strcasecmp(value.addr, "hgradientsunken"))
      resource.texture.pimenu = B_TextureSHGradient;
    else if (! strcasecmp(value.addr, "hgradientflat"))
      resource.texture.pimenu = B_TextureFHGradient;
    else if ((! strcasecmp(value.addr, "vgradient")) ||
	     (! strcasecmp(value.addr, "vgradientraised")))
      resource.texture.pimenu = B_TextureRVGradient;
    else if (! strcasecmp(value.addr, "vgradientsunken"))
      resource.texture.pimenu = B_TextureSVGradient;
    else if (! strcasecmp(value.addr, "vgradientflat"))
      resource.texture.pimenu = B_TextureFVGradient;
    else
      resource.texture.pimenu = B_TextureFSolid;
  } else
    resource.texture.pimenu = B_TextureFSolid;

  if (XrmGetResource(blackbox_database,
		     "blackbox.menu.menuItemTexture",
		     "Blackbox.Menu.MenuItemTexture", &value_type, &value)) {
    if ((! strcasecmp(value.addr, "solid")) ||
	(! strcasecmp(value.addr, "solidraised")))
      resource.texture.imenu = B_TextureRSolid;
    else if (! strcasecmp(value.addr, "solidsunken"))
      resource.texture.imenu = B_TextureSSolid;
    else if (! strcasecmp(value.addr, "solidflat"))
      resource.texture.imenu = B_TextureFSolid;
    else if ((! strcasecmp(value.addr, "dgradient")) ||
	     (! strcasecmp(value.addr, "dgradientraised")))
      resource.texture.imenu = B_TextureRDGradient;
    else if (! strcasecmp(value.addr, "dgradientsunken"))
      resource.texture.imenu = B_TextureSDGradient;
    else if (! strcasecmp(value.addr, "dgradientflat"))
      resource.texture.imenu = B_TextureFDGradient;
    else if ((! strcasecmp(value.addr, "hgradient")) ||
	     (! strcasecmp(value.addr, "hgradientraised")))
      resource.texture.imenu = B_TextureRHGradient;
    else if (! strcasecmp(value.addr, "hgradientsunken"))
      resource.texture.imenu = B_TextureSHGradient;
    else if (! strcasecmp(value.addr, "hgradientflat"))
      resource.texture.imenu = B_TextureFHGradient;
    else if ((! strcasecmp(value.addr, "vgradient")) ||
	     (! strcasecmp(value.addr, "vgradientraised")))
      resource.texture.imenu = B_TextureRVGradient;
    else if (! strcasecmp(value.addr, "vgradientsunken"))
      resource.texture.imenu = B_TextureSVGradient;
    else if (! strcasecmp(value.addr, "vgradientflat"))
      resource.texture.imenu = B_TextureFVGradient;
    else
      resource.texture.imenu = B_TextureRSolid;
  } else
    resource.texture.imenu = B_TextureRSolid;

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.frameColor",
		     "Blackbox.Session.FrameColor", &value_type, &value))
    resource.color.frame.pixel =
      getColor(value.addr, &resource.color.frame.r, &resource.color.frame.g,
	       &resource.color.frame.b);
  else
    resource.color.frame.pixel =
      getColor("black", &resource.color.frame.r, &resource.color.frame.g,
	       &resource.color.frame.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.toolboxColor",
		     "Blackbox.Session.ToolboxColor", &value_type, &value))
    resource.color.toolbox.pixel =
      getColor(value.addr, &resource.color.toolbox.r,
	       &resource.color.toolbox.g, &resource.color.toolbox.b);
  else
    resource.color.toolbox.pixel =
      getColor("grey", &resource.color.toolbox.r, &resource.color.toolbox.g,
	       &resource.color.toolbox.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.session.toolboxToColor",
		     "Blackbox.Session.ToolboxToColor", &value_type, &value))
    resource.color.toolbox_to.pixel =
      getColor(value.addr, &resource.color.toolbox_to.r,
	       &resource.color.toolbox_to.g, &resource.color.toolbox_to.b);
  else
    resource.color.toolbox_to.pixel =
      getColor("black", &resource.color.toolbox_to.r,
	       &resource.color.toolbox_to.g, &resource.color.toolbox_to.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.window.focusColor",
		     "Blackbox.Window.FocusColor", &value_type, &value))
    resource.color.focus.pixel =
      getColor(value.addr, &resource.color.focus.r, &resource.color.focus.g,
	       &resource.color.focus.b);
  else
    resource.color.focus.pixel =
      getColor("darkgrey", &resource.color.focus.r, &resource.color.focus.g,
	       &resource.color.focus.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.window.focusToColor",
		     "Blackbox.Window.FocusToColor", &value_type, &value))
    resource.color.focus_to.pixel =
      getColor(value.addr, &resource.color.focus_to.r,
	       &resource.color.focus_to.g, &resource.color.focus_to.b);
  else
    resource.color.focus_to.pixel =
      getColor("black", &resource.color.focus_to.r,
	       &resource.color.focus_to.g, &resource.color.focus_to.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.window.unfocusColor",
		     "Blackbox.Window.UnfocusColor", &value_type, &value))
    resource.color.unfocus.pixel =
      getColor(value.addr, &resource.color.unfocus.r,
	       &resource.color.unfocus.g, &resource.color.unfocus.b);
  else
    resource.color.unfocus.pixel =
      getColor("black", &resource.color.unfocus.r,
	       &resource.color.unfocus.g, &resource.color.unfocus.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.window.unfocusToColor",
		     "Blackbox.Window.UnfocusToColor", &value_type, &value))
    resource.color.unfocus_to.pixel =
      getColor(value.addr, &resource.color.unfocus_to.r,
	       &resource.color.unfocus_to.g, &resource.color.unfocus_to.b);
  else
    resource.color.unfocus_to.pixel =
      getColor("black", &resource.color.unfocus_to.r,
	       &resource.color.unfocus_to.g, &resource.color.unfocus_to.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.window.buttonColor",
		     "Blackbox.Window.ButtonColor", &value_type, &value))
    resource.color.button.pixel =
      getColor(value.addr, &resource.color.button.r,
	       &resource.color.button.g, &resource.color.button.b);
  else
    resource.color.button.pixel =
      getColor("grey" , &resource.color.button.r,
	       &resource.color.button.g, &resource.color.button.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.window.buttonToColor",
		     "Blackbox.Window.ButtonToColor", &value_type, &value))
    resource.color.button_to.pixel =
      getColor(value.addr, &resource.color.button_to.r,
	       &resource.color.button_to.g, &resource.color.button_to.b);
  else
    resource.color.button_to.pixel =
      getColor("black", &resource.color.button_to.r,
	       &resource.color.button_to.g, &resource.color.button_to.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.menu.menuColor",
		     "Blackbox.Menu.MenuColor", &value_type, &value))
    resource.color.menu.pixel =
      getColor(value.addr, &resource.color.menu.r,
	       &resource.color.menu.g, &resource.color.menu.b);
  else
    resource.color.menu.pixel =
      getColor("darkgrey", &resource.color.menu.r,
	       &resource.color.menu.g, &resource.color.menu.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.menu.menuToColor",
		     "Blackbox.Menu.MenuToColor", &value_type, &value))
    resource.color.menu_to.pixel =
      getColor(value.addr, &resource.color.menu_to.r,
	       &resource.color.menu_to.g, &resource.color.menu_to.b);
  else
    resource.color.menu_to.pixel =
      getColor("black", &resource.color.menu_to.r,
	       &resource.color.menu_to.g, &resource.color.menu_to.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.menu.menuItemColor",
		     "Blackbox.Menu.MenuItemColor", &value_type, &value))
    resource.color.imenu.pixel =
      getColor(value.addr, &resource.color.imenu.r,
	       &resource.color.imenu.g, &resource.color.imenu.b);
  else
    resource.color.imenu.pixel =
      getColor("black", &resource.color.imenu.r,
	       &resource.color.imenu.g, &resource.color.imenu.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.menu.menuItemToColor",
		     "Blackbox.Menu.MenuItemToColor", &value_type, &value))
    resource.color.imenu_to.pixel =
      getColor(value.addr, &resource.color.imenu_to.r,
	       &resource.color.imenu_to.g, &resource.color.imenu_to.b);
  else
    resource.color.imenu_to.pixel =
      getColor("grey", &resource.color.imenu_to.r,
	       &resource.color.imenu_to.g, &resource.color.imenu_to.b);
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.session.focusTextColor",
		     "Blackbox.Session.FocusTextColor", &value_type, &value))
    resource.color.ftext.pixel =
      getColor(value.addr, &resource.color.ftext.r, &resource.color.ftext.g,
	       &resource.color.ftext.b);
  else
    resource.color.ftext.pixel =
      getColor("white", &resource.color.ftext.r, &resource.color.ftext.g,
	       &resource.color.ftext.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.unfocusTextColor",
		     "Blackbox.Session.UnfocusTextColor", &value_type, &value))
    resource.color.utext.pixel =
      getColor(value.addr, &resource.color.utext.r, &resource.color.utext.g,
	       &resource.color.utext.b);
  else
    resource.color.utext.pixel =
      getColor("darkgrey", &resource.color.utext.r, &resource.color.utext.g,
	       &resource.color.utext.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.menuTextColor",
		     "Blackbox.Session.MenuTextColor", &value_type, &value))
    resource.color.mtext.pixel =
      getColor(value.addr, &resource.color.mtext.r, &resource.color.mtext.g,
	       &resource.color.mtext.b);
  else
    resource.color.mtext.pixel =
      getColor("white", &resource.color.mtext.r, &resource.color.mtext.g,
	       &resource.color.mtext.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.menuItemTextColor",
		     "Blackbox.Session.MenuItemTextColor", &value_type,
		     &value))
    resource.color.mitext.pixel =
      getColor(value.addr, &resource.color.mitext.r, &resource.color.mitext.g,
	       &resource.color.mitext.b);
  else
    resource.color.mitext.pixel =
      getColor("grey", &resource.color.mitext.r, &resource.color.mitext.g,
	       &resource.color.mitext.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.menuPressedTextColor",
		     "Blackbox.Session.MenuPressedTextColor", &value_type,
		     &value))
    resource.color.ptext.pixel =
      getColor(value.addr, &resource.color.ptext.r, &resource.color.ptext.g,
	       &resource.color.ptext.b);
  else
    resource.color.ptext.pixel =
      getColor("darkgrey", &resource.color.ptext.r, &resource.color.ptext.g,
	       &resource.color.ptext.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.iconTextColor",
		     "Blackbox.Session.IconTextColor", &value_type,
		     &value))
    resource.color.itext.pixel =
      getColor(value.addr, &resource.color.itext.r, &resource.color.itext.g,
	       &resource.color.itext.b);
  else
    resource.color.itext.pixel =
      getColor("black", &resource.color.itext.r, &resource.color.itext.g,
	       &resource.color.itext.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.toolboxTextColor",
		     "Blackbox.Session.ToolboxTextColor", &value_type,
		     &value))
    resource.color.ttext.pixel =
      getColor(value.addr, &resource.color.ttext.r, &resource.color.ttext.g,
	       &resource.color.ttext.b);
  else
    resource.color.ttext.pixel =
      getColor("black", &resource.color.ttext.r, &resource.color.ttext.g,
	       &resource.color.ttext.b);

  if (XrmGetResource(blackbox_database,
		     "blackbox.session.menuFile",
		     "Blackbox.Session.MenuFile", &value_type, &value)) {
    int len = strlen(value.addr);
    resource.menuFile = new char[len + 1];
    memset(resource.menuFile, 0, len + 1);
    strncpy(resource.menuFile, value.addr, len);
  } else {
#define BLACKBOXMENUAD XLIBDIR##"/Blackbox/Menu.ad"
    int len = strlen(BLACKBOXMENUAD);
    resource.menuFile = new char[len + 1];
    memset(resource.menuFile, 0, len + 1);
    strncpy(resource.menuFile, BLACKBOXMENUAD, len);
  }
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.session.workspaces",
		     "Blackbox.Session.Workspaces", &value_type, &value)) {
    if (sscanf(value.addr, "%d", &resource.workspaces) != 1) {
      resource.workspaces = 1;
    }
  } else
    resource.workspaces = 1;
  
  if (XrmGetResource(blackbox_database,
		     "blackbox.session.orientation",
		     "Blackbox.Session.Orientation", &value_type, &value)) {
    if (! strcasecmp(value.addr, "lefthanded"))
      resource.orientation = B_LeftHandedUser;
    else if (! strcasecmp(value.addr, "righthanded"))
      resource.orientation = B_RightHandedUser;
  } else
    resource.orientation = B_RightHandedUser;

  const char *defaultFont = "-*-charter-medium-r-*-*-*-120-*-*-*-*-*-*";
  if (resource.font.title) XFreeFont(display, resource.font.title);
  if (XrmGetResource(blackbox_database,
		     "blackbox.session.titleFont",
		     "Blackbox.Session.TitleFont", &value_type, &value)) {
    if ((resource.font.title = XLoadQueryFont(display, value.addr)) == NULL) {
      fprintf(stderr,
	      " blackbox: couldn't load font '%s'\n"
	      "  ...  reverting to default font.", value.addr);
      if ((resource.font.title = XLoadQueryFont(display, defaultFont))
	  == NULL) {
	fprintf(stderr,
		"blackbox: couldn't load default font.  please check to\n"
		"make sure the necessary font is installed '%s'\n",
		defaultFont);
	exit(2);
      }  
    }
  } else {
    if ((resource.font.title = XLoadQueryFont(display, defaultFont)) == NULL) {
      fprintf(stderr,
	      "blackbox: couldn't load default font.  please check to\n"
	      "make sure the necessary font is installed '%s'\n", defaultFont);
      exit(2);
    }
  }

  if (resource.font.menu) XFreeFont(display, resource.font.menu);
  if (XrmGetResource(blackbox_database,
		     "blackbox.session.menuFont",
		     "Blackbox.Session.MenuFont", &value_type, &value)) {
    if ((resource.font.menu = XLoadQueryFont(display, value.addr)) == NULL) {
      fprintf(stderr,
	      " blackbox: couldn't load font '%s'\n"
	      "  ...  reverting to default font.", value.addr);
      if ((resource.font.menu = XLoadQueryFont(display, defaultFont))
	  == NULL) {
	fprintf(stderr,
		"blackbox: couldn't load default font.  please check to\n"
		"make sure the necessary font is installed '%s'\n",
		defaultFont);
	exit(2);
      }  
    }
  } else {
    if ((resource.font.menu = XLoadQueryFont(display, defaultFont)) == NULL) {
      fprintf(stderr,
	      "blackbox: couldn't load default font.  please check to\n"
	      "make sure the necessary font is installed '%s'\n", defaultFont);
      exit(2);
    }
  }

  if (resource.font.icon) XFreeFont(display, resource.font.icon);
  if (XrmGetResource(blackbox_database,
		     "blackbox.session.iconFont",
		     "Blackbox.Session.IconFont", &value_type, &value)) {
    if ((resource.font.icon = XLoadQueryFont(display, value.addr)) == NULL) {
      fprintf(stderr,
	      " blackbox: couldn't load font '%s'\n"
	      "  ...  reverting to default font.", value.addr);
      if ((resource.font.icon = XLoadQueryFont(display, defaultFont))
	  == NULL) {
	fprintf(stderr,
		"blackbox: couldn't load default font.  please check to\n"
		"make sure the necessary font is installed '%s'\n",
		defaultFont);
	exit(2);
      }  
    }
  } else {
    if ((resource.font.icon = XLoadQueryFont(display, defaultFont)) == NULL) {
      fprintf(stderr,
	      "blackbox: couldn't load default font.  please check to\n"
	      "make sure the necessary font is installed '%s'\n", defaultFont);
      exit(2);
    }
  }

  XrmDestroyDatabase(blackbox_database);
  XUngrabServer(display);
}


void BlackboxSession::updateWorkspace(int w) {
  ws_manager->workspace(w)->updateMenu();
}


// *************************************************************************
// Color lookup and allocation methods
// *************************************************************************

void BlackboxSession::InitColor(void) {
  if (depth == 8) {
    colors_8bpp = new XColor[125];
    int i = 0;
    for (int r = 0; r < 5; r++)
      for (int g = 0; g < 5; g++)
	for (int b = 0; b < 5; b++) {
	  colors_8bpp[i].red = (r * 0xffff) / 4;
	  colors_8bpp[i].green = (g * 0xffff) / 4;
	  colors_8bpp[i].blue = (b * 0xffff) / 4;
	  colors_8bpp[i++].flags = DoRed|DoGreen|DoBlue;
	}
    
    XGrabServer(display);
    
    Colormap colormap = DefaultColormap(display, DefaultScreen(display));
    for (i = 0; i < 125; i++)
      if (! XAllocColor(display, colormap, &colors_8bpp[i])) {
	fprintf(stderr, "couldn't alloc color %i %i %i\n", colors_8bpp[i].red,
		colors_8bpp[i].green, colors_8bpp[i].blue);		
	colors_8bpp[i].flags = 0;
      } else
	colors_8bpp[i].flags = DoRed|DoGreen|DoBlue;

    XUngrabServer(display);
  } else
    colors_8bpp = 0;
}


unsigned long BlackboxSession::getColor(const char *colorname,
					unsigned char *r, unsigned char *g,
					unsigned char *b)
{
  XColor color;
  XWindowAttributes attributes;
  
  XGetWindowAttributes(display, root, &attributes);
  color.pixel = 0;
  if (!XParseColor(display, attributes.colormap, colorname, &color)) {
    fprintf(stderr, "blackbox: color parse error: \"%s\"\n", colorname);
  } else if (!XAllocColor(display, attributes.colormap, &color)) {
    fprintf(stderr, "blackbox: color alloc error: \"%s\"\n", colorname);
  }
  
  if (color.red == 65535) *r = 0xff;
  else *r = (unsigned char) (color.red / 0xff);
  if (color.green == 65535) *g = 0xff;
  else *g = (unsigned char) (color.green / 0xff);
  if (color.blue == 65535) *b = 0xff;
  else *b = (unsigned char) (color.blue / 0xff);
 
  return color.pixel;
}


unsigned long BlackboxSession::getColor(const char *colorname) {
  XColor color;
  XWindowAttributes attributes;
  
  XGetWindowAttributes(display, root, &attributes);
  color.pixel = 0;
  if (!XParseColor(display, attributes.colormap, colorname, &color)) {
    fprintf(stderr, "blackbox: color parse error: \"%s\"\n", colorname);
  } else if (!XAllocColor(display, attributes.colormap, &color)) {
    fprintf(stderr, "blackbox: color alloc error: \"%s\"\n", colorname);
  }

  return color.pixel;
}


// *************************************************************************
// Menu loading
// *************************************************************************

void BlackboxSession::InitMenu(void) {
  if (rootmenu) delete rootmenu;
  rootmenu = new SessionMenu(this);

  char *line = new char[121], *label = new char[41], *command = new char[81];
  FILE *menu_file = fopen(resource.menuFile, "r");
  if (menu_file != NULL) {
    memset(line, 0, 121);
    memset(label, 0, 41);
    memset(command, 0, 80);
    fgets(line, 120, menu_file);
    int i, ri, len = strlen(line);

    for (i = 0; i < len; ++i)
      if (line[i] == '[') { ++i; break; }
    for (ri = len; ri > 0; --ri)
      if (line[ri] == ']') break;
      
    char *c;
    if (i < ri && ri > 0) {
      c = new char[ri - i + 1];
      strncpy(c, line + i, ri - i);
      *(c + (ri - i)) = '\0';
      
      if (! strcasecmp(c, "begin")) {
	for (i = 0; i < len; ++i)
	  if (line[i] == '(') { ++i; break; }
	for (ri = len; ri > 0; --ri)
	  if (line[ri] == ')') break;
	
	char *l;
	if (i < ri && ri > 0) {
	  l = new char[ri - i + 1];
	  strncpy(l, line + i, ri - i);
	  *(l + (ri - i)) = '\0';
	} else
	  l = (char *) 0;
	
	rootmenu->setMenuLabel(l);
	parseSubMenu(menu_file, rootmenu);

	if (rootmenu->count() == 0) {
	  rootmenu->insert("Restart", B_Restart);
	  rootmenu->insert("Exit", B_Exit);
	}
      } else {
	rootmenu->insert("Restart", B_Restart);
	rootmenu->insert("Exit", B_Exit);
      }
    } else {
      rootmenu->insert("Restart", B_Restart);
      rootmenu->insert("Exit", B_Exit);
    }
  } else {
    // no menu file... fall back on default
    perror(resource.menuFile);
    rootmenu->insert("Restart", B_Restart);
    rootmenu->insert("Exit", B_Exit);
  }

  delete [] command;
  delete [] line;
  delete [] label;
  rootmenu->updateMenu();
}


void BlackboxSession::parseSubMenu(FILE *menu_file, SessionMenu *menu) {
  char *line = new char[121], *label = new char[41], *command = new char[81];

  if (! feof(menu_file)) {
    while (! feof(menu_file)) {
      memset(line, 0, 121);
      memset(label, 0, 41);
      memset(command, 0, 80);
      fgets(line, 120, menu_file);
      int len = strlen(line);

      int i, ri;
      for (i = 0; i < len; ++i)
	if (line[i] == '\"') { ++i; break; }
      for (ri = len; ri > 0; --ri)
	if (line[ri] == '\"') break;
      
      char *l;
      if (i < ri && ri > 0) {
	l = new char[ri - i + 1];
	strncpy(l, line + i, ri - i);
	*(l + (ri - i)) = '\0';
	
	for (i = 0; i < len; ++i)
	  if (line[i] == '(') { ++i; break; }
	for (ri = len; ri > 0; --ri)
	  if (line[ri] == ')') break;
	
	char *c;
	if (i < ri && ri > 0) {
	  c = new char[ri - i + 1];
	  strncpy(c, line + i, ri - i);
	  *(c + (ri - i)) = '\0';
	} else {
	  c = new char[6];
	  strncpy(c, "(nil)", 5);
	  *(c + 5) = '\0';
	}

	if (c) {
	  if (! strncasecmp(c, "Reconfigure", 11)) {
	    menu->insert(l, B_Reconfigure);
	    delete [] c;
	  } else if (! strncasecmp(c, "Restart", 7)) {
	    menu->insert(l, B_Restart);
	    delete [] c;
	  } else if (! strncasecmp(c, "Exit", 4)) {
	    menu->insert(l, B_Exit);
	    delete [] c;
	  } else if (! strncasecmp(c, "Shutdown", 8)) {
	    menu->insert(l, B_Shutdown);
	    delete [] c;
	  } else
	    menu->insert(l, B_Execute, c);
	} else
	  fprintf(stderr, "error in menu file... label must have command\n"
		  "  ex:  \"label\" (command)\n");
      } else {
	for (i = 0; i < len; ++i)
	  if (line[i] == '[') { ++i; break; }
	for (ri = len; ri > 0; --ri)
	  if (line[ri] == ']') break;
      
	char *c;
	if (i < ri && ri > 0) {
	  c = new char[ri - i + 1];
	  strncpy(c, line + i, ri - i);
	  *(c + (ri - i)) = '\0';
	  
	  if (! strcasecmp(c, "begin")) {
	    SessionMenu *newmenu = new SessionMenu(this);

	    for (i = 0; i < len; ++i)
	      if (line[i] == '(') { ++i; break; }
	    for (ri = len; ri > 0; --ri)
	      if (line[ri] == ')') break;
	
	    char *l;
	    if (i < ri && ri > 0) {
	      l = new char[ri - i + 1];
	      strncpy(l, line + i, ri - i);
	      *(l + (ri - i)) = '\0';
	    } else {
     	      l = new char[6];
	      strncpy(l, "(nil)", 5);
	      *(l + 5) = '\0';
	    }
	    
	    delete [] c;
	    newmenu->setMenuLabel(l);
	    newmenu->setMovable(False);
	    parseSubMenu(menu_file, newmenu);
	    menu->insert(l, newmenu);
	    XRaiseWindow(display, newmenu->windowID());
	  } else if (! strcasecmp(c, "end")) {
	    delete [] c;
	    break;
	  } else if (! strcasecmp(c, "restart")) {
	    
	    for (i = 0; i < len; ++i)
	      if (line[i] == '(') { ++i; break; }
	    for (ri = len; ri > 0; --ri)
	      if (line[ri] == ')') break;
	
	    char *l;
	    if (i < ri && ri > 0) {
	      l = new char[ri - i + 1];
	      strncpy(l, line + i, ri - i);
	      *(l + (ri - i)) = '\0';
	    } else {
	      l = new char[6];
	      strncpy(l, "(nil)", 5);
	      *(l + 5) = '\0';
	    }

	    for (i = 0; i < len; ++i)
	      if (line[i] == '{') { ++i; break; }
	    for (ri = len; ri > 0; --ri)
	      if (line[ri] == '}') break;
	
	    char *e;
	    if (i < ri && ri > 0) {
	      e = new char[ri - i + 1];
	      strncpy(e, line + i, ri - i);
	      *(e + (ri - i)) = '\0';
	    } else
	      e = 0;

	    delete [] c;
	    if (e)
	      menu->insert(l, B_RestartOther, e);
	  }
	}
      }
    }
  }

  delete [] command;
  delete [] line;
  delete [] label;
  menu->updateMenu();
}


// *************************************************************************
// Resource reconfiguration
// *************************************************************************

void BlackboxSession::Reconfigure(void) {
  if (! ReconfigureDialog.visible) {
    XGrabServer(display);
    XSynchronize(display, True);
    LoadDefaults();
    
    XGCValues gcv;
    gcv.foreground = getColor("white");
    gcv.function = GXxor;
    gcv.line_width = 2;
    gcv.subwindow_mode = IncludeInferiors;
    gcv.font = resource.font.title->fid;
    XChangeGC(display, opGC, GCForeground|GCFunction|GCSubwindowMode|GCFont,
	      &gcv);
    
    ws_manager->Reconfigure();
    
    Bool m = rootmenu->menuVisible();
    int x = rootmenu->X(), y = rootmenu->Y();
    rootmenu->hideMenu();
    InitMenu();
    if (m) {
      rootmenu->moveMenu(x, y);
      rootmenu->showMenu();
    }
    
    ReconfigureDialog.dialog->Reconfigure();
    XSetWindowBackground(display, ReconfigureDialog.window,
			 toolboxColor().pixel);
    XSetWindowBackground(display, ReconfigureDialog.text_window,
			 toolboxColor().pixel);
    XSetWindowBackground(display, ReconfigureDialog.yes_button,
			 toolboxColor().pixel);
    XSetWindowBackground(display, ReconfigureDialog.no_button,
			 toolboxColor().pixel);
    XSetWindowBorder(display, ReconfigureDialog.yes_button,
		     toolboxTextColor().pixel);
    XSetWindowBorder(display, ReconfigureDialog.no_button,
		     toolboxTextColor().pixel);
    
    XGCValues dgcv;
    dgcv.font = titleFont()->fid;
    dgcv.foreground = toolboxTextColor().pixel;
    XChangeGC(display, ReconfigureDialog.dialogGC, GCFont|GCForeground, &dgcv);
    
    XSynchronize(display, False);
    XUngrabServer(display);
  }
}


void BlackboxSession::createAutoConfigDialog(void) {
  XSetWindowAttributes attrib_create;
  unsigned long create_mask = CWBackPixmap|CWBackPixel|CWBorderPixel|
    CWOverrideRedirect |CWCursor|CWEventMask; 
  
  attrib_create.background_pixmap = None;
  attrib_create.background_pixel = toolboxColor().pixel;
  attrib_create.border_pixel = toolboxTextColor().pixel;
  attrib_create.override_redirect = False;
  attrib_create.cursor = sessionCursor();
  attrib_create.event_mask = SubstructureRedirectMask|ButtonPressMask|
    ButtonReleaseMask|ButtonMotionMask|ExposureMask|EnterWindowMask;
  
  ReconfigureDialog.DialogText[0] =
    "Blackbox has capabilities to perform an automatic";
  ReconfigureDialog.DialogText[1] =
    "reconfiguration, but this capability can best be described as";
  ReconfigureDialog.DialogText[2] =
    "buggy and unreliable.  If you want to allow Blackbox to";
  ReconfigureDialog.DialogText[3] =
    "reconfigure itself, choose \"Yes\" and be aware that Blackbox";
  ReconfigureDialog.DialogText[4] =
    "may dump core.  Choose \"No\" and you can either restart or";
  ReconfigureDialog.DialogText[5] =
    "choose the Reconfigure option from your root menu.";

  ReconfigureDialog.line_h = titleFont()->ascent + titleFont()->descent + 6;
  ReconfigureDialog.text_w = 0;
  ReconfigureDialog.text_h = ReconfigureDialog.line_h * 6;
 
  for (int i = 0; i < 6; i++) {
    int tmp =
      XTextWidth(titleFont(), ReconfigureDialog.DialogText[i],
		 strlen(ReconfigureDialog.DialogText[i])) + 6;
    ReconfigureDialog.text_w = ((ReconfigureDialog.text_w < tmp) ? tmp :
				ReconfigureDialog.text_w);
  }

  ReconfigureDialog.window =
    XCreateWindow(display, root, 200, 200, ReconfigureDialog.text_w + 10,
		  ReconfigureDialog.text_h + (ReconfigureDialog.line_h * 3),
		  0, depth, InputOutput, v, create_mask, &attrib_create);
  XStoreName(display, ReconfigureDialog.window,
	     "Perform Auto-Reconfiguration?");

  ReconfigureDialog.text_window =
    XCreateWindow(display, ReconfigureDialog.window, 5, 5,
		  ReconfigureDialog.text_w, ReconfigureDialog.text_h, 0,
		  depth, InputOutput, v, create_mask, &attrib_create);

  ReconfigureDialog.yes_button = 
    XCreateWindow(display, ReconfigureDialog.window, 5,
		  ReconfigureDialog.text_h + ReconfigureDialog.line_h,
		  (ReconfigureDialog.text_w / 2) - 10,
		  ReconfigureDialog.line_h, 1, depth, InputOutput, v,
		  create_mask, &attrib_create);

  ReconfigureDialog.no_button = 
    XCreateWindow(display, ReconfigureDialog.window,
		  (ReconfigureDialog.text_w / 2) + 10,
		  ReconfigureDialog.text_h + ReconfigureDialog.line_h,
		  (ReconfigureDialog.text_w / 2) - 10,
		  ReconfigureDialog.line_h, 1, depth, InputOutput, v,
		  create_mask, &attrib_create);

  XGCValues gcv;
  gcv.font = titleFont()->fid;
  gcv.foreground = toolboxTextColor().pixel;
  ReconfigureDialog.dialogGC = XCreateGC(display,
					 ReconfigureDialog.text_window,
					 GCFont|GCForeground, &gcv);

  ReconfigureDialog.dialog = new BlackboxWindow(this,
						ReconfigureDialog.window,
						True);

  XMapSubwindows(display, ReconfigureDialog.window);
  XMapWindow(display, ReconfigureDialog.window);
}
