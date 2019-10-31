# xelt

This is a fork of [Gediminas Morkevicius](https://github.com/l3pp4rd/st) terminal client.


## Requirements

In order to build st you need:

- Xlib header files - location of these might differ, edit config.mk
- xft lib headers.
- Inconsolata.ttf fonts, unless you change it in config.h


### Ubuntu required libraries

    apt-get install libx11-dev libxext-dev libxft-dev fonts-inconsolata
    
    
## Build instructions

**NOTE** concerning [inconsolata font
changes](https://bbs.archlinux.org/viewtopic.php?id=206805), recently it
got bold font support and if you had hinstyle set to **none** it should
not make any impact. Otherwise you may need to set it to none.

### Create application executable:

    ./build.sh make

### Build app and run

    ./build.sh make run

### Build app and run detached from terminal

    ./build.sh make rund

### Clean build directory

    ./build.sh clean


## Running xelt

    ./xelt

Try it with tmux:

    ./xelt -e tmux


## Credits

Based on source code of:
- Aurélien APTEL <aurelien dot aptel at gmail dot com> bt 
- http://st.suckless.org/
- Gediminas Morkevicius <https://github.com/l3pp4rd/st>

