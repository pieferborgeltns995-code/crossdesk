#include "platform/window_drag.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace crossdesk {
namespace {

__weak NSWindow *gallery_window = nil;
bool gallery_fullscreen = false;
NSRect gallery_restore_frame = NSZeroRect;
NSWindowStyleMask gallery_restore_style = NSWindowStyleMaskTitled;
NSApplicationPresentationOptions gallery_restore_presentation;
__weak NSWindow *stream_window = nil;
__weak NSView *stream_slint_view = nil;
bool stream_fullscreen = false;
id stream_will_enter_fs_observer = nil;
id stream_did_enter_fs_observer = nil;
id stream_will_exit_fs_observer = nil;
id stream_did_exit_fs_observer = nil;

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

void RemoveStreamFullscreenObservers() {
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
  if (stream_will_enter_fs_observer != nil) {
    [center removeObserver:stream_will_enter_fs_observer];
    stream_will_enter_fs_observer = nil;
  }
  if (stream_did_enter_fs_observer != nil) {
    [center removeObserver:stream_did_enter_fs_observer];
    stream_did_enter_fs_observer = nil;
  }
  if (stream_will_exit_fs_observer != nil) {
    [center removeObserver:stream_will_exit_fs_observer];
    stream_will_exit_fs_observer = nil;
  }
  if (stream_did_exit_fs_observer != nil) {
    [center removeObserver:stream_did_exit_fs_observer];
    stream_did_exit_fs_observer = nil;
  }
}

void InstallStreamFullscreenObservers(NSWindow *window) {
  RemoveStreamFullscreenObservers();
  if (window == nil) {
    return;
  }
  NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
  NSOperationQueue *main_queue = [NSOperationQueue mainQueue];
  stream_will_enter_fs_observer = [center
      addObserverForName:NSWindowWillEnterFullScreenNotification
                  object:window
                   queue:main_queue
              usingBlock:^(NSNotification *note) {
                (void)note;
                stream_fullscreen = true;
              }];
  stream_did_enter_fs_observer = [center
      addObserverForName:NSWindowDidEnterFullScreenNotification
                  object:window
                   queue:main_queue
              usingBlock:^(NSNotification *note) {
                (void)note;
                stream_fullscreen = true;
              }];
  stream_will_exit_fs_observer = [center
      addObserverForName:NSWindowWillExitFullScreenNotification
                  object:window
                   queue:main_queue
              usingBlock:^(NSNotification *note) {
                (void)note;
                stream_fullscreen = false;
              }];
  stream_did_exit_fs_observer = [center
      addObserverForName:NSWindowDidExitFullScreenNotification
                  object:window
                   queue:main_queue
              usingBlock:^(NSNotification *note) {
                (void)note;
                stream_fullscreen = false;
              }];
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
      gallery_restore_presentation = NSApp.presentationOptions;
      gallery_fullscreen = true;
      gallery_window.styleMask |= NSWindowStyleMaskFullSizeContentView;
      gallery_window.titleVisibility = NSWindowTitleHidden;
      gallery_window.movable = NO;
      SetTitlebarButtonsHidden(gallery_window, YES);
      NSApp.presentationOptions |=
          NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar;
      // Let the next event-loop frame paint the updated Slint layout. A
      // synchronous draw here exposes stale grid geometry during the resize.
      [gallery_window setFrame:screen.frame display:NO animate:NO];
    } else {
      gallery_fullscreen = false;
      NSApp.presentationOptions = gallery_restore_presentation;
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
    // Use native macOS Space fullscreen for the stream window so the system
    // menu bar / notch area turns black and Space gestures work natively.
    window.collectionBehavior =
        (window.collectionBehavior &
         ~(NSWindowCollectionBehaviorFullScreenNone |
           NSWindowCollectionBehaviorFullScreenAuxiliary)) |
        NSWindowCollectionBehaviorFullScreenPrimary;
    stream_fullscreen =
        (window.styleMask & NSWindowStyleMaskFullScreen) != 0;
    InstallStreamFullscreenObservers(window);
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
    RemoveStreamFullscreenObservers();
    stream_window = nil;
    stream_slint_view = nil;
    stream_fullscreen = false;
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
    if (stream_window == nil) {
      return false;
    }
    if (stream_fullscreen == fullscreen) {
      return true;
    }
    [stream_window toggleFullScreen:nil];
    return true;
  }
}

bool IsStreamWindowFullscreen() {
  @autoreleasepool {
    if (stream_window == nil) {
      return false;
    }
    return stream_fullscreen;
  }
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
