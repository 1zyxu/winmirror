#import <Cocoa/Cocoa.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <iostream>

@interface MirrorWindowController : NSObject <NSWindowDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) NSImageView *imageView;
@property (nonatomic, strong) NSTextField *statusLabel;
@property (nonatomic, assign) NSInteger frameCount;
@property (nonatomic, assign) NSTimeInterval lastFpsTime;
@end

@implementation MirrorWindowController

- (instancetype)init {
    self = [super init];
    if (self) {
        NSRect screenRect = [[NSScreen mainScreen] visibleFrame];
        NSRect frame = NSMakeRect(screenRect.origin.x + 100, screenRect.origin.y + 100, 1280, 720);

        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        _window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        [_window setTitle:@"WinMirror - Connecting to PC..."];
        [_window setBackgroundColor:[NSColor blackColor]];
        [_window setMinSize:NSMakeSize(640, 360)];
        [_window setDelegate:self];

        _imageView = [[NSImageView alloc] initWithFrame:[[_window contentView] bounds]];
        [_imageView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [_imageView setImageScaling:NSImageScaleProportionallyUpOrDown];
        [[_window contentView] addSubview:_imageView];

        _statusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(16, 16, 400, 28)];
        [_statusLabel setEditable:NO];
        [_statusLabel setSelectable:NO];
        [_statusLabel setBezeled:NO];
        [_statusLabel setDrawsBackground:NO];
        [_statusLabel setTextColor:[NSColor greenColor]];
        [_statusLabel setFont:[NSFont boldSystemFontOfSize:14]];
        [_statusLabel setStringValue:@"Searching for PC on USB / Network..."];
        [[_window contentView] addSubview:_statusLabel];

        [_window makeKeyAndOrderFront:nil];
        _lastFpsTime = [NSDate timeIntervalSinceReferenceDate];
    }
    return self;
}

- (void)updateFrameWithData:(NSData *)jpegData host:(NSString *)host {
    self.frameCount++;
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    if (now - self.lastFpsTime >= 1.0) {
        NSInteger fps = self.frameCount;
        self.frameCount = 0;
        self.lastFpsTime = now;
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.window setTitle:[NSString stringWithFormat:@"WinMirror &mdash; %@ (%ld FPS)", host, (long)fps]];
            [self.statusLabel setHidden:YES];
        });
    }

    NSImage *image = [[NSImage alloc] initWithData:jpegData];
    if (image) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self.imageView setImage:image];
        });
    }
}

- (void)setStatus:(NSString *)statusText {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self.statusLabel setStringValue:statusText];
        [self.statusLabel setHidden:NO];
    });
}

- (void)toggleFullscreen {
    [self.window toggleFullScreen:nil];
}

@end

static std::string discoverPcIp(int beaconPort = 8889, int timeoutSec = 3) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "";

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct timeval tv = { timeoutSec, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(beaconPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    char buffer[512] = {};
    sockaddr_in senderAddr = {};
    socklen_t senderLen = sizeof(senderAddr);

    ssize_t received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&senderAddr, &senderLen);
    close(sock);

    if (received > 0) {
        char senderIp[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &senderAddr.sin_addr, senderIp, sizeof(senderIp));
        return std::string(senderIp);
    }

    return "";
}

static void streamWorker(MirrorWindowController *controller, std::string targetHost, int port, std::atomic<bool> *running) {
    while (*running) {
        std::string host = targetHost;
        if (host.empty() || host == "auto") {
            [controller setStatus:@"Searching for Windows PC beacon..."];
            host = discoverPcIp(8889, 2);
            if (host.empty()) {
                // Default fallback to common local host or check local loop
                host = "127.0.0.1";
            }
        }

        NSString *nsHost = [NSString stringWithUTF8String:host.c_str()];
        [controller setStatus:[NSString stringWithFormat:@"Connecting to http://%s:%d/stream ...", host.c_str(), port]];

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            sleep(1);
            continue;
        }

        struct timeval tv = { 4, 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in saddr = {};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &saddr.sin_addr);

        if (connect(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
            close(sock);
            sleep(1);
            continue;
        }

        std::string req = "GET /stream HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        send(sock, req.c_str(), req.size(), 0);

        std::vector<uint8_t> buffer;
        buffer.reserve(512 * 1024);
        char chunk[16384];

        while (*running) {
            ssize_t bytes = recv(sock, chunk, sizeof(chunk), 0);
            if (bytes <= 0) break;

            buffer.insert(buffer.end(), chunk, chunk + bytes);

            // Look for JPEG delimiters (0xFFD8 to 0xFFD9)
            while (buffer.size() >= 4) {
                // Find SOI (Start of Image): 0xFF 0xD8
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

                // Find EOI (End of Image): 0xFF 0xD9
                size_t eoi = std::string::npos;
                for (size_t i = soi + 2; i + 1 < buffer.size(); ++i) {
                    if (buffer[i] == 0xFF && buffer[i + 1] == 0xD9) {
                        eoi = i + 2;
                        break;
                    }
                }

                if (eoi == std::string::npos) {
                    // Incomplete frame, wait for more data
                    if (soi > 0) {
                        buffer.erase(buffer.begin(), buffer.begin() + soi);
                    }
                    break;
                }

                NSData *data = [NSData dataWithBytes:&buffer[soi] length:(eoi - soi)];
                [controller updateFrameWithData:data host:nsHost];

                buffer.erase(buffer.begin(), buffer.begin() + eoi);
            }
        }

        close(sock);
        sleep(1);
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        std::string targetHost = "auto";
        int port = 8888;

        if (argc > 1) {
            targetHost = argv[1];
        }
        if (argc > 2) {
            port = std::stoi(argv[2]);
        }

        MirrorWindowController *controller = [[MirrorWindowController alloc] init];

        std::atomic<bool> running{true};
        std::thread worker(streamWorker, controller, targetHost, port, &running);
        worker.detach();

        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];

        running = false;
    }
    return 0;
}
