// Push-token stand-in for offline / Personal Team builds.
//
// After login the client waits for a push token before it finishes setting
// up Home (same as Android, where FirebaseMessaging.GetTokenAsync is hooked).
// A free-signed build has no aps-environment entitlement, so APNs
// registration fails and no token ever arrives. Objective-C methods can be
// replaced at runtime without patching code pages:
//   * -[FIRMessaging tokenWithCompletion:] answers with a placeholder token;
//   * the app delegate is told that APNs registration succeeded with a
//     placeholder device token.

#import <UIKit/UIKit.h>
#import <objc/runtime.h>

extern void LinkuraOfflineLog(const char* level, const char* message);

static NSString* const kToken = @"linkura-offline-push-token";

static bool InstallFirebaseStub() {
    Class messaging = NSClassFromString(@"FIRMessaging");
    if (!messaging) return false;
    SEL selector = NSSelectorFromString(@"tokenWithCompletion:");
    Method method = class_getInstanceMethod(messaging, selector);
    if (!method) {
        LinkuraOfflineLog("W", "[Push] FIRMessaging tokenWithCompletion: not found");
        return true;
    }
    IMP stub = imp_implementationWithBlock(^(id self, void (^completion)(NSString*, NSError*)) {
        LinkuraOfflineLog("I", "[Push] FIRMessaging tokenWithCompletion -> placeholder");
        if (completion) dispatch_async(dispatch_get_main_queue(), ^{ completion(kToken, nil); });
    });
    method_setImplementation(method, stub);
    LinkuraOfflineLog("I", "[Push] FIRMessaging token stub installed");
    return true;
}

static void SendDeviceToken() {
    UIApplication* app = UIApplication.sharedApplication;
    id<UIApplicationDelegate> delegate = app.delegate;
    if (![delegate respondsToSelector:@selector(application:didRegisterForRemoteNotificationsWithDeviceToken:)]) {
        LinkuraOfflineLog("W", "[Push] app delegate has no didRegister handler");
        return;
    }
    uint8_t bytes[32];
    for (int i = 0; i < 32; i++) bytes[i] = static_cast<uint8_t>(0xA0 + i);
    [delegate application:app didRegisterForRemoteNotificationsWithDeviceToken:[NSData dataWithBytes:bytes length:32]];
    LinkuraOfflineLog("I", "[Push] placeholder APNs device token delivered");
}

void LinkuraPushStart() {
    static dispatch_source_t timer;
    static bool firebaseDone = false;
    static bool tokenSent = false;
    static int ticks = 0;
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 5), NSEC_PER_SEC / 5, NSEC_PER_SEC / 20);
    dispatch_source_set_event_handler(timer, ^{
        ticks++;
        if (!firebaseDone) firebaseDone = InstallFirebaseStub();
        // Give UnityAppController time to finish launching first.
        if (!tokenSent && firebaseDone && ticks > 25 && UIApplication.sharedApplication.delegate) {
            tokenSent = true;
            SendDeviceToken();
        }
        if ((firebaseDone && tokenSent) || ticks > 300) dispatch_source_cancel(timer);
    });
    dispatch_resume(timer);
}
