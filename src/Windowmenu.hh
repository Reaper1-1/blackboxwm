//
// Windowmenu.hh for Blackbox - an X11 Window manager
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

#ifndef __Windowmenu_hh
#define __Windowmenu_hh

// forward declaration
class Windowmenu;
class SendtoWorkspaceMenu;

class Blackbox;
class BlackboxWindow;
class Toolbar;

#include "Basemenu.hh"


class Windowmenu : public Basemenu {
private:
  BlackboxWindow *window;
  Toolbar *toolbar;
  SendtoWorkspaceMenu *sendToMenu;
  
  
protected:
  virtual void itemSelected(int, int);


public:
  Windowmenu(BlackboxWindow *, Blackbox *);
  virtual ~Windowmenu(void);

  void Reconfigure(void);

  SendtoWorkspaceMenu *SendToMenu(void) { return sendToMenu; }
};


class SendtoWorkspaceMenu : public Basemenu {
private:
  BlackboxWindow *window;
  Toolbar *toolbar;


protected:
  virtual void itemSelected(int, int);


public:
  SendtoWorkspaceMenu(BlackboxWindow *, Blackbox *);

  void Show(void);
  void Update(void);
};


#endif // __Windowmenu_hh
