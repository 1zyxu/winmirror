#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#import <AudioToolbox/AudioToolbox.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

#define PACKET_MAGIC 0x574D4952 // 'WMIR'

enum PacketType : uint16_t {
    PACKET_TYPE_VIDEO = 0x01,
    PACKET_TYPE_AUDIO = 0x02,
    PACKET_TYPE_PING  = 0x03
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint16_t type;
    uint16_t flags;
    uint32_t payloadSize;
    uint32_t extra1;
    uint32_t extra2;
};
#pragma pack(pop)

#define NUM_AQ_BUFS 8
#define AQ_BUF_SIZE 16384

@interface MacAudioPlayer : NSObject
- (void)playPcm:(const uint8_t *)data length:(size_t)len;
- (void)recycleBuffer:(AudioQueueBufferRef)buf;
- (void)stop;
@end

@implementation MacAudioPlayer {
    AudioQueueRef _audioQueue;
    std::vector<AudioQueueBufferRef> _freeBuffers;
    std::mutex _bufMutex;
    BOOL _started;
}

static void FixedAudioQueueCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    MacAudioPlayer *player = (__bridge MacAudioPlayer *)inUserData;
    [player recycleBuffer:inBuffer];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _started = NO;

        AudioStreamBasicDescription format = {};
        format.mSampleRate = 48000.0;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        format.mFramesPerPacket = 1;
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 16;
        format.mBytesPerFrame = 4;
        format.mBytesPerPacket = 4;

        OSStatus status = AudioQueueNewOutput(
            &format,
            FixedAudioQueueCallback,
            (__bridge void *)self,
            NULL,
            kCFRunLoopCommonModes,
            0,
            &_audioQueue
        );

        if (status == noErr) {
            for (int i = 0; i < NUM_AQ_BUFS; ++i) {
                AudioQueueBufferRef b = nullptr;
                if (AudioQueueAllocateBuffer(_audioQueue, AQ_BUF_SIZE, &b) == noErr) {
                    _freeBuffers.push_back(b);
                }
            }
        }
    }
    return self;
}

- (void)recycleBuffer:(AudioQueueBufferRef)buf {
    std::lock_guard<std::mutex> lock(_bufMutex);
    _freeBuffers.push_back(buf);
}

- (void)playPcm:(const uint8_t *)data length:(size_t)len {
    if (!_audioQueue || len == 0) return;

    AudioQueueBufferRef buf = nullptr;
    {
        std::lock_guard<std::mutex> lock(_bufMutex);
        if (!_freeBuffers.empty()) {
            buf = _freeBuffers.back();
            _freeBuffers.pop_back();
        }
    }

    if (!buf) return;

    UInt32 copyLen = static_cast<UInt32>(std::min<size_t>(len, buf->mAudioDataBytesCapacity));
    memcpy(buf->mAudioData, data, copyLen);
    buf->mAudioDataByteSize = copyLen;

    AudioQueueEnqueueBuffer(_audioQueue, buf, 0, NULL);

    if (!_started) {
        _started = YES;
        AudioQueueStart(_audioQueue, NULL);
    }
}

- (void)stop {
    if (_audioQueue) {
        AudioQueueStop(_audioQueue, true);
        AudioQueueDispose(_audioQueue, true);
        _audioQueue = NULL;
    }
}

- (void)dealloc {
    [self stop];
    [super dealloc];
}

@end

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
@property (nonatomic, strong) MacAudioPlayer *audioPlayer;
@property (nonatomic, assign) NSInteger frameCount;
@property (nonatomic, assign) NSTimeInterval lastFpsTime;
@property (nonatomic, strong) NSString *hostName;
@end

@implementation MirrorWindowController

- (instancetype)initWithHost:(NSString *)host {
    self = [super init];
    if (self) {
        _hostName = host;
        _audioPlayer = [[MacAudioPlayer alloc] init];

        NSRect screenRect = [[NSScreen mainScreen] visibleFrame];
        NSRect frame = NSMakeRect(screenRect.origin.x + 40, screenRect.origin.y + 40,
                                  screenRect.size.width - 80, screenRect.size.height - 80);

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

- (void)presentImage:(CGImageRef)image width:(uint32_t)w height:(uint32_t)h {
    [self.renderView updateWithCGImage:image];

    self.frameCount++;
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    if (now - self.lastFpsTime >= 0.5) {
        double fps = (self.frameCount) / (now - self.lastFpsTime);
        self.frameCount = 0;
        self.lastFpsTime = now;
        [self.window setTitle:[NSString stringWithFormat:@"WinMirror Pro — %ux%u | %.0f FPS",
                              w, h, fps]];
    }
}

- (void)pushAudioData:(const uint8_t *)bytes length:(size_t)len {
    [self.audioPlayer playPcm:bytes length:len];
}

- (void)keyDown:(NSEvent *)event {
    if ([[event charactersIgnoringModifiers] isEqualToString:@"f"] ||
        [[event charactersIgnoringModifiers] isEqualToString:@"F"]) {
        [self.window toggleFullScreen:nil];
    }
}

- (void)windowWillClose:(NSNotification *)notification {
    [self.audioPlayer stop];
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
            usleep(200000);
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int rcvbuf = 4 * 1024 * 1024;
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

        std::string req = "GET /raw HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
        send(sock, req.c_str(), req.size(), 0);

        std::string headerBuf;
        char ch;
        while (headerBuf.find("\r\n\r\n") == std::string::npos) {
            if (recv(sock, &ch, 1, 0) <= 0) break;
            headerBuf += ch;
            if (headerBuf.size() > 2048) break;
        }

        std::vector<uint8_t> payloadBuf;
        payloadBuf.reserve(4 * 1024 * 1024);

        while (*running) {
            PacketHeader header = {};
            if (!recvExact(sock, &header, sizeof(header))) break;

            if (header.magic != PACKET_MAGIC) {
                break;
            }

            if (header.payloadSize == 0 || header.payloadSize > 20 * 1024 * 1024) {
                break;
            }

            if (payloadBuf.size() < header.payloadSize) {
                payloadBuf.resize(header.payloadSize);
            }

            if (!recvExact(sock, payloadBuf.data(), header.payloadSize)) break;

            if (header.type == PACKET_TYPE_VIDEO) {
                CFDataRef cfData = CFDataCreate(kCFAllocatorDefault, payloadBuf.data(), header.payloadSize);
                if (cfData) {
                    CGImageSourceRef source = CGImageSourceCreateWithData(cfData, NULL);
                    CFRelease(cfData);
                    if (source) {
                        CGImageRef cgImage = CGImageSourceCreateImageAtIndex(source, 0, NULL);
                        CFRelease(source);
                        if (cgImage) {
                            uint32_t w = header.extra1;
                            uint32_t h = header.extra2;
                            dispatch_async(dispatch_get_main_queue(), ^{
                                [controller presentImage:cgImage width:w height:h];
                                CGImageRelease(cgImage);
                            });
                        }
                    }
                }
            } else if (header.type == PACKET_TYPE_AUDIO) {
                [controller pushAudioData:payloadBuf.data() length:header.payloadSize];
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
