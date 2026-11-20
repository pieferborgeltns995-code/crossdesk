#include "platform/window_drag.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <objc/runtime.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

@interface CrossDeskStreamFullscreenTarget : NSObject
- (void)toggleStreamFullscreen:(id)sender;
@end

@implementation CrossDeskStreamFullscreenTarget
- (void)toggleStreamFullscreen:(id)sender {
  (void)sender;
  crossdesk::SetStreamWindowFullscreen(
      !crossdesk::IsStreamWindowFullscreen());
}
@end

namespace crossdesk {
namespace {

__weak NSWindow *gallery_window = nil;
bool gallery_fullscreen = false;
NSRect gallery_restore_frame = NSZeroRect;
NSWindowStyleMask gallery_restore_style = NSWindowStyleMaskTitled;
__weak NSWindow *stream_window = nil;
__weak NSView *stream_slint_view = nil;
CrossDeskStreamFullscreenTarget *stream_fullscreen_target = nil;
bool stream_fullscreen = false;
NSRect stream_restore_frame = NSZeroRect;
NSWindowStyleMask stream_restore_style_mask = NSWindowStyleMaskTitled;
NSWindowTitleVisibility stream_restore_title_visibility =
    NSWindowTitleVisible;
BOOL stream_restore_titlebar_transparent = NO;
BOOL stream_restore_movable = YES;
BOOL stream_restore_close_hidden = NO;
BOOL stream_restore_miniaturize_hidden = NO;
BOOL stream_restore_zoom_hidden = NO;
bool fullscreen_presentation_saved = false;
NSApplicationPresentationOptions saved_fullscreen_presentation_options =
    NSApplicationPresentationDefault;

struct FileDialogRequest {
  std::string title;
  std::function<void(std::string)> completion;
  std::string filename;
};
std::shared_ptr<FileDialogRequest> file_dialog_request;
NSSavePanel *file_dialog = nil;
NSOpenPanel *prepared_file_dialog = nil;
uint64_t file_dialog_prepare_generation = 0;

NSOpenPanel *CreateFileDialog() {
  NSOpenPanel *panel = [NSOpenPanel openPanel];
  panel.canChooseFiles = YES;
  panel.canChooseDirectories = NO;
  panel.allowsMultipleSelection = NO;
  panel.resolvesAliases = YES;
  return panel;
}

void InstallStreamFullscreenButton(NSWindow *window) {
  if (window == nil) {
    return;
  }
  if (stream_fullscreen_target == nil) {
    stream_fullscreen_target =
        [[CrossDeskStreamFullscreenTarget alloc] init];
  }
  NSButton *zoom_button =
      [window standardWindowButton:NSWindowZoomButton];
  zoom_button.target = stream_fullscreen_target;
  zoom_button.action = @selector(toggleStreamFullscreen:);
}

void SetTitlebarButtonsHidden(NSWindow *window, BOOL hidden) {
  [window standardWindowButton:NSWindowCloseButton].hidden = hidden;
  [window standardWindowButton:NSWindowMiniaturizeButton].hidden = hidden;
  [window standardWindowButton:NSWindowZoomButton].hidden = hidden;
}

void HideMainWindowZoomButton(NSWindow *window) {
  NSButton *button = [window standardWindowButton:NSWindowZoomButton];
  button.target = nil;
  button.action = nil;
  button.enabled = NO;
  button.hidden = YES;
}

void CenterWindowOnScreen(NSWindow *window, NSScreen *screen) {
  if (window == nil || screen == nil) {
    return;
  }
  const NSRect available_frame = screen.visibleFrame;
  const NSRect window_frame = window.frame;
  const NSPoint centered_origin = NSMakePoint(
      NSMidX(available_frame) - NSWidth(window_frame) * 0.5,
      NSMidY(available_frame) - NSHeight(window_frame) * 0.5);
  [window setFrameOrigin:centered_origin];
}

using ConstrainFrameRectImp = NSRect (*)(id, SEL, NSRect, NSScreen *);

struct ConstrainHookEntry {
  __unsafe_unretained Class window_class = Nil;
  ConstrainFrameRectImp original_imp = nullptr;
};

std::vector<ConstrainHookEntry> &ConstrainHooks() {
  static auto *hooks = new std::vector<ConstrainHookEntry>();
  return *hooks;
}

bool IsCustomFullscreenWindow(id window) {
  return (window == stream_window && stream_fullscreen) ||
         (window == gallery_window && gallery_fullscreen);
}

NSRect FullscreenFrameForScreen(NSScreen *screen) {
  if (screen == nil) {
    return NSZeroRect;
  }
  NSRect frame = screen.frame;
  if (@available(macOS 12.0, *)) {
    const CGFloat notch_inset = screen.safeAreaInsets.top;
    if (notch_inset > 0.0 && frame.size.height > notch_inset) {
      // Keep custom non-Space fullscreen below the display notch so the window
      // does not cross into the notch housing or create asymmetric bottom bars.
      frame.size.height -= notch_inset;
    }
  }
  return frame;
}

NSRect CustomFullscreenConstrainFrameRect(id self, SEL cmd, NSRect frame_rect,
                                          NSScreen *screen) {
  if (IsCustomFullscreenWindow(self)) {
    NSScreen *target_screen =
        screen ?: ([self isKindOfClass:NSWindow.class]
                       ? static_cast<NSWindow *>(self).screen
                       : nil);
    if (@available(macOS 12.0, *)) {
      if (target_screen != nil && target_screen.safeAreaInsets.top > 0.0) {
        return FullscreenFrameForScreen(target_screen);
      }
    }
    return frame_rect;
  }
  Class cls = object_getClass(self);
  const auto &hooks = ConstrainHooks();
  while (cls != Nil) {
    for (const auto &entry : hooks) {
      if (entry.window_class == cls && entry.original_imp != nullptr) {
        return entry.original_imp(self, cmd, frame_rect, screen);
      }
    }
    cls = class_getSuperclass(cls);
  }
  return frame_rect;
}

void EnsureUnconstrainedFullscreenHook(NSWindow *window) {
  if (window == nil) {
    return;
  }
  Class window_class = [window class];
  auto &hooks = ConstrainHooks();
  for (const auto &entry : hooks) {
    if (entry.window_class == window_class) {
      return;
    }
  }
  SEL selector = @selector(constrainFrameRect:toScreen:);
  Method method = class_getInstanceMethod(window_class, selector);
  if (method == nullptr) {
    return;
  }
  const char *encoding = method_getTypeEncoding(method);
  ConstrainFrameRectImp original_imp = nullptr;
  if (class_addMethod(window_class, selector,
                      reinterpret_cast<IMP>(CustomFullscreenConstrainFrameRect),
                      encoding)) {
    Class super_class = class_getSuperclass(window_class);
    Method super_method =
        super_class != Nil ? class_getInstanceMethod(super_class, selector)
                           : nullptr;
    if (super_method != nullptr) {
      original_imp = reinterpret_cast<ConstrainFrameRectImp>(
          method_getImplementation(super_method));
    }
  } else {
    original_imp = reinterpret_cast<ConstrainFrameRectImp>(
        method_setImplementation(
            method, reinterpret_cast<IMP>(CustomFullscreenConstrainFrameRect)));
  }
  hooks.push_back({window_class, original_imp});
}

NSApplicationPresentationOptions FullscreenPresentationOptions(
    NSApplicationPresentationOptions base) {
  // HideMenuBar must replace AutoHide* and pair with HideDock to avoid
  // NSInvalidArgumentException and keep secondary displays' menu bars hidden.
  return (base & ~(NSApplicationPresentationAutoHideDock |
                   NSApplicationPresentationAutoHideMenuBar)) |
         NSApplicationPresentationHideDock |
         NSApplicationPresentationHideMenuBar;
}

void ApplyFullscreenPresentation(NSWindow *window) {
  EnsureUnconstrainedFullscreenHook(window);
  if (!fullscreen_presentation_saved) {
    saved_fullscreen_presentation_options = NSApp.presentationOptions;
    fullscreen_presentation_saved = true;
  }
  [NSApp activateIgnoringOtherApps:YES];
  NSApp.presentationOptions =
      FullscreenPresentationOptions(saved_fullscreen_presentation_options);
}

void RestoreFullscreenPresentationIfIdle() {
  if (!stream_fullscreen && !gallery_fullscreen &&
      fullscreen_presentation_saved) {
    NSApp.presentationOptions = saved_fullscreen_presentation_options;
    fullscreen_presentation_saved = false;
  }
}

} // namespace

bool StartNativeWindowDrag() {
  @autoreleasepool {
    NSEvent *event = [[NSApplication sharedApplication] currentEvent];
    if (event == nil || event.type != NSEventTypeLeftMouseDown ||
        event.window == nil) {
      return false;
    }

    [event.window performWindowDragWithEvent:event];
    return true;
  }
}

float MainWindowInitialPositionYOffset() {
  @autoreleasepool {
    const NSRect content = NSMakeRect(0, 0, 640, 486);
    const NSRect frame = [NSWindow frameRectForContentRect:content
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                  NSWindowStyleMaskMiniaturizable];
    return float(NSMaxY(frame) - NSMaxY(content));
  }
}

bool CenterNativeWindowOnScreen(void *opaque_slint_view) {
  @autoreleasepool {
    NSView *slint_view = (__bridge NSView *)opaque_slint_view;
    NSWindow *window = slint_view.window;
    NSScreen *screen = window.screen ?: NSScreen.mainScreen;
    if (!window || !screen) return false;
    CenterWindowOnScreen(window, screen);
    return true;
  }
}

bool ConfigureWhiteWindowChrome(void *opaque_slint_view) {
  @autoreleasepool {
    NSView *slint_view = (__bridge NSView *)opaque_slint_view;
    NSWindow *window = slint_view.window;
    if (window == nil ||
        (window.styleMask & NSWindowStyleMaskTitled) == 0) {
      return false;
    }

    // Winit creates a transparent AppKit backing even when Slint paints an
    // opaque client area. macOS 27 can expose that backing in the titlebar.
    // Use a fixed white backing instead of AppKit's theme-dependent titlebar
    // material. Aqua keeps the native title and controls readable on white.
    window.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    window.titlebarAppearsTransparent = YES;
    window.opaque = YES;
    window.backgroundColor = NSColor.whiteColor;
    NSView *frame_view = window.contentView.superview;
    frame_view.needsLayout = YES;
    [frame_view layoutSubtreeIfNeeded];
    return true;
  }
}

bool ConfigureMainWindowChrome(void *opaque_slint_view, bool gallery) {
  @autoreleasepool {
    NSView *slint_view = (__bridge NSView *)opaque_slint_view;
    NSWindow *window = slint_view.window;
    if (window == nil ||
        (window.styleMask & NSWindowStyleMaskTitled) == 0) {
      return false;
    }

    NSButton *zoom_button =
        [window standardWindowButton:NSWindowZoomButton];
    if (zoom_button == nil) return false;
    // Keep the connection/DeskHub and remote-control windows independently
    // switchable even when the user prefers opening documents in tabs.
    window.tabbingMode = NSWindowTabbingModeDisallowed;
    window.excludedFromWindowsMenu = NO;
    if (gallery) gallery_window = window;
    if (gallery && gallery_fullscreen) return true;
    if (gallery)
      window.styleMask |= NSWindowStyleMaskResizable;
    else
      window.styleMask &= ~NSWindowStyleMaskResizable;
    window.collectionBehavior =
        (window.collectionBehavior & ~(NSWindowCollectionBehaviorFullScreenPrimary |
                                       NSWindowCollectionBehaviorFullScreenAuxiliary |
                                       NSWindowCollectionBehaviorFullScreenAllowsTiling)) |
        NSWindowCollectionBehaviorFullScreenNone;

    HideMainWindowZoomButton(window);
    return ConfigureWhiteWindowChrome(opaque_slint_view);
  }
}

bool SetGalleryWindowFullscreen(bool fullscreen) {
  @autoreleasepool {
    if (!gallery_window) return false;
    if (gallery_fullscreen == fullscreen) return true;
    if (fullscreen) {
      NSScreen *screen = gallery_window.screen ?: NSScreen.mainScreen;
      if (!screen) return false;
      gallery_restore_frame = gallery_window.frame;
      gallery_restore_style = gallery_window.styleMask;
      gallery_fullscreen = true;
      gallery_window.styleMask |= NSWindowStyleMaskFullSizeContentView;
      gallery_window.titleVisibility = NSWindowTitleHidden;
      gallery_window.movable = NO;
      SetTitlebarButtonsHidden(gallery_window, YES);
      ApplyFullscreenPresentation(gallery_window);
      // Let the next event-loop frame paint the updated Slint layout. A
      // synchronous draw here exposes stale grid geometry during the resize.
      [gallery_window setFrame:FullscreenFrameForScreen(screen)
                         display:NO
                         animate:NO];
      [gallery_window makeKeyAndOrderFront:nil];
    } else {
      gallery_fullscreen = false;
      RestoreFullscreenPresentationIfIdle();
      gallery_window.styleMask = gallery_restore_style;
      gallery_window.titleVisibility = NSWindowTitleVisible;
      gallery_window.movable = YES;
      [gallery_window standardWindowButton:NSWindowCloseButton].hidden = NO;
      [gallery_window standardWindowButton:NSWindowMiniaturizeButton].hidden = NO;
      // DeskHub uses its in-app fullscreen action.
      HideMainWindowZoomButton(gallery_window);
      [gallery_window setFrame:gallery_restore_frame display:NO animate:NO];
    }
    gallery_window.contentView.needsDisplay = YES;
    return true;
  }
}
bool IsGalleryWindowFullscreen() { return gallery_fullscreen; }
bool IsGalleryWindowActive() { return gallery_window && [NSApp isActive] && [gallery_window isKeyWindow]; }

bool MaximizeGalleryWindowWithoutAnimation() {
  @autoreleasepool {
    if (!gallery_window || gallery_fullscreen) return false;
    NSScreen *screen = gallery_window.screen ?: NSScreen.mainScreen;
    if (!screen) return false;
    [gallery_window setFrame:screen.visibleFrame display:YES animate:NO];
    return true;
  }
}

bool IsGalleryWindowMaximized() {
  @autoreleasepool {
    if (!gallery_window || gallery_fullscreen) return false;
    NSScreen *screen = gallery_window.screen;
    if (!screen) return false;
    // isZoomed can report true for a constrained compact window. Only treat
    // the ordinary window as maximized when it fills the usable screen.
    const NSRect frame = gallery_window.frame;
    const NSRect available = screen.visibleFrame;
    return std::abs(frame.origin.x - available.origin.x) < 1 &&
           std::abs(frame.origin.y - available.origin.y) < 1 &&
           std::abs(frame.size.width - available.size.width) < 1 &&
           std::abs(frame.size.height - available.size.height) < 1;
  }
}

bool ConfigureStreamWindowLiveResize(void *opaque_slint_view) {
  @autoreleasepool {
    NSView *slint_view = (__bridge NSView *)opaque_slint_view;
    NSWindow *window = slint_view.window;
    if (window == nil) {
      return false;
    }

    NSButton *zoom_button =
        [window standardWindowButton:NSWindowZoomButton];
    NSView *content_view = window.contentView;
    if (zoom_button == nil || !zoom_button.enabled || content_view == nil) {
      return false;
    }

    // During an ordinary edge resize, scale one cached frame and redraw once
    // the new window size has settled.
    window.preservesContentDuringLiveResize = YES;
    content_view.layerContentsRedrawPolicy =
        NSViewLayerContentsRedrawBeforeViewResize;
    content_view.layerContentsPlacement =
        NSViewLayerContentsPlacementScaleProportionallyToFit;
    stream_window = window;
    stream_slint_view = slint_view;
    window.tabbingMode = NSWindowTabbingModeDisallowed;
    window.excludedFromWindowsMenu = NO;
    // Native Space fullscreen cross-fades an AppKit source snapshot over a
    // separately rendered destination window. Disable that path for the
    // stream window and route the green button to the immediate transition.
    window.collectionBehavior =
        (window.collectionBehavior &
         ~(NSWindowCollectionBehaviorFullScreenPrimary |
           NSWindowCollectionBehaviorFullScreenAuxiliary |
           NSWindowCollectionBehaviorFullScreenAllowsTiling)) |
        NSWindowCollectionBehaviorFullScreenNone;
    InstallStreamFullscreenButton(window);
    content_view.needsDisplay = YES;
    return true;
  }
}

void UnregisterStreamWindow(void *opaque_slint_view) {
  @autoreleasepool {
    NSView *slint_view = (__bridge NSView *)opaque_slint_view;
    if (slint_view == nil || slint_view != stream_slint_view) {
      return;
    }
    stream_window = nil;
    stream_slint_view = nil;
    stream_fullscreen = false;
    stream_restore_frame = NSZeroRect;
    stream_restore_style_mask = NSWindowStyleMaskTitled;
    stream_restore_title_visibility = NSWindowTitleVisible;
    stream_restore_titlebar_transparent = NO;
    stream_restore_movable = YES;
    stream_restore_close_hidden = NO;
    stream_restore_miniaturize_hidden = NO;
    stream_restore_zoom_hidden = NO;
    RestoreFullscreenPresentationIfIdle();
  }
}

bool IsStreamWindowActive() {
  @autoreleasepool {
    return stream_window != nil && [NSApp isActive] &&
           [stream_window isKeyWindow];
  }
}

bool SetStreamWindowFullscreen(bool fullscreen) {
  @autoreleasepool {
    if (stream_window == nil || stream_fullscreen == fullscreen) {
      return stream_window != nil;
    }

    if (fullscreen) {
      NSScreen *screen = stream_window.screen ?: NSScreen.mainScreen;
      if (screen == nil) {
        return false;
      }

      stream_restore_frame = stream_window.frame;
      stream_restore_style_mask = stream_window.styleMask;
      stream_restore_title_visibility = stream_window.titleVisibility;
      stream_restore_titlebar_transparent =
          stream_window.titlebarAppearsTransparent;
      stream_restore_movable = stream_window.movable;
      stream_restore_close_hidden =
          [stream_window standardWindowButton:NSWindowCloseButton].hidden;
      stream_restore_miniaturize_hidden =
          [stream_window standardWindowButton:NSWindowMiniaturizeButton].hidden;
      stream_restore_zoom_hidden =
          [stream_window standardWindowButton:NSWindowZoomButton].hidden;

      stream_fullscreen = true;
      stream_window.styleMask =
          stream_restore_style_mask | NSWindowStyleMaskFullSizeContentView;
      stream_window.titleVisibility = NSWindowTitleHidden;
      stream_window.titlebarAppearsTransparent = YES;
      stream_window.movable = NO;
      SetTitlebarButtonsHidden(stream_window, YES);

      ApplyFullscreenPresentation(stream_window);
      [stream_window setFrame:FullscreenFrameForScreen(screen)
                        display:YES
                        animate:NO];
      [stream_window makeKeyAndOrderFront:nil];
    } else {
      stream_fullscreen = false;
      RestoreFullscreenPresentationIfIdle();
      stream_window.styleMask = stream_restore_style_mask;
      stream_window.titleVisibility = stream_restore_title_visibility;
      stream_window.titlebarAppearsTransparent =
          stream_restore_titlebar_transparent;
      stream_window.movable = stream_restore_movable;
      [stream_window setFrame:stream_restore_frame display:YES animate:NO];
      [stream_window standardWindowButton:NSWindowCloseButton].hidden =
          stream_restore_close_hidden;
      [stream_window standardWindowButton:NSWindowMiniaturizeButton].hidden =
          stream_restore_miniaturize_hidden;
      [stream_window standardWindowButton:NSWindowZoomButton].hidden =
          stream_restore_zoom_hidden;
      InstallStreamFullscreenButton(stream_window);
      [stream_window makeKeyAndOrderFront:nil];
    }

    stream_window.contentView.alphaValue = 1.0;
    stream_window.contentView.needsDisplay = YES;
    [stream_window displayIfNeeded];
    return true;
  }
}

bool IsStreamWindowFullscreen() {
  return stream_fullscreen;
}

void ShowFileTransferNotification(void* slint_view,
                                  const std::function<void()>& show) {
  @autoreleasepool {
    NSWindow *previous_key = NSApp.keyWindow;
    NSView *view = (__bridge NSView *)slint_view;
    NSWindow *notification = view.window;
    notification.excludedFromWindowsMenu = YES;
    notification.hidesOnDeactivate = NO;
    notification.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorFullScreenAuxiliary |
        NSWindowCollectionBehaviorIgnoresCycle;
    show();
    // Winit's show path calls makeKeyAndOrderFront even for frameless windows.
    // Restore the input owner synchronously before the next event-loop turn.
    if (notification.isKeyWindow) {
      if (previous_key && previous_key != notification) [previous_key makeKeyWindow];
      else [notification resignKeyWindow];
    }
  }
}

void PrepareNativeFileDialog() {
  const auto generation = ++file_dialog_prepare_generation;
  // Creating NSOpenPanel initializes the system file browser and can take
  // hundreds of milliseconds. Do that once after the main window is visible,
  // without showing a panel, changing focus, or blocking the initial frame.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
      dispatch_get_main_queue(), ^{
        if (generation != file_dialog_prepare_generation ||
            file_dialog_request || prepared_file_dialog) return;
        NSOpenPanel *panel = CreateFileDialog();
        if (generation == file_dialog_prepare_generation &&
            !file_dialog_request && !prepared_file_dialog)
          prepared_file_dialog = panel;
      });
}

void OpenNativeFileDialog(const std::string& title,
                          std::function<void(std::string)> completion) {
  if (file_dialog_request) {
    completion({});
    return;
  }
  const auto request = std::make_shared<FileDialogRequest>(
      FileDialogRequest{title, std::move(completion)});
  file_dialog_request = request;
  // Leave the current Slint callback before AppKit creates or shows the panel.
  // runModal would let display-link callbacks reenter Slint's timer dispatch.
  dispatch_async(dispatch_get_main_queue(), ^{
    if (file_dialog_request != request) return;
    NSOpenPanel *panel = prepared_file_dialog ?: CreateFileDialog();
    prepared_file_dialog = nil;
    file_dialog = panel;

    NSString *prompt =
        [[NSString alloc] initWithBytes:request->title.data()
                                 length:request->title.size()
                               encoding:NSUTF8StringEncoding];
    panel.title = prompt ?: @"";
    panel.message = prompt ?: @"";

    // tinyfiledialogs uses an external osascript process on macOS, so its
    // chooser can appear without becoming the active window. Keep the picker
    // inside CrossDesk and explicitly activate both the app and panel.
    [NSApp activateIgnoringOtherApps:YES];
    NSWindow *owner = NSApp.keyWindow ?: NSApp.mainWindow;
    if (owner != nil) {
      [owner makeKeyAndOrderFront:nil];
    }
    NSScreen *target_screen = owner.screen ?: NSScreen.mainScreen;
    CenterWindowOnScreen(panel, target_screen);
    __weak NSOpenPanel *weak_panel = panel;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
      // Cancellation can complete a request before AppKit calls this block.
      if (file_dialog_request != request) return;
      NSOpenPanel *finished_panel = weak_panel;
      const char *selected = result == NSModalResponseOK
          ? finished_panel.URL.fileSystemRepresentation : nullptr;
      const std::string path = selected ? selected : "";
      [finished_panel orderOut:nil];
      file_dialog = nil;
      // AppKit has returned the panel to its configuration phase. Keep the
      // browser alive for the next selection instead of rebuilding it.
      prepared_file_dialog = finished_panel;
      file_dialog_request.reset();
      auto finished = std::move(request->completion);
      finished(path);
    }];
    if (file_dialog_request == request) [panel makeKeyAndOrderFront:nil];
  });
}

void SaveNativeFileDialog(const std::string& title, const std::string& filename,
                          std::function<void(std::string)> completion) {
  if (file_dialog_request) { completion({}); return; }
  const auto request = std::make_shared<FileDialogRequest>(
      FileDialogRequest{title, std::move(completion), filename});
  file_dialog_request = request;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (file_dialog_request != request) return;
    NSSavePanel *panel = [NSSavePanel savePanel];
    file_dialog = panel;
    panel.title = [NSString stringWithUTF8String:request->title.c_str()];
    panel.nameFieldStringValue = [NSString stringWithUTF8String:request->filename.c_str()];
    panel.allowedContentTypes = @[UTTypeZIP];
    panel.allowsOtherFileTypes = NO;
    panel.canCreateDirectories = YES;
    [NSApp activateIgnoringOtherApps:YES];
    NSWindow *owner = NSApp.keyWindow ?: NSApp.mainWindow;
    CenterWindowOnScreen(panel, owner.screen ?: NSScreen.mainScreen);
    __weak NSSavePanel *weak_panel = panel;
    [panel beginWithCompletionHandler:^(NSModalResponse result) {
      if (file_dialog_request != request) return;
      NSSavePanel *finished_panel = weak_panel;
      const char *selected = result == NSModalResponseOK
          ? finished_panel.URL.fileSystemRepresentation : nullptr;
      const std::string path = selected ? selected : "";
      [finished_panel orderOut:nil];
      file_dialog = nil;
      file_dialog_request.reset();
      auto finished = std::move(request->completion);
      finished(path);
    }];
    if (file_dialog_request == request) [panel makeKeyAndOrderFront:nil];
  });
}

void CancelNativeFileDialog() {
  ++file_dialog_prepare_generation;
  prepared_file_dialog = nil;
  const auto request = std::move(file_dialog_request);
  if (!request) return;
  NSSavePanel *panel = file_dialog;
  file_dialog = nil;
  [panel cancel:nil];
  [panel orderOut:nil];
  auto finished = std::move(request->completion);
  finished({});
}

} // namespace crossdesk
