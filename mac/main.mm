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

#define RAW_FRAME_MAGIC 0x52494D57 // 'WMIR'

#pragma pack(push, 1)
struct RawFrameHeader {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t payloadSize;
};
#pragma pack(pop)

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
@property (nonatomic, assign) uint32_t currentWidth;
@property (nonatomic, assign) uint32_t currentHeight;
@end

@implementation MirrorWindowController

- (instancetype)initWithHost:(NSString *)host {
    self = [super init];
    if (self) {
        _hostName = host;
        NSRect screenRect = [[NSScreen mainScreen] visibleFrame];
        NSRect frame = NSMakeRect(screenRect.origin.x + 50, screenRect.origin.y + 50,
                                  screenRect.size.width - 100, screenRect.size.height - 100);

        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        _window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        [_window setTitle:[NSString stringWithFormat:@"WinMirror Pro — Connecting to %@...", host]];
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

- (void)pushFrameData:(const uint8_t *)bytes length:(size_t)len width:(uint32_t)w height:(uint32_t)h {
    self.currentWidth = w;
    self.currentHeight = h;

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
            if (now - self.lastFpsTime >= 0.5) {
                double fps = (self.frameCount) / (now - self.lastFpsTime);
                self.frameCount = 0;
                self.lastFpsTime = now;
                [self.window setTitle:[NSString stringWithFormat:@"WinMirror Pro — %ux%u | %.0f FPS",
                                      self.currentWidth, self.currentHeight, fps]];
            }
        });
    }
}

- (void)keyDown:(NSEvent *)event {
    if ([[event charactersIgnoringModifiers] isEqualToString:@"f"] ||
        [[event charactersIgnoringModifiers] isEqualToString:@"F"]) {
        [self.window toggleFullScreen:nil];
    }
}

- (void)windowWillClose:(NSNotification *)notification {
    exit(0);
}

@end

static bool recvExact(int sock, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t r = recv(sock, p + total, len - total, 0);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

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

        struct timeval tv = { 5, 0 };
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

        // Request ultra-fast raw binary stream
        std::string req = "GET /raw HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        send(sock, req.c_str(), req.size(), 0);

        // Read until HTTP header boundary "\r\n\r\n"
        std::string headerBuf;
        char ch;
        while (headerBuf.find("\r\n\r\n") == std::string::npos) {
            if (recv(sock, &ch, 1, 0) <= 0) break;
            headerBuf += ch;
            if (headerBuf.size() > 2048) break;
        }

        std::vector<uint8_t> payloadBuf;
        payloadBuf.reserve(1024 * 1024);

        while (*running) {
            RawFrameHeader header = {};
            if (!recvExact(sock, &header, sizeof(header))) break;

            if (header.magic != RAW_FRAME_MAGIC) {
                // Out of sync, reconnect
                break;
            }

            if (header.payloadSize == 0 || header.payloadSize > 10 * 1024 * 1024) {
                break;
            }

            if (payloadBuf.size() < header.payloadSize) {
                payloadBuf.resize(header.payloadSize);
            }

            if (!recvExact(sock, payloadBuf.data(), header.payloadSize)) break;

            [controller pushFrameData:payloadBuf.data()
                               length:header.payloadSize
                                width:header.width
                               height:header.height];
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
