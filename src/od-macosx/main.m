/*
 * E-UAE - The portable Amiga emulator
 *
 * Main entry point for our Cocoa-ized app and support
 * routines.
 *
 * Based on SDLMain.m from libSDL.
 * Initial Version: Darrell Walisser <dwaliss1@purdue.edu>
 * Non-NIB-Code & other changes: Max Horn <max@quendi.de>
 *
 * Adaption for E-UAE:
 * Steven J. Saunders
 * Richard Drummond
 */

#import "sysconfig.h"
#import "sysdeps.h"

#import "options.h"
#import "uae.h"

#ifdef USE_SDL
# import "SDL.h"
#endif

#import "main.h"
#import <sys/param.h> /* for MAXPATHLEN */
#import <unistd.h>

/* Use this flag to determine whether we use CPS (docking) or not */
#define	SDL_USE_CPS	1

#ifdef SDL_USE_CPS
/* Portions of CPS.h */
typedef struct CPSProcessSerNum
{
    UInt32	lo;
    UInt32	hi;
} CPSProcessSerNum;

extern OSErr CPSGetCurrentProcess (CPSProcessSerNum *psn);
extern OSErr CPSEnableForegroundOperation (CPSProcessSerNum *psn, UInt32 _arg2, UInt32 _arg3, UInt32 _arg4, UInt32 _arg5);
extern OSErr CPSSetFrontProcess (CPSProcessSerNum *psn);

#endif /* SDL_USE_CPS */

static int     gArgc;
static char  **gArgv;
BOOL           gFinderLaunch;

/****i* januae-od-macosx/getApplicationName *******************************
*
*   NAME
*       getApplicationName()
*
*   SYNOPSIS
*       NSString *getApplicationName (void);
*
*   FUNCTION
*       Return the main bundle CFBundleName, falling back to the current
*       process name if the bundle name is absent or empty.
*
*   RESULT
*       An NSString provided by the bundle dictionary or NSProcessInfo. The
*       caller does not acquire ownership.
*
*************************************************************************
*
*/
NSString *getApplicationName (void)
{
    NSDictionary *dict;
    NSString *appName = 0;

    /* Determine the application name */
    dict = (NSDictionary *) CFBundleGetInfoDictionary (CFBundleGetMainBundle ());
    if (dict)
	appName = [dict objectForKey: @"CFBundleName"];

    if (![appName length])
	appName = [[NSProcessInfo processInfo] processName];

    return appName;
}

/* Fix warnings generated when using the setAppleMenu method and compiling
in Tiger or later */
@interface NSApplication (EUAE)
- (void)setAppleMenu:(NSMenu *)menu;
@end


@interface EUAE_Application : NSApplication
@end

@implementation EUAE_Application

/****i* januae-od-macosx/EUAE_Application_terminate ***********************
*
*   NAME
*       -[EUAE_Application terminate:]
*
*   SYNOPSIS
*       - (void)terminate:(id)sender;
*
*   FUNCTION
*       Request emulator termination by queuing an SDL_QUIT event when
*       USE_SDL is enabled.
*
*   INPUTS
*       sender - Unused menu sender.
*
*   NOTES
*       The non-SDL branch has no termination action; this method does not
*       invoke the superclass termination handler.
*
*************************************************************************
*
*/
- (void)terminate:(id)sender
{
#ifdef USE_SDL
    /* Post a SDL_QUIT event */
    SDL_Event event;
    event.type = SDL_QUIT;
    SDL_PushEvent (&event);
#else
    /* FIXME */
#endif
}

/****i* januae-od-macosx/EUAE_Application_sendEvent ***********************
*
*   NAME
*       -[EUAE_Application sendEvent:]
*
*   SYNOPSIS
*       - (void)sendEvent:(NSEvent *)anEvent;
*
*   FUNCTION
*       Forward non-key events and Command-modified key-up or key-down
*       events to NSApplication; suppress other key events.
*
*   INPUTS
*       anEvent - Cocoa event submitted to the application.
*
*************************************************************************
*
*/
- (void)sendEvent:(NSEvent *)anEvent
{
    if (NSKeyDown == [anEvent type] || NSKeyUp == [anEvent type]) {
	if ([anEvent modifierFlags] & NSCommandKeyMask)
	    [super sendEvent: anEvent];
    } else
	[super sendEvent: anEvent];
}

@end


/* The main class of the application, the application's delegate */
@implementation EUAE_Main

/****i* januae-od-macosx/EUAE_Main_setupWorkingDirectory ******************
*
*   NAME
*       -[EUAE_Main setupWorkingDirectory:]
*
*   SYNOPSIS
*       - (void) setupWorkingDirectory:(BOOL)shouldChdir;
*
*   FUNCTION
*       When requested, change the current working directory to the parent
*       of the application bundle.
*
*   INPUTS
*       shouldChdir - Whether to attempt the directory change.
*
*   NOTES
*       File-system path conversion must succeed; chdir() failure is
*       ignored. The directory change affects the process.
*
*************************************************************************
*
*/
- (void) setupWorkingDirectory:(BOOL)shouldChdir
{
    if (shouldChdir) {
	char parentdir[MAXPATHLEN];
	CFURLRef url  = CFBundleCopyBundleURL (CFBundleGetMainBundle());
	CFURLRef url2 = CFURLCreateCopyDeletingLastPathComponent (0, url);

	if (CFURLGetFileSystemRepresentation (url2, true, (UInt8 *)parentdir, MAXPATHLEN)) {
	    chdir (parentdir) == 0;   /* chdir to the binary app's parent */
	}
	CFRelease (url);
	CFRelease (url2);
    }
}

/****i* januae-od-macosx/setApplicationMenu *******************************
*
*   NAME
*       setApplicationMenu()
*
*   SYNOPSIS
*       static void setApplicationMenu (void);
*
*   FUNCTION
*       Install the Cocoa application menu with About, visibility and Quit
*       actions.
*
*   NOTES
*       The standard Command-key menu shortcuts are intentionally left
*       unset for the emulator.
*
*************************************************************************
*
*/
static void setApplicationMenu (void)
{
    /* warning: this code is very odd */
    NSMenu *appleMenu;
    NSMenuItem *menuItem;
    NSString *title;
    NSString *appName;

    appName = getApplicationName ();
    appleMenu = [[NSMenu alloc] initWithTitle:@""];

    /* Add menu items */
    title = [@"About " stringByAppendingString:appName];
    [appleMenu addItemWithTitle:title action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];

    [appleMenu addItem:[NSMenuItem separatorItem]];

    title = [@"Hide " stringByAppendingString:appName];
    /* E-UAE: Removed @"h" key equivalent so it doesn't interfere with using the command key as an emulated amiga key */
    [appleMenu addItemWithTitle:title action:@selector(hide:) keyEquivalent:@""];

    /* E-UAE: Removed @"h" key equivalent so it doesn't interfere with using the command key as an emulated amiga key */
    menuItem = (NSMenuItem *)[appleMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@""];
    [menuItem setKeyEquivalentModifierMask:(NSAlternateKeyMask|NSCommandKeyMask)];

    [appleMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];

    [appleMenu addItem:[NSMenuItem separatorItem]];

    title = [@"Quit " stringByAppendingString:appName];
    /* E-UAE: Removed @"q" key equivalent so it doesn't interfere with using the command key as an emulated amiga key */
    [appleMenu addItemWithTitle:title action:@selector(terminate:) keyEquivalent:@""];


    /* Put menu into the menubar */
    menuItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    [menuItem setSubmenu:appleMenu];
    [[NSApp mainMenu] addItem:menuItem];

    /* Tell the application object that this is now the application menu */
    [NSApp setAppleMenu:appleMenu];

    /* Finally give up our references to the objects */
    [appleMenu release];
    [menuItem release];
}

/* Create a window menu */
/****i* januae-od-macosx/setupWindowMenu **********************************
*
*   NAME
*       setupWindowMenu()
*
*   SYNOPSIS
*       static void setupWindowMenu (void);
*
*   FUNCTION
*       Install the Cocoa Window menu with a Minimize command.
*
*   NOTES
*       The Minimize command has no key equivalent, preserving Command-key
*       delivery to emulation.
*
*************************************************************************
*
*/
static void setupWindowMenu (void)
{
    NSMenu      *windowMenu;
    NSMenuItem  *windowMenuItem;
    NSMenuItem  *menuItem;

    windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];

    /* "Minimize" item */
    /* E-UAE: Removed @"m" key equivalent so it doesn't interfere with using the command key as an emulated amiga key */
    menuItem = [[NSMenuItem alloc] initWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@""];
    [windowMenu addItem:menuItem];
    [menuItem release];

    /* Put menu into the menubar */
    windowMenuItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
    [windowMenuItem setSubmenu:windowMenu];
    [[NSApp mainMenu] addItem:windowMenuItem];

    /* Tell the application object that this is now the window menu */
    [NSApp setWindowsMenu:windowMenu];

    /* Finally give up our references to the objects */
    [windowMenu release];
    [windowMenuItem release];
}

/* Replacement for NSApplicationMain */
/****i* januae-od-macosx/CustomApplicationMain ****************************
*
*   NAME
*       CustomApplicationMain()
*
*   SYNOPSIS
*       static void CustomApplicationMain (int argc, char **argv);
*
*   FUNCTION
*       Initialize the Cocoa application, optionally request foreground
*       activation through CPS, create menus and a delegate, then run the
*       application event loop.
*
*   INPUTS
*       argc - Argument count; not used by this function.
*       argv - Argument vector; not used by this function.
*
*   NOTES
*       Returns after NSApp run ends and releases its delegate and
*       autorelease pool. The launch callback performs emulator startup.
*
*************************************************************************
*
*/
static void CustomApplicationMain (int argc, char **argv)
{
    NSAutoreleasePool	*pool = [[NSAutoreleasePool alloc] init];
    EUAE_Main		*euae_main;

    /* Ensure the application object is initialised */
    [EUAE_Application sharedApplication];

#ifdef SDL_USE_CPS
    {
	CPSProcessSerNum PSN;
	/* Tell the dock about us */
	if (!CPSGetCurrentProcess(&PSN))
	    if (!CPSEnableForegroundOperation(&PSN,0x03,0x3C,0x2C,0x1103))
		if (!CPSSetFrontProcess(&PSN))
		    [EUAE_Application sharedApplication];
    }
#endif /* SDL_USE_CPS */

    /* Set up the menubar */
    [NSApp setMainMenu:[[NSMenu alloc] init]];
    setApplicationMenu ();
    setupWindowMenu ();

    /* Create SDLMain and make it the app delegate */
    euae_main = [[EUAE_Main alloc] init];
    [NSApp setDelegate:euae_main];

    /* Start the main event loop */
    [NSApp run];

    [euae_main release];
    [pool release];
}

/****i* januae-od-macosx/EUAE_Main_applicationShouldOpenUntitledFile ******
*
*   NAME
*       -[EUAE_Main applicationShouldOpenUntitledFile:]
*
*   SYNOPSIS
*       - (BOOL) applicationShouldOpenUntitledFile:(NSApplication *)sender;
*
*   FUNCTION
*       Suppress creation of an untitled document at application startup.
*
*   INPUTS
*       sender - Unused application object.
*
*   RESULT
*       Always NO.
*
*************************************************************************
*
*/
- (BOOL) applicationShouldOpenUntitledFile:(NSApplication *)sender
{
    return NO;
}

extern NSString *finderLaunchFilename;
/****i* januae-od-macosx/EUAE_Main_application_openFile *******************
*
*   NAME
*       -[EUAE_Main application:openFile:]
*
*   SYNOPSIS
*       - (BOOL) application:(NSApplication *)theApplication
*       openFile:(NSString *)filename;
*
*   FUNCTION
*       Store a copy of the Finder-selected file name, releasing any
*       previously retained name.
*
*   INPUTS
*       theApplication - Unused application object.
*       filename - File path supplied by Cocoa; may be nil.
*
*   RESULT
*       YES when filename is non-nil; otherwise NO.
*
*   NOTES
*       The saved global finderLaunchFilename is retained until replaced or
*       released elsewhere.
*
*************************************************************************
*
*/
- (BOOL) application:(NSApplication *)theApplication openFile:(NSString *)filename
{
    if (filename != nil) {
	if (finderLaunchFilename != nil)
	    [finderLaunchFilename release];
	finderLaunchFilename = [[NSString alloc] initWithString:filename];
	return YES;
    }

    return NO;
}

/****i* januae-od-macosx/EUAE_Main_applicationDidFinishLaunching **********
*
*   NAME
*       -[EUAE_Main applicationDidFinishLaunching:]
*
*   SYNOPSIS
*       - (void) applicationDidFinishLaunching: (NSNotification *) note;
*
*   FUNCTION
*       Set the working directory for Finder launches, initialize SDL when
*       enabled, run the emulator if initialization returns nonzero, then
*       terminate the process.
*
*   INPUTS
*       note - Unused launch notification.
*
*   NOTES
*       With USE_SDL, real_main() is skipped if init_sdl() returns false.
*       The process exits with EXIT_SUCCESS regardless of that outcome.
*
*************************************************************************
*
*/
- (void) applicationDidFinishLaunching: (NSNotification *) note
{
    /* Set the working directory to the .app's parent directory */
    [self setupWorkingDirectory:gFinderLaunch];

#ifdef USE_SDL
    setenv ("SDL_ENABLEAPPEVENTS", "1", 1);


    if (init_sdl ()) 
#endif
    {
      
	/* Hand off to main application code */
	real_main (gArgc, gArgv);
    }

    /* We're done, thank you for playing */
    exit (EXIT_SUCCESS);
}

@end


#ifdef main
#  undef main
#endif

/****i* januae-od-macosx/main *********************************************
*
*   NAME
*       main()
*
*   SYNOPSIS
*       int main (int argc, char **argv);
*
*   FUNCTION
*       Record launch arguments and run the Cocoa application. A Finder
*       process-serial-number argument selects Finder launch handling and
*       configures the per-user log path.
*
*   INPUTS
*       argc - Argument count.
*       argv - Argument vector.
*
*   RESULT
*       0 if the Cocoa application loop returns.
*
*   NOTES
*       Finder launches pass an effective single argument to the emulator;
*       the original argv pointer is retained.
*
*************************************************************************
*
*/
int main (int argc, char **argv)
{
    char logfile_path[MAX_PATH] = "~/Library/Logs/E-UAE.log";

    /* Copy the arguments into a global variable */
    /* This is passed if we are launched by double-clicking */
    if (argc >= 2 && strncmp (argv[1], "-psn", 4) == 0 ) {
	gArgc = 1;
	gFinderLaunch = YES;
        gFinderLaunch = YES;
        cfgfile_subst_home (logfile_path, MAX_PATH);
        set_logfile (logfile_path);
    } else {
	gArgc = argc;
	gFinderLaunch = NO;
    }
    gArgv = argv;

    CustomApplicationMain (argc, argv);

    return 0;
}


/****i* januae-od-macosx/setup_brkhandler *********************************
*
*   NAME
*       setup_brkhandler()
*
*   SYNOPSIS
*       void setup_brkhandler (void);
*
*   FUNCTION
*       Perform no signal-handler setup on the Cocoa target.
*
*************************************************************************
*
*/
void setup_brkhandler (void)
{
}


/****i* januae-od-macosx/target_save_options ******************************
*
*   NAME
*       target_save_options()
*
*   SYNOPSIS
*       void target_save_options (FILE *f, const struct uae_prefs *p);
*
*   FUNCTION
*       Write no Cocoa-specific configuration settings.
*
*   INPUTS
*       f - Unused output stream.
*       p - Unused preference set.
*
*************************************************************************
*
*/
void target_save_options (FILE *f, const struct uae_prefs *p)
{
}

/****i* januae-od-macosx/target_parse_option ******************************
*
*   NAME
*       target_parse_option()
*
*   SYNOPSIS
*       int target_parse_option (struct uae_prefs *p, const char *option,
*       const char *value);
*
*   FUNCTION
*       Decline all Cocoa-specific configuration options.
*
*   INPUTS
*       p - Unused preference set.
*       option - Unused option name.
*       value - Unused option value.
*
*   RESULT
*       Always 0.
*
*************************************************************************
*
*/
int target_parse_option (struct uae_prefs *p, const char *option, const char *value)
{
    return 0;
}

/****i* januae-od-macosx/target_default_options ***************************
*
*   NAME
*       target_default_options()
*
*   SYNOPSIS
*       void target_default_options (struct uae_prefs *p);
*
*   FUNCTION
*       Leave the Cocoa-specific preferences unchanged.
*
*   INPUTS
*       p - Unused preference set.
*
*************************************************************************
*
*/
void target_default_options (struct uae_prefs *p)
{
}
