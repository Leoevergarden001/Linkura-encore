// Remote control for on-device debugging without a screen mirror.
//
// The Mac drops Documents/LinkuraOffline/cmd.json into the app container
// (devicectl device copy to). A main-queue timer picks it up:
//   {"id": 7, "tap": [x, y]}   tap at window point (x, y), then screenshot
//   {"id": 8}                  screenshot only
// The screenshot is written to Documents/LinkuraOffline/screen.png at 1 px
// per point and cmd.done receives the id. Only compiled into the debug
// dylib flow; it does nothing unless a command file appears.

#import <UIKit/UIKit.h>
#include <unistd.h>

extern void LinkuraOfflineLog(const char* level, const char* message);

@interface LKTouch : UITouch
@property(nonatomic) CGPoint lkPoint;
@property(nonatomic) UITouchPhase lkPhase;
@property(nonatomic, weak) UIView* lkView;
@property(nonatomic) NSTimeInterval lkTime;
@end

@implementation LKTouch
- (UITouchPhase)phase { return self.lkPhase; }
- (NSUInteger)tapCount { return 1; }
- (NSTimeInterval)timestamp { return self.lkTime; }
- (UITouchType)type { return UITouchTypeDirect; }
- (UIView*)view { return self.lkView; }
- (UIWindow*)window { return self.lkView.window; }
- (CGFloat)majorRadius { return 20; }
- (CGFloat)majorRadiusTolerance { return 5; }
- (CGFloat)force { return 0; }
- (CGFloat)maximumPossibleForce { return 0; }
- (CGFloat)altitudeAngle { return M_PI_2; }
- (CGFloat)azimuthAngleInView:(UIView*)view { return 0; }
- (CGVector)azimuthUnitVectorInView:(UIView*)view { return CGVectorMake(1, 0); }
- (NSArray*)gestureRecognizers { return @[]; }
- (CGPoint)locationInView:(UIView*)view {
    UIWindow* window = self.lkView.window;
    return view ? [window convertPoint:self.lkPoint toView:view] : self.lkPoint;
}
- (CGPoint)previousLocationInView:(UIView*)view { return [self locationInView:view]; }
- (CGPoint)preciseLocationInView:(UIView*)view { return [self locationInView:view]; }
- (CGPoint)precisePreviousLocationInView:(UIView*)view { return [self locationInView:view]; }
@end

static UIWindow* LKWindow() {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene* windowScene = (UIWindowScene*)scene;
        for (UIWindow* window in windowScene.windows) if (window.isKeyWindow) return window;
        if (windowScene.windows.count) return windowScene.windows.firstObject;
    }
    return nil;
}

static UIView* LKFindUnityView(UIView* view) {
    if ([NSStringFromClass(view.class) containsString:@"UnityView"]) return view;
    for (UIView* child in view.subviews) {
        UIView* found = LKFindUnityView(child);
        if (found) return found;
    }
    return nil;
}

static void LKScreenshot(NSURL* directory) {
    UIWindow* window = LKWindow();
    if (!window) { LinkuraOfflineLog("W", "[Remote] no window for screenshot"); return; }
    UIGraphicsImageRendererFormat* format = [UIGraphicsImageRendererFormat preferredFormat];
    format.scale = 1;
    UIGraphicsImageRenderer* renderer = [[UIGraphicsImageRenderer alloc] initWithSize:window.bounds.size format:format];
    UIImage* image = [renderer imageWithActions:^(UIGraphicsImageRendererContext* context) {
        [window drawViewHierarchyInRect:window.bounds afterScreenUpdates:NO];
    }];
    [UIImagePNGRepresentation(image) writeToURL:[directory URLByAppendingPathComponent:@"screen.png"] atomically:YES];
}

static void LKTap(CGPoint point) {
    UIWindow* window = LKWindow();
    UIView* target = window ? LKFindUnityView(window) : nil;
    if (!target) { LinkuraOfflineLog("W", "[Remote] UnityView not found for tap"); return; }
    LKTouch* touch = [[LKTouch alloc] init];
    touch.lkPoint = point;
    touch.lkView = target;
    touch.lkPhase = UITouchPhaseBegan;
    touch.lkTime = NSProcessInfo.processInfo.systemUptime;
    NSSet* touches = [NSSet setWithObject:touch];
    [target touchesBegan:touches withEvent:nil];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.08 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        touch.lkPhase = UITouchPhaseEnded;
        touch.lkTime = NSProcessInfo.processInfo.systemUptime;
        [target touchesEnded:touches withEvent:nil];
    });
}

void LinkuraRemoteStart(NSURL* directory) {
    static dispatch_source_t timer;
    NSURL* command = [directory URLByAppendingPathComponent:@"cmd.json"];
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), NSEC_PER_SEC / 2, NSEC_PER_SEC / 5);
    dispatch_source_set_event_handler(timer, ^{
        // Cheap existence check only; nothing else runs on the main thread
        // unless a command is waiting (keeps frame pacing untouched).
        if (access(command.fileSystemRepresentation, F_OK) != 0) return;
        NSData* raw = [NSData dataWithContentsOfURL:command];
        if (!raw) return;
        [NSFileManager.defaultManager removeItemAtURL:command error:nil];
        id parsed = [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil];
        if (![parsed isKindOfClass:NSDictionary.class]) return;
        NSDictionary* request = parsed;
        NSArray* tap = request[@"tap"];
        NSTimeInterval delay = 0.05;
        if ([tap isKindOfClass:NSArray.class] && tap.count == 2) {
            LKTap(CGPointMake([tap[0] doubleValue], [tap[1] doubleValue]));
            delay = [request[@"wait"] isKindOfClass:NSNumber.class] ? [request[@"wait"] doubleValue] : 1.5;
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            LKScreenshot(directory);
            NSString* done = [NSString stringWithFormat:@"%@\n", request[@"id"] ?: @0];
            [done writeToURL:[directory URLByAppendingPathComponent:@"cmd.done"] atomically:YES
                    encoding:NSUTF8StringEncoding error:nil];
        });
    });
    dispatch_resume(timer);
}
