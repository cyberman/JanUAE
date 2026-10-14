# Janus-UAE

JanUAE is a fork of Janus-UAE is a fork of E-UAE 0.8.29-WIP4, 
so the same license(s) apply to it.

Name was changed as of different goals than the e-uae developer.
The main idea of Janus-UAE was the integration into AROS as
described in 

http://www.power2people.org/bounty_001.html

## Installation:

It should be enough, to copy the janus-uae file from the aros 
directory in this archive to your preferred location inside AROS. 

In the Amiga directory you can find usefull applications for 
AmigaOS (transdisk and transrom are not built/tested).

In the aros directory there is a small binary to start AmigaOS 
applications.

## m68k helpers:

You have to start some AmigaOS executables (preferable from 
s:user-startup) to use the integration features:

### janusd

- mouse sync
- rootless mode (coherency)

### clipd

- AmigaOS <-> AROS clipboard sync

### launchd

- start AmigaOS workbench executables from wanderer
- run AmigaOS CLI executables with amirun

Best way is to run them at the end of your 
s:user-startup.

You can find janusd, clipd and launchd in the Amiga directory 
of this archive.

**ATTENTION:**
In order to test everything, it is a *good idea*, to have 
coherency switched off during boot. If everything works as 
expected, you can save coherency in your uae config. 

If coherency is on during boot, you can't see errors happening
until janusd is started. So if you get a 'please insert xy' 
requester before janusd is run, you are lost.

If you get mouse trails, especially right after starting 
AmigaOS 3.x, please replace the Picasso96 rtg.library with the 
one in this package.

### clonescreenmode

If started, it sets the default host screen
resolution as the default m68k screen resolution.
So it is a wise idea, to do this at a very
early monent in your m68k s:startup-sequence

**Usage**:

clonescreenmode [ENV|ENVARC|PRETEND]

ENV: use AROS/x86 host default public screen
     resolution (patch ENV:SYS/screenmode.prefs)
	 
ENVARC: save AROS/x86 host default public screen resolution 
        (patch ENVARC:SYS/screenmode.prefs)

PRETEND: just print resolution and depth to STDOUT

### janus-splash

**Usage**: janus-splash <time in seconds> <text message>
       janus-splash close

You can start janus-uae with -splash_text text and -splash_timeout 
seconds, then a small splash window will be displayed.

It closes either when the GUI or the janus-uae main window is 
displayed (coherency off) or when you call "janus-splash close". 

As long as it is displayed, you can controll the text with

janus-splash <time in seconds> <text message>.

**WARNING:**

All those helpers only work with JanUAE. They won't work 
with WinUAE etc.

## AROS x86 helpers:

### amirun

With the AROS executable you can start any AmigaOS executable,
that is mounted somewhere in AmigaOS.

**Usage:**

amirun <AmigaOS executable> [parameters]

amirun will exit with the following error codes, if something 
goes wrong:

  121: j-uae/janusd not running
  103: out of AROS memory
  205: AmigaOS executable does not exist (in AROS filesystem)

At the moment, you get *no* return code from the AmigaOS side.

So you get *no* feedback, if the AmigaOS executable is within any 
mounted device, if it is an AmigaOS executable at all or if the 
AmigaOS executable returned any error code.

This might change in the future, but most likely not for v1.x.

All output always happens in an AmigaOS window, if there is no 
output of the AmigaOS executable, no window is opened. There is
no way, to redirect the input/output of the AmigaOS executable.

## Configuration:

Janus-UAE uses a file uaerc.config whith the usual uae config syntax. 

You can show/hide the GUI at startup with use_gui=yes or use_gui=no.
The GUI can also be shown/hidden via exchange. The hotkey "ctrl alt j"
toggles GUI visibility.

There might be a problem, if you don't setup your sound correctly.
A linux music player may make the AmigaOS boot hang (Krzysztof).

## Sources:

JanUAE can be built with a GTK user interface for Linux. With the help
of GTK-MUI (v2), all requirements for a GTK-GUI for AROS are met. To
build JanUAE yourself with GTK for AROS, you will need GTK-MUI from

http://sourceforge.net/projects/gtk-mui/

and you will also need glib. Sources available at the same URL.

There are also pre-built libraries at

- http://archives.aros-exec.org/share/development/library/libgtk-mui.i386.tar.gz
- http://archives.aros-exec.org/share/development/library/glib.i386.tar.gz

You should be able to download the janus-uae sources from the same
location, you've got the executable archive from.

The unmodified sources for E-UAE are available at:

http://sourceforge.net/projects/uaedev

## Tools:

If you are missing some helper programs especially for the AmigaOS side
like mousehack or such, you should be able to get them from the uaedev site.

---

## License:

The GPL applies to all files (same as for E-UAE). A copy of 
the GPL is supplied within this archive.

## Disclaimer:

I am no E-UAE developer (and don't want to become one),
I am doing JanUAE ;).
