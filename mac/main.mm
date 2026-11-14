#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <iostream>

@interface FastImageView : NSView
@property (nonatomic, strong) NSImage *currentImage;
- (void)displayImage:(NSImage *)image;
@end

@implementation FastImageView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        self.layer.drawsAsynchronously = YES;
        self.layer.contentsGravity = kCAGravityResizeAspect;
    }
    return self;
}

- (void)displayImage:(NSImage *)image {
    if (!image) return;
    CGImageRef cgImage = [image CGImageForProposedRect:nil context:nil hints:nil];
    if (cgImage) {
        self.layer.contents = (__bridge id)cgImage;
    }
}

@end

@interface MirrorWindowController : NSObject <NSWindowDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) FastImageView *renderView;
@property (nonatomic, strong) NSTextField *fpsBadge;
@property (nonatomic, assign) NSInteger frameCount;
@property (nonatomic, assign) NSTimeInterval lastFpsTime;
@property (atomic, strong) NSData *latestJpegData;
@property (atomic, assign) BOOL isRenderScheduled;
@property (nonatomic, strong) NSString *hostName;
@end

@implementation MirrorWindowController

- (instancetype)initWithHost:(NSString *)host {
    self = [super init];
    if (self) {
        _hostName = host;
        NSRect screenRect = [[NSScreen mainScreen] visibleFrame];
        NSRect frame = NSMakeRect(screenRect.origin.x + 50, screenRect.origin.y + 50, screenRect.size.width - 100, screenRect.size.height - 100);

        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        _window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        [_window setTitle:[NSString stringWithFormat:@"WinMirror — Connecting to %@...", host]];
        [_window setBackgroundColor:[NSColor blackColor]];
        [_window setMinSize:NSMakeSize(640, 360)];
        [_window setDelegate:self];

        _renderView = [[FastImageView alloc] initWithFrame:[[_window contentView] bounds]];
        [_renderView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [[_window contentView] addSubview:_renderView];

        _fpsBadge = [[NSTextField alloc] initWithFrame:NSMakeRect(20, [[_window contentView] bounds].size.height - 45, 260, 30)];
        [_fpsBadge setAutoresizingMask:NSViewMinYMargin | NSViewMaxXMargin];
        [_fpsBadge setEditable:NO];
        [_fpsBadge setSelectable:NO];
        [_fpsBadge setBezeled:NO];
        [_fpsBadge setDrawsBackground:YES];
        [_fpsBadge setBackgroundColor:[NSColor colorWithCalibratedRed:0 green:0 blue:0 alpha:0.65]];
        [_fpsBadge setTextColor:[NSColor colorWithCalibratedRed:0.2 green:0.9 blue:0.4 alpha:1.0]];
        [_fpsBadge setFont:[NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightBold]];
        [_fpsBadge setStringValue:@"Connecting..."];
        [_fpsBadge.layer setCornerRadius:6];
        [[_window contentView] addSubview:_fpsBadge];

        [_window makeKeyAndOrderFront:nil];
        _lastFpsTime = [NSDate timeIntervalSinceReferenceDate];
    }
    return self;
}

- (void)pushJpegFrame:(NSData *)jpegData {
    self.latestJpegData = jpegData;
    self.frameCount++;

    if (!self.isRenderScheduled) {
        self.isRenderScheduled = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            NSData *data = self.latestJpegData;
            self.isRenderScheduled = NO;
            if (data) {
                NSImage *image = [[NSImage alloc] initWithData:data];
                [self.renderView displayImage:image];
            }

            NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
            if (now - self.lastFpsTime >= 0.5) {
                double fps = (self.frameCount) / (now - self.lastFpsTime);
                self.frameCount = 0;
                self.lastFpsTime = now;
                [self.fpsBadge setStringValue:[NSString stringWithFormat:@"⚡ %.0f FPS (Unlimited)", fps]];
                [self.window setTitle:[NSString stringWithFormat:@"WinMirror — %@ | %.0f FPS", self.hostName, fps]];
            }
        });
    }
}

- (void)windowWillClose:(NSNotification *)notification {
    exit(0);
}

@end

static void streamWorker(MirrorWindowController *controller, std::string host, int port, std::atomic<bool> *running) {
    while (*running) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            usleep(200000);
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int rcvbuf = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct timeval tv = { 3, 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in saddr = {};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &saddr.sin_addr);

        if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
            close(sock);
            usleep(500000);
            continue;
        }

        std::string req = "GET /stream HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        send(sock, req.c_str(), req.size(), 0);

        std::vector<uint8_t> buffer;
        buffer.reserve(512 * 1024);
        char chunk[32768];

        while (*running) {
            ssize_t bytes = recv(sock, chunk, sizeof(chunk), 0);
            if (bytes <= 0) break;

            buffer.insert(buffer.end(), chunk, chunk + bytes);

            while (buffer.size() >= 4) {
                // Find SOI (0xFF 0xD8)
                size_t soi = std::string::npos;
                for (size_t i = 0; i + 1 < buffer.size(); ++i) {
                    if (buffer[i] == 0xFF && buffer[i + 1] == 0xD8) {
                        soi = i;
                        break;
                    }
                }

                if (soi == std::string::npos) {
                    buffer.clear();
                    break;
                }

                // Find EOI (0xFF 0xD9)
                size_t eoi = std::string::npos;
                for (size_t i = soi + 2; i + 1 < buffer.size(); ++i) {
                    if (buffer[i] == 0xFF && buffer[i + 1] == 0xD9) {
                        eoi = i + 2;
                        break;
                    }
                }

                if (eoi == std::string::npos) {
                    if (soi > 0) {
                        buffer.erase(buffer.begin(), buffer.begin() + soi);
                    }
                    break;
                }

                NSData *frameData = [NSData dataWithBytes:&buffer[soi] length:(eoi - soi)];
                [controller pushJpegFrame:frameData];

                buffer.erase(buffer.begin(), buffer.begin() + eoi);
            }
        }

        close(sock);
        usleep(200000);
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        std::string targetHost = "192.168.0.105";
        int port = 8888;

        if (argc > 1) {
            targetHost = argv[1];
        }
        if (argc > 2) {
            port = std::stoi(argv[2]);
        }

        NSString *nsHost = [NSString stringWithUTF8String:targetHost.c_str()];
        MirrorWindowController *controller = [[MirrorWindowController alloc] initWithHost:nsHost];

        std::atomic<bool> running{true};
        std::thread worker(streamWorker, controller, targetHost, port, &running);
        worker.detach();

        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];

        running = false;
    }
    return 0;
}
