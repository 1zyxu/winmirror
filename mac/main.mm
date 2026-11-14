#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <sstream>

@interface RenderView : NSView
- (void)updateWithCGImage:(CGImageRef)image;
@end

@implementation RenderView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        self.layer.drawsAsynchronously = YES;
        self.layer.contentsGravity = kCAGravityResizeAspect;
        self.layer.backgroundColor = [NSColor blackColor].CGColor;
    }
    return self;
}

- (void)updateWithCGImage:(CGImageRef)image {
    if (image) {
        self.layer.contents = (__bridge id)image;
    }
}

@end

@interface MirrorWindowController : NSObject <NSWindowDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) RenderView *renderView;
@property (nonatomic, assign) NSInteger frameCount;
@property (nonatomic, assign) NSTimeInterval lastFpsTime;
@property (nonatomic, strong) NSString *hostName;
@property (atomic, assign) BOOL isRenderPending;
@property (atomic, assign) CGImageRef latestImage;
@end

@implementation MirrorWindowController

- (instancetype)initWithHost:(NSString *)host {
    self = [super init];
    if (self) {
        _hostName = host;
        NSRect screenRect = [[NSScreen mainScreen] visibleFrame];
        NSRect frame = NSMakeRect(screenRect.origin.x + 60, screenRect.origin.y + 60,
                                  screenRect.size.width - 120, screenRect.size.height - 120);

        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        _window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        [_window setTitle:@"WinMirror — Connecting..."];
        [_window setBackgroundColor:[NSColor blackColor]];
        [_window setMinSize:NSMakeSize(800, 450)];
        [_window setDelegate:self];

        _renderView = [[RenderView alloc] initWithFrame:[[_window contentView] bounds]];
        [_renderView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [[_window contentView] addSubview:_renderView];

        [_window makeKeyAndOrderFront:nil];
        _lastFpsTime = [NSDate timeIntervalSinceReferenceDate];
    }
    return self;
}

- (void)pushFrameData:(const uint8_t *)bytes length:(size_t)len {
    CFDataRef cfData = CFDataCreate(kCFAllocatorDefault, bytes, len);
    if (!cfData) return;

    CGImageSourceRef source = CGImageSourceCreateWithData(cfData, NULL);
    CFRelease(cfData);
    if (!source) return;

    CGImageRef cgImage = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    CFRelease(source);
    if (!cgImage) return;

    CGImageRef oldImage = self.latestImage;
    self.latestImage = cgImage;
    if (oldImage) {
        CGImageRelease(oldImage);
    }

    self.frameCount++;

    if (!self.isRenderPending) {
        self.isRenderPending = YES;
        dispatch_async(dispatch_get_main_queue(), ^{
            CGImageRef imgToRender = self.latestImage;
            self.isRenderPending = NO;
            if (imgToRender) {
                [self.renderView updateWithCGImage:imgToRender];
            }

            NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
            if (now - self.lastFpsTime >= 1.0) {
                double fps = (self.frameCount) / (now - self.lastFpsTime);
                self.frameCount = 0;
                self.lastFpsTime = now;
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
            usleep(250000);
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int rcvbuf = 2 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct timeval tv = { 4, 0 };
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
        buffer.reserve(1024 * 1024);
        char chunk[65536];

        while (*running) {
            ssize_t bytes = recv(sock, chunk, sizeof(chunk), 0);
            if (bytes <= 0) break;

            buffer.insert(buffer.end(), chunk, chunk + bytes);

            // Robust multipart parser using exact Content-Length
            while (true) {
                // Find header delimiter: "\r\n\r\n"
                std::string bufStr(reinterpret_cast<char*>(buffer.data()), buffer.size());
                size_t headerEnd = bufStr.find("\r\n\r\n");
                if (headerEnd == std::string::npos) break;

                // Look for "Content-Length:" inside header
                size_t clPos = bufStr.find("Content-Length:");
                if (clPos == std::string::npos || clPos > headerEnd) {
                    // Skip boundary line if before Content-Length
                    buffer.erase(buffer.begin(), buffer.begin() + headerEnd + 4);
                    continue;
                }

                size_t valStart = clPos + 15;
                while (valStart < headerEnd && (bufStr[valStart] == ' ' || bufStr[valStart] == '\t')) valStart++;
                size_t valEnd = bufStr.find("\r\n", valStart);
                if (valEnd == std::string::npos || valEnd > headerEnd) break;

                size_t contentLength = 0;
                try {
                    contentLength = std::stoul(bufStr.substr(valStart, valEnd - valStart));
                } catch (...) {
                    buffer.erase(buffer.begin(), buffer.begin() + headerEnd + 4);
                    continue;
                }

                size_t bodyStart = headerEnd + 4;
                if (buffer.size() < bodyStart + contentLength) {
                    // Wait for remaining frame bytes to arrive
                    break;
                }

                // Complete valid JPEG frame
                [controller pushFrameData:buffer.data() + bodyStart length:contentLength];

                // Remove processed frame from buffer
                buffer.erase(buffer.begin(), buffer.begin() + bodyStart + contentLength);
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
