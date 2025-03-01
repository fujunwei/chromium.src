// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "chrome/browser/ui/cocoa/apps/app_shim_menu_controller_mac.h"

#include "base/command_line.h"
#include "base/mac/scoped_nsautorelease_pool.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/apps/app_shim/extension_app_shim_handler_mac.h"
#include "chrome/browser/apps/app_window_registry_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#import "chrome/browser/ui/cocoa/apps/native_app_window_cocoa.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/grit/generated_resources.h"
#include "extensions/browser/app_window/app_window.h"
#include "extensions/common/extension.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/l10n_util_mac.h"

using extensions::Extension;

#include "chrome/browser/devtools/devtools_window.h"

#include "content/nw/src/api/menu/menu.h"
#include "content/nw/src/common/shell_switches.h"

namespace {

// When an app window loses main status, AppKit may make another app window main
// instead. Rather than trying to predict what AppKit will do (which is hard),
// just protect against changes in the event queue that will clobber each other.
int g_window_cycle_sequence_number = 0;

// Whether Custom Cmd+` window cycling is enabled for apps.
bool IsAppWindowCyclingEnabled() {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kDisableAppWindowCycling))
    return false;
  if (command_line->HasSwitch(switches::kEnableAppWindowCycling))
    return true;

  return false;  // Current default.
}

// Gets an item from the main menu given the tag of the top level item
// |menu_tag| and the tag of the item |item_tag|.
NSMenuItem* GetItemByTag(NSInteger menu_tag, NSInteger item_tag) {
  return [[[[NSApp mainMenu] itemWithTag:menu_tag] submenu]
      itemWithTag:item_tag];
}

// Finds a top level menu item using |menu_tag| and creates a new NSMenuItem
// with the same title.
NSMenuItem* NewTopLevelItemFrom(NSInteger menu_tag) {
  NSMenuItem* original = [[NSApp mainMenu] itemWithTag:menu_tag];
  base::scoped_nsobject<NSMenuItem> item([[NSMenuItem alloc]
      initWithTitle:[original title]
             action:nil
      keyEquivalent:@""]);
  DCHECK([original hasSubmenu]);
  base::scoped_nsobject<NSMenu> sub_menu([[NSMenu alloc]
      initWithTitle:[[original submenu] title]]);
  [item setSubmenu:sub_menu];
  return item.autorelease();
}

// Finds an item using |menu_tag| and |item_tag| and adds a duplicate of it to
// the submenu of |top_level_item|.
void AddDuplicateItem(NSMenuItem* top_level_item,
                      NSInteger menu_tag,
                      NSInteger item_tag) {
  base::scoped_nsobject<NSMenuItem> item(
      [GetItemByTag(menu_tag, item_tag) copy]);
  DCHECK(item);
  [[top_level_item submenu] addItem:item];
}

#if 0
// Finds an item with |item_tag| and removes it from the submenu of
// |top_level_item|.
void RemoveMenuItemWithTag(NSMenuItem* top_level_item,
                           NSInteger item_tag,
                           bool remove_following_separator) {
  NSMenu* submenu = [top_level_item submenu];
  NSInteger index = [submenu indexOfItemWithTag:item_tag];
  if (index < 0)
    return;

  [submenu removeItemAtIndex:index];

  if (!remove_following_separator || index == [submenu numberOfItems])
    return;

  NSMenuItem* nextItem = [submenu itemAtIndex:index];
  if ([nextItem isSeparatorItem])
    [submenu removeItem:nextItem];
}

// Sets the menu item with |item_tag| in |top_level_item| visible.
// If |has_alternate| is true, the item immediately following |item_tag| is
// assumed to be its (only) alternate. Since AppKit is unable to hide items
// with alternates, |has_alternate| will cause -[NSMenuItem alternate] to be
// removed when hiding and restored when showing.
void SetItemWithTagVisible(NSMenuItem* top_level_item,
                           NSInteger item_tag,
                           bool visible,
                           bool has_alternate) {
  NSMenu* submenu = [top_level_item submenu];
  NSMenuItem* menu_item = [submenu itemWithTag:item_tag];
  DCHECK(menu_item);

  if (visible != [menu_item isHidden])
    return;

  if (!has_alternate) {
    [menu_item setHidden:!visible];
    return;
  }

  NSInteger next_index = [submenu indexOfItem:menu_item] + 1;
  DCHECK_LT(next_index, [submenu numberOfItems]);

  NSMenuItem* alternate_item = [submenu itemAtIndex:next_index];
  if (!visible) {
    // When hiding (only), we can verify the assumption that the item following
    // |item_tag| is actually an alternate.
    DCHECK([alternate_item isAlternate]);
  }

  // The alternate item visibility should always be in sync.
  DCHECK_EQ([alternate_item isHidden], [menu_item isHidden]);
  [alternate_item setAlternate:visible];
  [alternate_item setHidden:!visible];
  [menu_item setHidden:!visible];
}
#endif

// Return the Extension (if any) associated with the given window. If it is not
// a platform app nor hosted app, but it is a browser, |is_browser| will be set
// to true (otherwise false).
const Extension* GetExtensionForNSWindow(NSWindow* window, bool* is_browser) {
  const Extension* extension = nullptr;
  Browser* browser = nullptr;

  extensions::AppWindow* app_window =
      AppWindowRegistryUtil::GetAppWindowForNativeWindowAnyProfile(window);
  if (app_window) {
    extension = app_window->GetExtension();
  } else {
    // If there is no corresponding AppWindow, this could be a hosted app, so
    // check for a browser.
    browser = chrome::FindBrowserWithWindow(window);
    extension = apps::ExtensionAppShimHandler::MaybeGetAppForBrowser(browser);
  }

  *is_browser = extension == nullptr && browser != nullptr;
  return extension;
}

// Sets or clears NSWindowCollectionBehaviorIgnoresCycle for |window|. Does not
// change NSWindowCollectionBehaviorParticipatesInCycle. That exists, e.g, for
// an NSPanel to override its default behavior, but this should only ever be
// called for Browser windows and App windows (which are not panels).
bool SetWindowParticipatesInCycle(NSWindow* window, bool participates) {
  const NSWindowCollectionBehavior past_behavior = [window collectionBehavior];
  NSWindowCollectionBehavior behavior = past_behavior;
  if (participates)
    behavior &= ~NSWindowCollectionBehaviorIgnoresCycle;
  else
    behavior |= NSWindowCollectionBehaviorIgnoresCycle;

  // Often, there is no change. AppKit has no early exit since the value is
  // derived partially from styleMask and other things, so do our own.
  if (behavior == past_behavior)
    return false;

  [window setCollectionBehavior:behavior];
  return true;
}

// Sets the window cycle list to |app_id|'s windows only.
void SetAppCyclesWindows(const std::string& app_id, int sequence_number) {
  if (g_window_cycle_sequence_number != sequence_number)
    return;

  bool any_change = false;
  for (NSWindow* window : [NSApp windows]) {
    bool is_browser;
    const Extension* extension = GetExtensionForNSWindow(window, &is_browser);
    if (extension && extension->id() == app_id)
      any_change |= SetWindowParticipatesInCycle(window, true);
    else if (extension || is_browser)
      any_change |= SetWindowParticipatesInCycle(window, false);
  }

  // Without the following, -[NSApplication _getLockedWindowListForCycle] will
  // happily return windows that were just set to ignore window cycling. Doing
  // this seems to trick AppKit into updating the window cycle list. But it is a
  // bit scary, so avoid it when there is no change. These attempts were based
  // on the observation that clicking a window twice to switch focus would
  // always work. Also tried (without luck):
  //  - [NSApp setWindowsNeedUpdate:YES],
  //  - Creating a deferred NSWindow and immediately releasing it,
  //  - Calling private methods like [NSApp _unlockWindowListForCycle],
  //  - [NSApp postEvent:[NSEvent otherEventWithType:NSApplicationDefined...
  //      (an attempt to tickle AppKit into an update of some kind),
  //  - Calling synchronously (i.e. not via PostTask) <- this was actually the
  //      initial attempt. Then, switching to PostTask didn't help with this
  //      quirk, but was useful for the sequence number stuff, and
  //  - Re-ordering collection behavior changes to ensure one window was always
  //      participating (i.e. all 'adds' before any 'removes').
  if (any_change)
    [[NSApp keyWindow] makeKeyAndOrderFront:nil];
}

#if 0
// Sets the window cycle list to Chrome browser windows only.
void SetChromeCyclesWindows(int sequence_number) {
  if (g_window_cycle_sequence_number != sequence_number)
    return;

  bool any_change = false;
  for (NSWindow* window : [NSApp windows]) {
    bool is_browser;
    const Extension* extension = GetExtensionForNSWindow(window, &is_browser);
    if (extension || is_browser)
      any_change |= SetWindowParticipatesInCycle(window, is_browser);
  }
  if (any_change)
    [[NSApp keyWindow] makeKeyAndOrderFront:nil];
}
#endif

}  // namespace

// Used by AppShimMenuController to manage menu items that are a copy of a
// Chrome menu item but with a different action. This manages unsetting and
// restoring the original item's key equivalent, so that we can use the same
// key equivalent in the copied item with a different action. If |resourceId_|
// is non-zero, this will also update the title to include the app name.
// If the copy (menuItem) has no key equivalent, and the title does not have the
// app name, then enableForApp and disable do not need to be called. I.e. the
// doppelganger just copies the item and sets a new action.
@interface DoppelgangerMenuItem : NSObject {
 @private
  base::scoped_nsobject<NSMenuItem> menuItem_;
  base::scoped_nsobject<NSMenuItem> sourceItem_;
  base::scoped_nsobject<NSString> sourceKeyEquivalent_;
  int resourceId_;
}

@property(readonly, nonatomic) NSMenuItem* menuItem;

// Get the source item using the tags and create the menu item.
- (id)initWithController:(AppShimMenuController*)controller
                 menuTag:(NSInteger)menuTag
                 itemTag:(NSInteger)itemTag
              resourceId:(int)resourceId
                  action:(SEL)action
           keyEquivalent:(NSString*)keyEquivalent;
// Retain the source item given |menuTag| and |sourceItemTag|. Copy
// the menu item given |menuTag| and |targetItemTag|.
// This is useful when we want a doppelganger with a different source item.
// For example, if there are conflicting key equivalents.
- (id)initWithMenuTag:(NSInteger)menuTag
        sourceItemTag:(NSInteger)sourceItemTag
        targetItemTag:(NSInteger)targetItemTag
        keyEquivalent:(NSString*)keyEquivalent;
// Set the title using |resourceId_| and unset the source item's key equivalent.
- (void)enableForApp:(const Extension*)app;
// Restore the source item's key equivalent.
- (void)disable;
@end

@implementation DoppelgangerMenuItem

- (NSMenuItem*)menuItem {
  return menuItem_;
}

- (id)initWithController:(AppShimMenuController*)controller
                 menuTag:(NSInteger)menuTag
                 itemTag:(NSInteger)itemTag
              resourceId:(int)resourceId
                  action:(SEL)action
           keyEquivalent:(NSString*)keyEquivalent {
  if ((self = [super init])) {
    sourceItem_.reset([GetItemByTag(menuTag, itemTag) retain]);
    DCHECK(sourceItem_);
    sourceKeyEquivalent_.reset([[sourceItem_ keyEquivalent] copy]);
    menuItem_.reset([[NSMenuItem alloc]
        initWithTitle:[sourceItem_ title]
               action:action
        keyEquivalent:keyEquivalent]);
    [menuItem_ setTarget:controller];
    [menuItem_ setTag:itemTag];
    resourceId_ = resourceId;
  }
  return self;
}

- (id)initWithMenuTag:(NSInteger)menuTag
        sourceItemTag:(NSInteger)sourceItemTag
        targetItemTag:(NSInteger)targetItemTag
        keyEquivalent:(NSString*)keyEquivalent {
  if ((self = [super init])) {
    menuItem_.reset([GetItemByTag(menuTag, targetItemTag) copy]);
    sourceItem_.reset([GetItemByTag(menuTag, sourceItemTag) retain]);
    DCHECK(menuItem_);
    DCHECK(sourceItem_);
    sourceKeyEquivalent_.reset([[sourceItem_ keyEquivalent] copy]);
  }
  return self;
}

- (void)enableForApp:(const Extension*)app {
  // It seems that two menu items that have the same key equivalent must also
  // have the same action for the keyboard shortcut to work. (This refers to the
  // original keyboard shortcut, regardless of any overrides set in OSX).
  // In order to let the app menu items have a different action, we remove the
  // key equivalent of the original items and restore them later.
  [sourceItem_ setKeyEquivalent:@""];
  if (!resourceId_)
    return;

  [menuItem_ setTitle:l10n_util::GetNSStringF(resourceId_,
                                              base::UTF8ToUTF16(app->name()))];
}

- (void)disable {
  // Restore the keyboard shortcut to Chrome. This just needs to be set back to
  // the original keyboard shortcut, regardless of any overrides in OSX. The
  // overrides still work as they are based on the title of the menu item.
  [sourceItem_ setKeyEquivalent:sourceKeyEquivalent_];
}

@end

@interface AppShimMenuController ()
// Construct the NSMenuItems for apps.
- (void)buildAppMenuItems;
// Register for NSWindow notifications.
- (void)registerEventHandlers;
// If the window is an app window, add or remove menu items.
- (void)windowMainStatusChanged:(NSNotification*)notification;
// Called when |app| becomes the main window in the Chrome process.
- (void)appBecameMain:(const Extension*)app;
// Called when there is no main window, or if the main window is not an app.
- (void)chromeBecameMain;
// Add menu items for an app and hide Chrome menu items.
- (void)addMenuItems:(const Extension*)app;
// If the window belongs to the currently focused app, remove the menu items and
// unhide Chrome menu items.
- (void)removeMenuItems;
// If the currently focused window belongs to a platform app, quit the app.
- (void)quitCurrentPlatformApp;
// If the currently focused window belongs to a platform app, hide the app.
- (void)hideCurrentPlatformApp;
// If the currently focused window belongs to a platform app, focus the app.
- (void)focusCurrentPlatformApp;
- (void)showDevtools;
@end

@implementation AppShimMenuController

- (id)init {
  if ((self = [super init])) {
    [self buildAppMenuItems];
    [self registerEventHandlers];
  }
  return self;
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  [super dealloc];
}

- (void)buildAppMenuItems {
  aboutDoppelganger_.reset([[DoppelgangerMenuItem alloc]
      initWithController:self
                 menuTag:IDC_CHROME_MENU
                 itemTag:IDC_ABOUT
              resourceId:IDS_ABOUT_MAC
                  action:nil
           keyEquivalent:@""]);
  hideDoppelganger_.reset([[DoppelgangerMenuItem alloc]
      initWithController:self
                 menuTag:IDC_CHROME_MENU
                 itemTag:IDC_HIDE_APP
              resourceId:IDS_HIDE_APP_MAC
                  action:@selector(hideCurrentPlatformApp)
           keyEquivalent:@"h"]);
  quitDoppelganger_.reset([[DoppelgangerMenuItem alloc]
      initWithController:self
                 menuTag:IDC_CHROME_MENU
                 itemTag:IDC_EXIT
              resourceId:IDS_EXIT_MAC
                  action:@selector(quitCurrentPlatformApp)
           keyEquivalent:@"q"]);
#if 0
  newDoppelganger_.reset([[DoppelgangerMenuItem alloc]
      initWithController:self
                 menuTag:IDC_FILE_MENU
                 itemTag:IDC_NEW_WINDOW
              resourceId:0
                  action:nil
           keyEquivalent:@"n"]);
  // Since the "Close Window" menu item will have the same shortcut as "Close
  // Tab" on the Chrome menu, we need to create a doppelganger.
  closeWindowDoppelganger_.reset([[DoppelgangerMenuItem alloc]
                initWithMenuTag:IDC_FILE_MENU
                  sourceItemTag:IDC_CLOSE_TAB
                  targetItemTag:IDC_CLOSE_WINDOW
                  keyEquivalent:@"w"]);
  // For apps, the "Window" part of "New Window" is dropped to match the default
  // menu set given to Cocoa Apps.
  [[newDoppelganger_ menuItem] setTitle:l10n_util::GetNSString(IDS_NEW_MAC)];
  openDoppelganger_.reset([[DoppelgangerMenuItem alloc]
      initWithController:self
                 menuTag:IDC_FILE_MENU
                 itemTag:IDC_OPEN_FILE
              resourceId:0
                  action:nil
           keyEquivalent:@"o"]);
#endif
  allToFrontDoppelganger_.reset([[DoppelgangerMenuItem alloc]
      initWithController:self
                 menuTag:IDC_WINDOW_MENU
                 itemTag:IDC_ALL_WINDOWS_FRONT
              resourceId:0
                  action:@selector(focusCurrentPlatformApp)
           keyEquivalent:@""]);

  // The app's menu.
  appMenuItem_.reset([[NSMenuItem alloc] initWithTitle:@""
                                                action:nil
                                         keyEquivalent:@""]);
  base::scoped_nsobject<NSMenu> appMenu([[NSMenu alloc] initWithTitle:@""]);
  [appMenuItem_ setSubmenu:appMenu];
  [appMenu setAutoenablesItems:NO];

  [appMenu addItem:[aboutDoppelganger_ menuItem]];
  [[aboutDoppelganger_ menuItem] setEnabled:NO];  // Not implemented yet.
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItem:[hideDoppelganger_ menuItem]];
  [appMenu addItem:[NSMenuItem separatorItem]];
  [appMenu addItem:[quitDoppelganger_ menuItem]];

#if 0
  // File menu.
  fileMenuItem_.reset([NewTopLevelItemFrom(IDC_FILE_MENU) retain]);
  [[fileMenuItem_ submenu] addItem:[newDoppelganger_ menuItem]];
  [[fileMenuItem_ submenu] addItem:[openDoppelganger_ menuItem]];
  [[fileMenuItem_ submenu] addItem:[NSMenuItem separatorItem]];
  [[fileMenuItem_ submenu] addItem:[closeWindowDoppelganger_ menuItem]];
#endif

  // Edit menu. We copy the menu because the last two items, "Start Dictation"
  // and "Special Characters" are added by OSX, so we can't copy them
  // explicitly.
  editMenuItem_.reset([[[NSApp mainMenu] itemWithTag:IDC_EDIT_MENU] copy]);

#if 0
  // View menu. Remove "Always Show Bookmark Bar" and separator.
  viewMenuItem_.reset([[[NSApp mainMenu] itemWithTag:IDC_VIEW_MENU] copy]);
  RemoveMenuItemWithTag(viewMenuItem_, IDC_SHOW_BOOKMARK_BAR, YES);

  // History menu.
  historyMenuItem_.reset([NewTopLevelItemFrom(IDC_HISTORY_MENU) retain]);
  AddDuplicateItem(historyMenuItem_, IDC_HISTORY_MENU, IDC_BACK);
  AddDuplicateItem(historyMenuItem_, IDC_HISTORY_MENU, IDC_FORWARD);

#endif
  // Window menu.
  windowMenuItem_.reset([NewTopLevelItemFrom(IDC_WINDOW_MENU) retain]);
  AddDuplicateItem(windowMenuItem_, IDC_WINDOW_MENU, IDC_MINIMIZE_WINDOW);
  AddDuplicateItem(windowMenuItem_, IDC_WINDOW_MENU, IDC_MAXIMIZE_WINDOW);
  [[windowMenuItem_ submenu] addItem:[NSMenuItem separatorItem]];
  [[windowMenuItem_ submenu] addItem:[allToFrontDoppelganger_ menuItem]];

#if defined(NWJS_SDK)
  bool enable_devtools = true;
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kDisableDevTools))
    enable_devtools = false;

  if (enable_devtools) {
  [[windowMenuItem_ submenu] setAutoenablesItems:NO];
  NSMenuItem* item = [[NSMenuItem alloc]
		      initWithTitle:@"Devtools"
		      action:@selector(showDevtools)
                      keyEquivalent:@"i"];
  [item setTag:IDC_DEV_TOOLS_CONSOLE];
  [item setTarget:self];
  [item setEnabled:YES];
  [item setKeyEquivalentModifierMask:NSCommandKeyMask | NSAlternateKeyMask];
  [[windowMenuItem_ submenu] addItem:item];
  }
#endif
}

- (void)registerEventHandlers {
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(windowMainStatusChanged:)
             name:NSWindowDidBecomeMainNotification
           object:nil];

  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(windowMainStatusChanged:)
             name:NSWindowDidResignMainNotification
           object:nil];
}

- (void)windowMainStatusChanged:(NSNotification*)notification {
  // A Yosemite AppKit bug causes this notification to be sent during the
  // -dealloc for a specific NSWindow. Any autoreleases sent to that window
  // must be drained before the window finishes -dealloc. In this method, an
  // autorelease is sent by the invocation of [NSApp windows].
  // http://crbug.com/406944.
  base::mac::ScopedNSAutoreleasePool pool;

  NSString* name = [notification name];
  if ([name isEqualToString:NSWindowDidBecomeMainNotification]) {
    id window = [notification object];
    bool is_browser;
    const Extension* extension = GetExtensionForNSWindow(window, &is_browser);
    // Ignore is_browser: if a window becomes main that does not belong to an
    // extension or browser, treat it the same as switching to a browser.

    extensions::AppWindow* appWindow =
        AppWindowRegistryUtil::GetAppWindowForNativeWindowAnyProfile(
            window);
    if (extension) {
      if (appWindow->menu_) {
        [NSApp setMainMenu:appWindow->menu_->menu_];
        return;
      }
      [self appBecameMain:extension];
    }
    else
      [self chromeBecameMain];
  } else if ([name isEqualToString:NSWindowDidResignMainNotification]) {
    // When a window resigns main status, reset back to the Chrome menu.
    // In the past we've tried:
    // - Only doing this when a window closes, but this would not be triggered
    // when an app becomes hidden (Cmd+h), and there are no Chrome windows to
    // become main.
    // - Scanning [NSApp windows] to predict whether we could
    // expect another Chrome window to become main, and skip the reset. However,
    // panels need to do strange things during window close to ensure panels
    // never get chosen for key status over a browser window (which is likely
    // because they are given an elevated [NSWindow level]). Trying to handle
    // this case is not robust.
    //
    // Unfortunately, resetting the menu to Chrome
    // unconditionally means that if another packaged app window becomes key,
    // the menu will flicker. TODO(tapted): Investigate restoring the logic when
    // the panel code is removed.
    [self chromeBecameMain];
  } else {
    NOTREACHED();
  }
}

- (void)appBecameMain:(const Extension*)app {
  if (appId_ == app->id())
    return;

  if (!appId_.empty())
    return;
    // #4591: when app sets menubar and launch another chrome app,
    // removeMenuItems will try to remove appmenuitem which isn't
    // in the main menu; then app will crash.
    // so after this function runs once, we just do nothing and return here
    //[self removeMenuItems];

  appId_ = app->id();
  [self addMenuItems:app];
  if (IsAppWindowCyclingEnabled()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::Bind(&SetAppCyclesWindows, appId_,
                              ++g_window_cycle_sequence_number));
  }
}

- (void)chromeBecameMain {
#if 0
  if (appId_.empty())
    return;

  appId_.clear();
  [self removeMenuItems];
  if (IsAppWindowCyclingEnabled()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::Bind(&SetChromeCyclesWindows, ++g_window_cycle_sequence_number));
  }
#endif
}

- (void)addMenuItems:(const Extension*)app {
  DCHECK_EQ(appId_, app->id());
  NSString* title = base::SysUTF8ToNSString(app->name());

  // Hide Chrome menu items.
  NSMenu* mainMenu = [NSApp mainMenu];
  for (NSMenuItem* item in [mainMenu itemArray])
    [item setHidden:YES];

  [aboutDoppelganger_ enableForApp:app];
  [hideDoppelganger_ enableForApp:app];
  [quitDoppelganger_ enableForApp:app];
  //[newDoppelganger_ enableForApp:app];
  //[openDoppelganger_ enableForApp:app];
  //[closeWindowDoppelganger_ enableForApp:app];

  [appMenuItem_ setTitle:base::SysUTF8ToNSString(appId_)];
  [[appMenuItem_ submenu] setTitle:title];

  [mainMenu addItem:appMenuItem_];
#if 0
  [mainMenu addItem:fileMenuItem_];

  SetItemWithTagVisible(editMenuItem_,
                        IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE,
                        app->is_hosted_app(), true);
  SetItemWithTagVisible(editMenuItem_, IDC_FIND_MENU, app->is_hosted_app(),
                        false);
#endif
  [mainMenu addItem:editMenuItem_];

#if 0
  if (app->is_hosted_app()) {
    [mainMenu addItem:viewMenuItem_];
    [mainMenu addItem:historyMenuItem_];
  }
#endif
  [mainMenu addItem:windowMenuItem_];
}

- (void)removeMenuItems {
  NSMenu* mainMenu = [NSApp mainMenu];
  [mainMenu removeItem:appMenuItem_];
  //[mainMenu removeItem:fileMenuItem_];
#if 0
  if ([mainMenu indexOfItem:viewMenuItem_] >= 0)
    [mainMenu removeItem:viewMenuItem_];
  if ([mainMenu indexOfItem:historyMenuItem_] >= 0)
    [mainMenu removeItem:historyMenuItem_];
#endif
  [mainMenu removeItem:editMenuItem_];
  [mainMenu removeItem:windowMenuItem_];

  // Restore the Chrome main menu bar.
  for (NSMenuItem* item in [mainMenu itemArray])
    [item setHidden:NO];

  [aboutDoppelganger_ disable];
  [hideDoppelganger_ disable];
  [quitDoppelganger_ disable];
  //[newDoppelganger_ disable];
  //[openDoppelganger_ disable];
  //[closeWindowDoppelganger_ disable];
}

- (void)quitCurrentPlatformApp {
  extensions::AppWindow* appWindow =
      AppWindowRegistryUtil::GetAppWindowForNativeWindowAnyProfile(
          [NSApp keyWindow]);
  if (appWindow) {
    apps::ExtensionAppShimHandler::QuitAppForWindow(appWindow, true);
  } else {
    Browser* browser = chrome::FindBrowserWithWindow([NSApp keyWindow]);
    const Extension* extension =
        apps::ExtensionAppShimHandler::MaybeGetAppForBrowser(browser);
    if (extension)
      apps::ExtensionAppShimHandler::QuitHostedAppForWindow(browser->profile(),
                                                            extension->id());
  }
}

- (void)hideCurrentPlatformApp {
  extensions::AppWindow* appWindow =
      AppWindowRegistryUtil::GetAppWindowForNativeWindowAnyProfile(
          [NSApp keyWindow]);
  if (appWindow) {
    apps::ExtensionAppShimHandler::HideAppForWindow(appWindow);
  } else {
    Browser* browser = chrome::FindBrowserWithWindow([NSApp keyWindow]);
    const Extension* extension =
        apps::ExtensionAppShimHandler::MaybeGetAppForBrowser(browser);
    if (extension)
      apps::ExtensionAppShimHandler::HideHostedApp(browser->profile(),
                                                   extension->id());
  }
}

- (void)focusCurrentPlatformApp {
  extensions::AppWindow* appWindow =
      AppWindowRegistryUtil::GetAppWindowForNativeWindowAnyProfile(
          [NSApp keyWindow]);
  if (appWindow)
    apps::ExtensionAppShimHandler::FocusAppForWindow(appWindow);
}

- (void)showDevtools {
  extensions::AppWindow* appWindow =
      AppWindowRegistryUtil::GetAppWindowForNativeWindowAnyProfile(
          [NSApp keyWindow]);
  if (appWindow)
    DevToolsWindow::OpenDevToolsWindow(appWindow->web_contents());
}

@end
