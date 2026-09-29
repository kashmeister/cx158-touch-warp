// cx158-touch-warp: make touches on the CX158 portable monitor land on the CX158.
//
// macOS drives the monitor's touch controller (USB 0416:c169) as an absolute-position mouse,
// but scales its coordinates onto whichever display the cursor is on. With the cursor on
// another display, a touch clicks there. The controller sends press and position in the same
// first report, so the press is aimed at the wrong display before user space can react.
//
// Fix: an event tap on mouse button events swallows each touchscreen press and re-posts it,
// with its drags and release, as synthetic events at the matching point on the CX158. (Editing
// the location of a hardware event is not honoured for absolute devices.) A press counts as a
// touch only if it is a hardware event (source pid 0), not from a trackpad (subtype 0), and the
// touch controller reported that button pressed within the last 60 ms. A touchscreen and a USB
// mouse produce identical CGEvents, so without reading the controller nothing is remapped.
//
// Fallback without Accessibility: on the first report of a touch the cursor is warped onto the
// CX158. That corrects the rest of the touch, not the initial click.
//
// The device is never seized; if the helper stops, touch behaves as it does natively.
// Permissions: Input Monitoring (read the controller) and Accessibility (event tap).
//
//   --verbose   log every touch report and every remapped event
//   --check     print displays, device and permission status, then exit

#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hidsystem/IOHIDLib.h>      // IOHIDCheckAccess / IOHIDRequestAccess
#include <dispatch/dispatch.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- configuration

enum { TOUCH_VID = 0x0416, TOUCH_PID = 0xC169 };   // touch controller in absolute-mouse mode
enum { DIGI_VID = 0xEEEF, DIGI_PID = 0x2828 };     // same controller in digitizer mode (no macOS driver)
static const uint32_t DISPLAY_VENDOR = 0x0614;     // CX158 (CoreGraphics vendor / model numbers)
static const uint32_t DISPLAY_MODEL  = 0x4778;

static const double CORRELATION_WINDOW = 0.06; // s; max age of the touch report behind a press
static const double OWNED_TIMEOUT      = 0.5;  // s; drop a takeover if touch reports stop this long
static const double ONSET_IDLE_GAP     = 0.15; // s; (warp fallback) gap that starts a new touch
static const double GUARD_WINDOW       = 0.30; // s; (warp fallback) correction time after a touch starts
static const double PERMISSION_RETRY   = 10;   // s; Accessibility re-check interval
static const double RELAUNCH_DELAY     = 30;   // s; relaunch to pick up an Input Monitoring grant
static const double RELAUNCH_COOLDOWN  = 600;  // s; at most one such relaunch per this period

static bool verbose = false;

// ---------------------------------------------------------------- utilities

static void logmsg(const char *fmt, ...) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm; localtime_r(&tv.tv_sec, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    printf("%s.%03d ", ts, (int)(tv.tv_usec / 1000));
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n'); fflush(stdout);
}

static double now_s(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }

// Notification Center banner via osascript (a bare executable cannot use the notification
// framework). Text is passed as arguments, not spliced into the script. The child is not waited
// for, so the run loop serving the event tap never blocks; SIGCHLD is ignored so it is reaped.
static void notify(const char *title, const char *body) {
    char *argv[] = { "/usr/bin/osascript",
                     "-e", "on run argv",
                     "-e", "display notification (item 2 of argv) with title (item 1 of argv)",
                     "-e", "end run",
                     (char *)title, (char *)body, NULL };
    extern char **environ;
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0) logmsg("notification failed: %s", body);
}

static CFRunLoopTimerRef schedule(CFRunLoopTimerRef existing, double delay, double interval, CFRunLoopTimerCallBack cb) {
    if (existing) return existing;
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + delay, interval, 0, 0, cb, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopDefaultMode);
    return t;
}

static void cancel(CFRunLoopTimerRef *t) {
    if (*t) { CFRunLoopTimerInvalidate(*t); CFRelease(*t); *t = NULL; }
}

static CFMutableDictionaryRef usb_matching(int vid, int pid) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid), p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(props, CFSTR("idVendor"), v);
    CFDictionarySetValue(props, CFSTR("idProduct"), p);
    CFDictionarySetValue(m, CFSTR(kIOPropertyMatchKey), props);
    CFRelease(props); CFRelease(v); CFRelease(p);
    return m;
}

// ---------------------------------------------------------------- display tracking

static bool have_display = false;
static CGRect display_bounds;

static bool is_target(CGDirectDisplayID id) {
    return CGDisplayVendorNumber(id) == DISPLAY_VENDOR && CGDisplayModelNumber(id) == DISPLAY_MODEL;
}

static void refresh_display(void) {
    CGDirectDisplayID ids[16]; uint32_t count = 0;
    if (CGGetOnlineDisplayList(16, ids, &count) != kCGErrorSuccess) return;
    bool found = false; CGRect b = CGRectZero; CGDirectDisplayID id = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (is_target(ids[i])) { found = true; id = ids[i]; b = CGDisplayBounds(ids[i]); break; }
    }
    static bool reported = false;   // always log the first state
    if (reported && found == have_display && (!found || CGRectEqualToRect(b, display_bounds))) return;
    reported = true; have_display = found; display_bounds = b;
    if (found) logmsg("CX158 display %u: origin (%d,%d) size %dx%d pt", id,
                      (int)b.origin.x, (int)b.origin.y, (int)b.size.width, (int)b.size.height);
    else       logmsg("CX158 display not connected; idle until it appears");
}

static void display_cb(CGDirectDisplayID d, CGDisplayChangeSummaryFlags flags, void *ctx) {
    (void)d; (void)ctx;
    if (flags & kCGDisplayBeginConfigurationFlag) return;
    refresh_display();
}

// ---------------------------------------------------------------- touch reports

static double x_max = 1920, y_max = 1920;   // logical ranges; read from the device on attach
static double last_report = 0;              // arrival time of the latest report
static uint8_t last_buttons = 0;            // its button bits
static uint16_t last_x = 0, last_y = 0;     // its position
static bool was_touching = false;
static double guard_until = 0;
static unsigned warp_count = 0, remap_count = 0;
static CFMachPortRef tap = NULL;            // non-NULL once the event tap is active

static CGPoint map_to_display(double x, double y) {
    return CGPointMake(display_bounds.origin.x + x / x_max * display_bounds.size.width,
                       display_bounds.origin.y + y / y_max * display_bounds.size.height);
}

static void handle_report(uint8_t buttons, uint16_t x, uint16_t y) {
    double now = now_s();
    bool touching = buttons & 0x01;
    bool onset = (now - last_report > ONSET_IDLE_GAP) || (touching && !was_touching);
    last_report = now; last_buttons = buttons; last_x = x; last_y = y; was_touching = touching;
    if (onset) guard_until = now + GUARD_WINDOW;

    char action[96] = "";
    // Warp fallback, only while the event tap is unavailable.
    if (!tap && have_display && now <= guard_until) {
        CGEventRef ev = CGEventCreate(NULL);
        if (ev) {
            CGPoint cur = CGEventGetLocation(ev); CFRelease(ev);
            if (!CGRectContainsPoint(display_bounds, cur)) {
                CGPoint target = map_to_display(x, y);
                CGError err = CGWarpMouseCursorPosition(target);
                CGAssociateMouseAndMouseCursorPosition(true);
                if (err == kCGErrorSuccess) {
                    warp_count++;
                    snprintf(action, sizeof action, " WARP (%d,%d) -> (%d,%d)", (int)cur.x, (int)cur.y, (int)target.x, (int)target.y);
                } else {
                    snprintf(action, sizeof action, " warp failed: CGError %d", (int)err);
                }
            }
        }
    }
    if (verbose) logmsg("report btn=%u x=%u y=%u%s%s", buttons, x, y, onset ? " onset" : "", action);
}

// ---------------------------------------------------------------- event tap

static bool manager_open = false;           // true once the touch controller can be read

static const CGEventType   up_type[3]   = { kCGEventLeftMouseUp, kCGEventRightMouseUp, kCGEventOtherMouseUp };
static const CGMouseButton button_of[3] = { kCGMouseButtonLeft, kCGMouseButtonRight, kCGMouseButtonCenter };

// Per button: set while a touch press is replaced with synthetic events; `at` is the last
// point posted, so an abandoned press can be released where it was held.
static struct { bool active; CGPoint at; } owned[3];

static const char *type_name(CGEventType t) {
    switch (t) {
    case kCGEventLeftMouseDown:     return "LDown";
    case kCGEventLeftMouseUp:       return "LUp";
    case kCGEventLeftMouseDragged:  return "LDrag";
    case kCGEventRightMouseDown:    return "RDown";
    case kCGEventRightMouseUp:      return "RUp";
    case kCGEventRightMouseDragged: return "RDrag";
    case kCGEventOtherMouseDown:    return "ODown";
    case kCGEventOtherMouseUp:      return "OUp";
    case kCGEventOtherMouseDragged: return "ODrag";
    default:                        return "?";
    }
}

static int button_slot(CGEventType t, bool *down, bool *up) {
    *down = t == kCGEventLeftMouseDown || t == kCGEventRightMouseDown || t == kCGEventOtherMouseDown;
    *up   = t == kCGEventLeftMouseUp   || t == kCGEventRightMouseUp   || t == kCGEventOtherMouseUp;
    switch (t) {
    case kCGEventLeftMouseDown:  case kCGEventLeftMouseUp:  case kCGEventLeftMouseDragged:  return 0;
    case kCGEventRightMouseDown: case kCGEventRightMouseUp: case kCGEventRightMouseDragged: return 1;
    case kCGEventOtherMouseDown: case kCGEventOtherMouseUp: case kCGEventOtherMouseDragged: return 2;
    default: return -1;
    }
}

static void post_mouse(CGEventType type, CGPoint at, CGMouseButton btn, CGEventRef like) {
    CGEventRef e = CGEventCreateMouseEvent(NULL, type, at, btn);
    if (!e) return;
    if (like) {
        CGEventSetIntegerValueField(e, kCGMouseEventClickState, CGEventGetIntegerValueField(like, kCGMouseEventClickState));
        CGEventSetDoubleValueField(e, kCGMouseEventPressure, CGEventGetDoubleValueField(like, kCGMouseEventPressure));
        CGEventSetFlags(e, CGEventGetFlags(like));
    }
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

// Ends a takeover whose mouse-up will not arrive, so the synthetic press is not left held and
// later events from other devices are not captured.
static void release_owned(int slot, const char *why) {
    if (!owned[slot].active) return;
    owned[slot].active = false;
    post_mouse(up_type[slot], owned[slot].at, button_of[slot], NULL);
    logmsg("released an abandoned touch press (%s)", why);
}

static CGEventRef tap_cb(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *ctx) {
    (void)proxy; (void)ctx;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(tap, true);
        logmsg("event tap was disabled (%s); re-enabled", type == kCGEventTapDisabledByTimeout ? "timeout" : "user input");
        return event;
    }
    // Synthetic events (including those posted here) carry the posting pid; trackpad events carry subtype 3.
    if (CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) != 0) return event;
    if (CGEventGetIntegerValueField(event, kCGMouseEventSubtype) != kCGEventMouseSubtypeDefault) return event;
    bool is_down, is_up;
    int slot = button_slot(type, &is_down, &is_up);
    if (slot < 0) return event;
    if (slot == 2 && CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) != kCGMouseButtonCenter) return event;

    double age = now_s() - last_report;
    if (owned[slot].active && is_down)             release_owned(slot, "new press");
    if (owned[slot].active && age > OWNED_TIMEOUT) release_owned(slot, "touch reports stopped");

    CGPoint loc = CGEventGetLocation(event);
    if (!owned[slot].active) {
        // Only a press the touch controller is reporting right now starts a takeover.
        if (!is_down || !manager_open || !have_display) return event;
        if (age > CORRELATION_WINDOW || !(last_buttons & (1u << slot))) {
            if (verbose) logmsg("tap: %s at (%d,%d) is not a touch; left alone", type_name(type), (int)loc.x, (int)loc.y);
            return event;
        }
        if (CGRectContainsPoint(display_bounds, loc)) return event;   // already on the CX158
        owned[slot].active = true;
    }

    CGPoint target = map_to_display(last_x, last_y);
    owned[slot].at = target;
    if (is_up) owned[slot].active = false;
    post_mouse(type, target, button_of[slot], event);
    remap_count++;
    if (verbose) logmsg("tap: %s (%d,%d) => posted at (%d,%d), report age %.1f ms",
                        type_name(type), (int)loc.x, (int)loc.y, (int)target.x, (int)target.y, age * 1000);
    return NULL;   // swallow the hardware event
}

static CFRunLoopTimerRef ax_timer = NULL;
static bool ax_requested = false, ax_warned = false;

static void try_tap(CFRunLoopTimerRef t, void *ctx) {
    (void)t; (void)ctx;
    if (tap) return;
    if (!AXIsProcessTrusted()) {
        if (!ax_requested) {
            ax_requested = true;
            const void *k = kAXTrustedCheckOptionPrompt, *v = kCFBooleanTrue;
            CFDictionaryRef opts = CFDictionaryCreate(NULL, &k, &v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            AXIsProcessTrustedWithOptions(opts);   // adds the binary to the list and shows the prompt
            CFRelease(opts);
        }
        if (!ax_warned) {
            logmsg("Accessibility permission missing: enable \"cx158-touch-warp\" in System Settings > Privacy & Security > "
                   "Accessibility. Until then only the cursor-warp fallback runs. Re-checking every %d s.", (int)PERMISSION_RETRY);
            notify("cx158-touch-warp needs Accessibility",
                   "Enable cx158-touch-warp under System Settings > Privacy & Security > Accessibility.");
            ax_warned = true;
        }
        ax_timer = schedule(ax_timer, PERMISSION_RETRY, PERMISSION_RETRY, try_tap);
        return;
    }
    CGEventMask mask = CGEventMaskBit(kCGEventLeftMouseDown)  | CGEventMaskBit(kCGEventLeftMouseUp)  | CGEventMaskBit(kCGEventLeftMouseDragged)
                     | CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventRightMouseDragged)
                     | CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) | CGEventMaskBit(kCGEventOtherMouseDragged);
    tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, tap_cb, NULL);
    if (!tap) {
        logmsg("CGEventTapCreate failed although Accessibility looks granted; retrying in %d s", (int)PERMISSION_RETRY);
        ax_timer = schedule(ax_timer, PERMISSION_RETRY, PERMISSION_RETRY, try_tap);
        return;
    }
    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(NULL, tap, 0);
    CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopDefaultMode);
    CFRelease(src);
    CGEventTapEnable(tap, true);
    cancel(&ax_timer);
    logmsg("Accessibility granted; event tap active%s",
           manager_open ? "" : " (idle: nothing is remapped until Input Monitoring is granted)");
}

// ---------------------------------------------------------------- touch controller (HID)

static IOHIDManagerRef manager;
static int touch_devices = 0;

#define MAX_DEVICES 4
static struct { IOHIDDeviceRef dev; uint8_t buf[64]; } armed[MAX_DEVICES];   // fixed slots: buf addresses must not move

static long prop_int(IOHIDDeviceRef d, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(d, key); long n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue(v, kCFNumberLongType, &n);
    return n;
}

static void describe(IOHIDDeviceRef d, char *out, size_t n) {
    char name[64] = "?";
    CFTypeRef v = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductKey));
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) CFStringGetCString(v, name, sizeof name, kCFStringEncodingUTF8);
    snprintf(out, n, "%s %04lx:%04lx", name, prop_int(d, CFSTR(kIOHIDVendorIDKey)), prop_int(d, CFSTR(kIOHIDProductIDKey)));
}

static void read_axis_ranges(IOHIDDeviceRef d) {
    CFArrayRef els = IOHIDDeviceCopyMatchingElements(d, NULL, kIOHIDOptionsTypeNone);
    if (!els) return;
    for (CFIndex i = 0; i < CFArrayGetCount(els); i++) {
        IOHIDElementRef e = (IOHIDElementRef)CFArrayGetValueAtIndex(els, i);
        if (IOHIDElementGetUsagePage(e) != kHIDPage_GenericDesktop) continue;
        CFIndex max = IOHIDElementGetLogicalMax(e);
        if (max <= 0) continue;
        if (IOHIDElementGetUsage(e) == kHIDUsage_GD_X) x_max = (double)max;
        else if (IOHIDElementGetUsage(e) == kHIDUsage_GD_Y) y_max = (double)max;
    }
    CFRelease(els);
}

// Report ID 2: [id] [buttons] [x lo] [x hi] [y lo] [y hi] [wheel] [pan]  (8 bytes incl. the ID byte)
static void report_cb(void *ctx, IOReturn result, void *sender, IOHIDReportType type,
                      uint32_t report_id, uint8_t *report, CFIndex length) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || report_id != 2 || length < 7) return;
    int off = length >= 8 ? 1 : 0;
    handle_report(report[off],
                  (uint16_t)(report[off + 1] | report[off + 2] << 8),
                  (uint16_t)(report[off + 3] | report[off + 4] << 8));
}

static int armed_index(IOHIDDeviceRef d) {
    for (int i = 0; i < MAX_DEVICES; i++) if (armed[i].dev && CFEqual(armed[i].dev, d)) return i;
    return -1;
}

static void arm(IOHIDDeviceRef d) {
    if (armed_index(d) >= 0) return;
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) if (!armed[i].dev) { slot = i; break; }
    if (slot < 0) { logmsg("too many touch devices; ignoring one"); return; }
    read_axis_ranges(d);
    armed[slot].dev = (IOHIDDeviceRef)CFRetain(d);
    IOHIDDeviceRegisterInputReportCallback(d, armed[slot].buf, sizeof armed[slot].buf, report_cb, NULL);
    char s[96]; describe(d, s, sizeof s);
    logmsg("listening to %s; X 0..%d Y 0..%d", s, (int)x_max, (int)y_max);
}

static void disarm(IOHIDDeviceRef d) {
    int i = armed_index(d);
    if (i < 0) return;
    IOHIDDeviceRegisterInputReportCallback(d, armed[i].buf, sizeof armed[i].buf, NULL, NULL);
    CFRelease(armed[i].dev);
    armed[i].dev = NULL;
}

// ---------------------------------------------------------------- digitizer mode (0xEEEF:0x2828)
//
// On some power-ups the controller enumerates as a multitouch digitizer whose control endpoint
// stalls descriptor requests, so macOS attaches no driver and touch does nothing. Only removing
// power from the monitor recovers it. The helper cannot fix this; it tells the user.

static bool digi_warned  = false;   // warned about the digitizer-mode device currently present
static bool digi_episode = false;   // digitizer mode was seen since the touch device last attached

static void digitizer_arrived(void *ctx, io_iterator_t it) {
    (void)ctx;
    bool any = false;
    for (io_object_t s; (s = IOIteratorNext(it)); IOObjectRelease(s)) any = true;
    if (!any || digi_warned) return;
    digi_warned = digi_episode = true;
    logmsg("WARNING: touch controller is in digitizer mode (eeef:2828), which macOS cannot drive. "
           "Remove power from the monitor for ~15 s, then reconnect.");
    notify("CX158 touch is not working",
           "The touch controller is in digitizer mode. Remove power from the monitor for 15 s.");
}

static void digitizer_gone(void *ctx, io_iterator_t it) {
    (void)ctx;
    for (io_object_t s; (s = IOIteratorNext(it)); IOObjectRelease(s)) digi_warned = false;
}

static void watch_digitizer(void) {
    static io_iterator_t added, removed;   // must stay alive for the notifications to keep firing
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
    // Each call consumes its matching dictionary. Draining the iterator arms the notification
    // and handles a device that is already present.
    if (IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, usb_matching(DIGI_VID, DIGI_PID),
                                         digitizer_arrived, NULL, &added) == KERN_SUCCESS)
        digitizer_arrived(NULL, added);
    if (IOServiceAddMatchingNotification(port, kIOTerminatedNotification, usb_matching(DIGI_VID, DIGI_PID),
                                         digitizer_gone, NULL, &removed) == KERN_SUCCESS)
        digitizer_gone(NULL, removed);
}

static bool digitizer_present(void) {
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, usb_matching(DIGI_VID, DIGI_PID), &it) != KERN_SUCCESS) return false;
    io_object_t svc = IOIteratorNext(it);
    bool found = svc != 0;
    if (found) IOObjectRelease(svc);
    IOObjectRelease(it);
    return found;
}

static void device_matched(void *ctx, IOReturn r, void *sender, IOHIDDeviceRef d) {
    (void)ctx; (void)r; (void)sender;
    touch_devices++;
    char s[96]; describe(d, s, sizeof s);
    logmsg("touch device attached: %s", s);
    if (digi_episode) notify("CX158 touch is back", "The touch controller is in mouse mode again.");
    digi_episode = false;
    if (manager_open) arm(d);
}

static void device_removed(void *ctx, IOReturn r, void *sender, IOHIDDeviceRef d) {
    (void)ctx; (void)r; (void)sender;
    if (touch_devices > 0) touch_devices--;
    char s[96]; describe(d, s, sizeof s);
    logmsg("touch device detached: %s", s);
    disarm(d);
    was_touching = false;
    for (int i = 0; i < 3; i++) release_owned(i, "device detached");
}

// ---------------------------------------------------------------- Input Monitoring

static const char *access_str(IOHIDAccessType a) {
    return a == kIOHIDAccessTypeGranted ? "granted" : a == kIOHIDAccessTypeDenied ? "denied" : "not decided";
}

// An Input Monitoring grant is only visible to processes started after it was given, so the
// helper relaunches itself once (launchd restarts it). A marker file limits this to one relaunch
// per RELAUNCH_COOLDOWN, so a permission that is never granted cannot cause a restart loop.
static CFRunLoopTimerRef relaunch_timer = NULL;

static const char *marker_path(void) {
    static char path[1024];
    if (!path[0]) {
        const char *tmp = getenv("TMPDIR");
        snprintf(path, sizeof path, "%s/cx158-touch-warp.relaunched", tmp && *tmp ? tmp : "/tmp");
    }
    return path;
}

static bool relaunched_recently(void) {
    struct stat st;
    return stat(marker_path(), &st) == 0 && difftime(time(NULL), st.st_mtime) < RELAUNCH_COOLDOWN;
}

static void relaunch(CFRunLoopTimerRef t, void *ctx) {
    (void)t; (void)ctx;
    FILE *f = fopen(marker_path(), "w");
    if (f) fclose(f);
    logmsg("relaunching once to pick up an Input Monitoring grant");
    exit(0);
}

static void try_open(void) {
    IOHIDAccessType a = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    if (a != kIOHIDAccessTypeGranted) {
        if (a == kIOHIDAccessTypeUnknown) IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);   // adds the binary to the list and prompts
        if (relaunched_recently()) {
            logmsg("Input Monitoring %s: idle. Enable \"cx158-touch-warp\" in System Settings > Privacy & Security > "
                   "Input Monitoring, then restart the agent.", access_str(a));
            notify("cx158-touch-warp needs Input Monitoring",
                   "Without it touches cannot be told apart from other pointing devices, so nothing is remapped.");
        } else {
            logmsg("Input Monitoring %s: idle until granted. Enable \"cx158-touch-warp\" in System Settings > Privacy & "
                   "Security > Input Monitoring. Relaunching once in %d s to pick it up.", access_str(a), (int)RELAUNCH_DELAY);
            relaunch_timer = schedule(relaunch_timer, RELAUNCH_DELAY, 0, relaunch);
        }
        return;
    }
    IOReturn r = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        logmsg("IOHIDManagerOpen failed: 0x%08x; nothing will be remapped", r);
        return;
    }
    manager_open = true;
    logmsg("Input Monitoring granted; HID manager open");
    CFSetRef devs = IOHIDManagerCopyDevices(manager);
    if (devs) {
        CFIndex n = CFSetGetCount(devs);
        if (n > 0) {
            const void **arr = calloc((size_t)n, sizeof *arr);
            CFSetGetValues(devs, arr);
            for (CFIndex i = 0; i < n; i++) arm((IOHIDDeviceRef)arr[i]);
            free(arr);
        }
        CFRelease(devs);
    }
}

// ---------------------------------------------------------------- main

static void print_check(void) {
    CGDirectDisplayID ids[16]; uint32_t count = 0;
    if (CGGetOnlineDisplayList(16, ids, &count) == kCGErrorSuccess) {
        for (uint32_t i = 0; i < count; i++) {
            CGRect b = CGDisplayBounds(ids[i]);
            logmsg("display %u: vendor 0x%04x model 0x%04x origin (%d,%d) size %dx%d pt%s%s", ids[i],
                   CGDisplayVendorNumber(ids[i]), CGDisplayModelNumber(ids[i]),
                   (int)b.origin.x, (int)b.origin.y, (int)b.size.width, (int)b.size.height,
                   CGDisplayIsBuiltin(ids[i]) ? " builtin" : "", is_target(ids[i]) ? " <- target" : "");
        }
    }
    // Permissions are checked for this process; run from a terminal they reflect the terminal's grants.
    logmsg("check: display %s; touch devices %d; digitizer-mode device %s; Input Monitoring %s; Accessibility %s",
           have_display ? "ok" : "MISSING", touch_devices,
           digitizer_present() ? "PRESENT (remove power from the monitor)" : "absent",
           access_str(IOHIDCheckAccess(kIOHIDRequestTypeListenEvent)),
           AXIsProcessTrusted() ? "granted" : "missing");
}

static void on_signal(int sig) {
    logmsg("%s (this run: %u events remapped, %u cursor warps)", sig == SIGTERM ? "terminating" : "interrupted", remap_count, warp_count);
    exit(0);
}

int main(int argc, char **argv) {
    bool check_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "--check")) check_only = true;
        else { fprintf(stderr, "usage: %s [--verbose] [--check]\n", argv[0]); return 2; }
    }
    signal(SIGCHLD, SIG_IGN);   // reap notification children automatically

    logmsg("cx158-touch-warp starting (pid %d)%s%s", getpid(), verbose ? " [verbose]" : "", check_only ? " [check]" : "");
    refresh_display();

    manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vid = TOUCH_VID, pid = TOUCH_PID;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid), p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), v);
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), p);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match); CFRelease(v); CFRelease(p);
    IOHIDManagerRegisterDeviceMatchingCallback(manager, device_matched, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(manager, device_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

    if (check_only) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);   // let the matching callbacks fire
        print_check();
        return 0;
    }

    CGDisplayRegisterReconfigurationCallback(display_cb, NULL);
    try_open();
    try_tap(NULL, NULL);
    watch_digitizer();

    // Handle SIGTERM/SIGINT on the run loop so the exit line can be logged safely.
    signal(SIGTERM, SIG_IGN); signal(SIGINT, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_t intr = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT,  0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(term, ^{ on_signal(SIGTERM); });
    dispatch_source_set_event_handler(intr, ^{ on_signal(SIGINT); });
    dispatch_resume(term); dispatch_resume(intr);

    CFRunLoopRun();
    return 0;
}
