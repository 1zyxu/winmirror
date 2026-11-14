#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#import <IOBluetooth/IOBluetooth.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>

#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <sstream>

#define PACKET_MAGIC 0x574D4952 // 'WMIR'
#define AIRPLAY_PORT 7001
#define BEACON_PORT 8889

enum VideoCodec : uint8_t {
    CODEC_H264 = 1,
    CODEC_HEVC = 2
};

enum ColorProfile : uint8_t {
    COLOR_SDR_BT709  = 1,
    COLOR_HDR10_PQ   = 2
};

enum PacketType : uint16_t {
    PACKET_TYPE_HELLO         = 0x01,
    PACKET_TYPE_VIDEO         = 0x02,
    PACKET_TYPE_AUDIO         = 0x03,
    PACKET_TYPE_CURSOR        = 0x04,
    PACKET_TYPE_PING          = 0x05,
    PACKET_TYPE_PONG          = 0x06,
    PACKET_TYPE_KEYFRAME_REQ  = 0x07,
    PACKET_TYPE_AUDIO_DEVICES = 0x08,
    PACKET_TYPE_SET_AUDIO_SRC = 0x09
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

struct HelloPayload {
    uint32_t width;
    uint32_t height;
    uint16_t fps;
    uint8_t codec;
    uint8_t colorProfile;
    uint32_t audioSampleRate;
    uint16_t audioChannels;
    uint16_t flags;
    char deviceName[64];
};

struct CursorPayload {
    int32_t x;
    int32_t y;
    uint32_t screenW;
    uint32_t screenH;
    int16_t hotspotX;
    int16_t hotspotY;
    uint16_t width;
    uint16_t height;
    uint8_t visible;
    uint8_t hasBitmap;
    uint16_t reserved;
};
#pragma pack(pop)

// ============================================================================
// CoreAudio Device Enumeration + Low-Latency Pull-Ring Audio Output
// ============================================================================
#define NUM_AQ_BUFS 4

struct MacAudioDeviceInfo {
    AudioDeviceID deviceId = 0;
    NSString *name = nil;
    NSString *uid = nil;
    BOOL isDefault = NO;
    BOOL isMuted = NO;
    float volume = -1.0f;
};

static std::vector<MacAudioDeviceInfo> enumerateMacOutputDevices() {
    std::vector<MacAudioDeviceInfo> result;

    AudioDeviceID defId = 0;
    UInt32 defSize = sizeof(defId);
    AudioObjectPropertyAddress defAddr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &defAddr, 0, nullptr, &defSize, &defId);

    AudioObjectPropertyAddress devListAddr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 ds = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devListAddr, 0, nullptr, &ds) != noErr || ds == 0) {
        return result;
    }

    std::vector<AudioDeviceID> ids(ds / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &devListAddr, 0, nullptr, &ds, ids.data()) != noErr) {
        return result;
    }

    for (AudioDeviceID devId : ids) {
        AudioObjectPropertyAddress streamAddr = {
            kAudioDevicePropertyStreams,
            kAudioObjectPropertyScopeOutput,
            kAudioObjectPropertyElementMain
        };
        UInt32 ss = 0;
        if (AudioObjectGetPropertyDataSize(devId, &streamAddr, 0, nullptr, &ss) != noErr || ss == 0) {
            continue;
        }

        CFStringRef cfName = nullptr, cfUid = nullptr;
        UInt32 ns = sizeof(cfName), us = sizeof(cfUid);
        AudioObjectPropertyAddress na = { kAudioDevicePropertyDeviceNameCFString, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectGetPropertyData(devId, &na, 0, nullptr, &ns, &cfName);
        AudioObjectGetPropertyData(devId, &ua, 0, nullptr, &us, &cfUid);

        if (!cfName || !cfUid) {
            if (cfName) CFRelease(cfName);
            if (cfUid) CFRelease(cfUid);
            continue;
        }

        MacAudioDeviceInfo info;
        info.deviceId = devId;
        info.name = [NSString stringWithString:(__bridge NSString *)cfName];
        info.uid = [NSString stringWithString:(__bridge NSString *)cfUid];
        info.isDefault = (devId == defId);

        UInt32 muted = 0, ms = sizeof(muted);
        AudioObjectPropertyAddress ma = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(devId, &ma, 0, nullptr, &ms, &muted) == noErr) {
            info.isMuted = (muted != 0);
        }

        Float32 vol = -1.0f;
        UInt32 vs = sizeof(vol);
        AudioObjectPropertyAddress va0 = { kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, 0 };
        AudioObjectPropertyAddress va1 = { kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, 1 };
        if (AudioObjectGetPropertyData(devId, &va0, 0, nullptr, &vs, &vol) == noErr) {
            info.volume = vol;
        } else if (AudioObjectGetPropertyData(devId, &va1, 0, nullptr, &vs, &vol) == noErr) {
            info.volume = vol;
        }

        CFRelease(cfName);
        CFRelease(cfUid);
        result.push_back(std::move(info));
    }
    return result;
}

static void unmuteMacDevice(AudioDeviceID devId, float targetVolume) {
    UInt32 muted = 0;
    AudioObjectPropertyAddress ma = { kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(devId, &ma, 0, nullptr, sizeof(muted), &muted);

    Float32 vol = std::max(0.0f, std::min(1.0f, targetVolume));
    for (UInt32 ch : { 0u, 1u, 2u }) {
        AudioObjectPropertyAddress va = { kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput, ch };
        AudioObjectSetPropertyData(devId, &va, 0, nullptr, sizeof(vol), &vol);
    }
}

@interface MacAudioPlayer : NSObject
@property (nonatomic, copy) NSString *selectedDeviceUID;
@property (nonatomic, assign) BOOL muted;
@property (nonatomic, readonly) uint32_t activeSampleRate;
@property (nonatomic, readonly) BOOL hasActiveSignal;

- (void)playPcm:(const uint8_t *)data length:(size_t)len sampleRate:(uint32_t)sampleRate;
- (void)handleCallback:(AudioQueueBufferRef)buf;
- (void)handleDefaultDeviceRouteChange;
- (void)switchToDeviceUID:(NSString *)uid;
- (void)restartAudio;
- (void)stop;
@end

@implementation MacAudioPlayer {
    AudioQueueRef _audioQueue;
    std::mutex _queueMutex;
    std::mutex _bufMutex;
    std::deque<uint8_t> _pcmRing;
    uint32_t _sampleRate;
    UInt32 _chunkBytes;
    BOOL _started;
    std::atomic<bool> _disposing;
    std::atomic<bool> _alive;
    std::chrono::steady_clock::time_point _lastSignalTime;
}

static void AirPlayAudioCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    MacAudioPlayer *player = (__bridge MacAudioPlayer *)inUserData;
    [player handleCallback:inBuffer];
}

static OSStatus OnCoreAudioRouteChanged(AudioObjectID, UInt32, const AudioObjectPropertyAddress *, void *inClientData) {
    MacAudioPlayer *player = (__bridge MacAudioPlayer *)inClientData;
    [player retain];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        [player handleDefaultDeviceRouteChange];
        [player release];
    });
    return noErr;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _sampleRate = 48000;
        _chunkBytes = (48000 / 50) * 4;
        _started = NO;
        _disposing = false;
        _alive = true;
        _muted = NO;
        _selectedDeviceUID = nil;
        _lastSignalTime = std::chrono::steady_clock::time_point{};

        AudioObjectPropertyAddress defOutAddr = {
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        AudioObjectAddPropertyListener(kAudioObjectSystemObject, &defOutAddr, OnCoreAudioRouteChanged, (__bridge void *)self);
    }
    return self;
}

- (uint32_t)activeSampleRate {
    return _sampleRate;
}

- (BOOL)hasActiveSignal {
    auto age = std::chrono::steady_clock::now() - _lastSignalTime;
    return _lastSignalTime.time_since_epoch().count() > 0 && age < std::chrono::seconds(2);
}

- (void)stopLocked {
    _disposing = true;
    AudioQueueRef q = _audioQueue;
    _audioQueue = NULL;
    _started = NO;

    if (q) {
        AudioQueueStop(q, true);
        AudioQueueDispose(q, true);
    }
    {
        std::lock_guard<std::mutex> lock(_bufMutex);
        _pcmRing.clear();
    }
    _disposing = false;
}

- (void)configureForSampleRateLocked:(uint32_t)sr {
    [self stopLocked];
    if (_muted) return;

    if (sr < 8000 || sr > 192000) sr = 48000;
    _sampleRate = sr;
    _chunkBytes = ((sr / 50) * 4);
    if (_chunkBytes < 256) _chunkBytes = 3840;

    AudioStreamBasicDescription format = {};
    format.mSampleRate = static_cast<Float64>(sr);
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 16;
    format.mBytesPerFrame = 4;
    format.mBytesPerPacket = 4;

    OSStatus status = AudioQueueNewOutput(
        &format,
        AirPlayAudioCallback,
        (__bridge void *)self,
        NULL,
        kCFRunLoopCommonModes,
        0,
        &_audioQueue
    );

    if (status != noErr || !_audioQueue) {
        _audioQueue = NULL;
        return;
    }

    if (_selectedDeviceUID.length > 0) {
        CFStringRef cfUid = (__bridge CFStringRef)_selectedDeviceUID;
        OSStatus routeSt = AudioQueueSetProperty(_audioQueue, kAudioQueueProperty_CurrentDevice, &cfUid, sizeof(cfUid));
        if (routeSt != noErr) {
            // Fallback gracefully to system default if selected external device vanished
            _selectedDeviceUID = nil;
        }
    }

    // Prime continuous pull buffers so CoreAudio & Bluetooth A2DP never starve or drop bursts
    for (int i = 0; i < NUM_AQ_BUFS; ++i) {
        AudioQueueBufferRef b = nullptr;
        if (AudioQueueAllocateBuffer(_audioQueue, _chunkBytes, &b) == noErr && b) {
            memset(b->mAudioData, 0, _chunkBytes);
            b->mAudioDataByteSize = _chunkBytes;
            AudioQueueEnqueueBuffer(_audioQueue, b, 0, NULL);
        }
    }

    if (AudioQueueStart(_audioQueue, NULL) == noErr) {
        _started = YES;
    }
}

- (void)handleCallback:(AudioQueueBufferRef)buf {
    if (_disposing.load() || !_audioQueue || !buf) return;

    const UInt32 want = std::min<UInt32>(_chunkBytes, buf->mAudioDataBytesCapacity);
    uint8_t *dst = static_cast<uint8_t *>(buf->mAudioData);
    UInt32 copied = 0;

    if (!_muted) {
        std::lock_guard<std::mutex> lock(_bufMutex);
        copied = std::min<UInt32>(want, static_cast<UInt32>(_pcmRing.size()));
        copied &= ~3u; // keep stereo 16-bit 4-byte frame alignment
        if (copied > 0) {
            std::copy_n(_pcmRing.begin(), copied, dst);
            _pcmRing.erase(_pcmRing.begin(), _pcmRing.begin() + copied);
        }
    }

    if (copied < want) {
        memset(dst + copied, 0, want - copied);
    }
    buf->mAudioDataByteSize = want;

    if (!_disposing.load() && _audioQueue) {
        AudioQueueEnqueueBuffer(_audioQueue, buf, 0, NULL);
    }
}

- (void)playPcm:(const uint8_t *)data length:(size_t)len sampleRate:(uint32_t)sampleRate {
    if (!data || len < 4) return;
    if (sampleRate < 8000 || sampleRate > 192000) sampleRate = 48000;

    // Detect non-silent signal accurately on incoming PCM for live UI status
    const int16_t *samples = reinterpret_cast<const int16_t *>(data);
    size_t count = len / sizeof(int16_t);
    for (size_t i = 0; i < count; i += 8) {
        if (samples[i] > 8 || samples[i] < -8) {
            _lastSignalTime = std::chrono::steady_clock::now();
            break;
        }
    }

    std::lock_guard<std::mutex> qlock(_queueMutex);
    if (_muted) return;

    UInt32 isRun = 0, runSz = sizeof(isRun);
    BOOL queueDead = (!_audioQueue || _sampleRate != sampleRate || !_started ||
                      AudioQueueGetProperty(_audioQueue, kAudioQueueProperty_IsRunning, &isRun, &runSz) != noErr ||
                      isRun == 0);
    if (queueDead) {
        [self configureForSampleRateLocked:sampleRate];
    }
    if (!_audioQueue) return;

    std::lock_guard<std::mutex> block(_bufMutex);
    _pcmRing.insert(_pcmRing.end(), data, data + (len & ~3u));

    // Cap buffer at ~100ms so A/V stays tightly synced on wireless/USB
    const size_t maxBytes = static_cast<size_t>(_sampleRate) * 4 / 10;
    if (_pcmRing.size() > maxBytes) {
        size_t drop = (_pcmRing.size() - maxBytes + 3) & ~3u;
        drop = std::min(drop, _pcmRing.size());
        _pcmRing.erase(_pcmRing.begin(), _pcmRing.begin() + drop);
    }
}

- (void)switchToDeviceUID:(NSString *)uid {
    std::lock_guard<std::mutex> qlock(_queueMutex);
    _muted = NO;
    self.selectedDeviceUID = uid;
    [self configureForSampleRateLocked:_sampleRate];
}

- (void)setMuted:(BOOL)muted {
    std::lock_guard<std::mutex> qlock(_queueMutex);
    if (_muted == muted) return;
    _muted = muted;
    if (muted) {
        [self stopLocked];
    } else {
        [self configureForSampleRateLocked:_sampleRate];
    }
}

- (void)handleDefaultDeviceRouteChange {
    std::lock_guard<std::mutex> qlock(_queueMutex);
    if (_alive.load() && !_muted && _selectedDeviceUID.length == 0 && _audioQueue) {
        [self configureForSampleRateLocked:_sampleRate];
    }
}

- (void)restartAudio {
    std::lock_guard<std::mutex> qlock(_queueMutex);
    [self configureForSampleRateLocked:_sampleRate];
}

- (void)stop {
    _alive = false;
    AudioObjectPropertyAddress defOutAddr = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &defOutAddr, OnCoreAudioRouteChanged, (__bridge void *)self);
    std::lock_guard<std::mutex> qlock(_queueMutex);
    [self stopLocked];
}

- (void)dealloc {
    [self stop];
    [_selectedDeviceUID release];
    _selectedDeviceUID = nil;
    [super dealloc];
}
@end

// ============================================================================
// Hardware VideoToolbox Decoder + AVSampleBufferDisplayLayer HDR View
// ============================================================================
@interface AirPlayVideoView : NSView
@property (nonatomic, strong) AVSampleBufferDisplayLayer *displayLayer;
@property (nonatomic, strong) CALayer *cursorLayer;
- (void)updateCursor:(const CursorPayload *)meta bitmap:(const uint8_t *)bgra size:(size_t)bgraLen;
- (void)setVideoDimensions:(uint32_t)w height:(uint32_t)h hdr:(BOOL)isHdr;
@end

@implementation AirPlayVideoView {
    uint32_t _videoW;
    uint32_t _videoH;
    CursorPayload _lastCursor;
    BOOL _hasCursorMeta;
    CGImageRef _cursorImage;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = [NSColor blackColor].CGColor;

        _displayLayer = [AVSampleBufferDisplayLayer layer];
        _displayLayer.frame = self.bounds;
        _displayLayer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        _displayLayer.videoGravity = AVLayerVideoGravityResizeAspect;
        _displayLayer.backgroundColor = [NSColor blackColor].CGColor;
        _displayLayer.wantsExtendedDynamicRangeContent = YES;
        [self.layer addSublayer:_displayLayer];

        _cursorLayer = [CALayer layer];
        _cursorLayer.hidden = YES;
        _cursorLayer.contentsScale = [[NSScreen mainScreen] backingScaleFactor];
        _cursorLayer.zPosition = 100.0;
        [self.layer addSublayer:_cursorLayer];

        _videoW = 1920;
        _videoH = 1080;
        _hasCursorMeta = NO;
        _cursorImage = nullptr;
    }
    return self;
}

- (void)layout {
    [super layout];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _displayLayer.frame = self.bounds;
    [self repositionCursor];
    [CATransaction commit];
}

- (void)setVideoDimensions:(uint32_t)w height:(uint32_t)h hdr:(BOOL)isHdr {
    if (w > 0 && h > 0) {
        _videoW = w;
        _videoH = h;
    }
    _displayLayer.wantsExtendedDynamicRangeContent = YES;
    [self repositionCursor];
}

- (void)repositionCursor {
    if (!_hasCursorMeta || !_lastCursor.visible || !_cursorImage || _videoW == 0 || _videoH == 0) {
        _cursorLayer.hidden = YES;
        return;
    }

    NSRect bounds = self.bounds;
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return;

    CGFloat videoAspect = static_cast<CGFloat>(_videoW) / static_cast<CGFloat>(_videoH);
    CGFloat viewAspect = bounds.size.width / bounds.size.height;

    NSRect videoRect;
    if (viewAspect > videoAspect) {
        CGFloat h = bounds.size.height;
        CGFloat w = h * videoAspect;
        videoRect = NSMakeRect((bounds.size.width - w) * 0.5, 0, w, h);
    } else {
        CGFloat w = bounds.size.width;
        CGFloat h = w / videoAspect;
        videoRect = NSMakeRect(0, (bounds.size.height - h) * 0.5, w, h);
    }

    CGFloat scaleX = videoRect.size.width / static_cast<CGFloat>(_videoW);
    CGFloat scaleY = videoRect.size.height / static_cast<CGFloat>(_videoH);

    CGFloat curW = _lastCursor.width * scaleX;
    CGFloat curH = _lastCursor.height * scaleY;
    CGFloat hotX = _lastCursor.hotspotX * scaleX;
    CGFloat hotY = _lastCursor.hotspotY * scaleY;

    // Cocoa layer coordinates have Y=0 at bottom
    CGFloat posX = videoRect.origin.x + (_lastCursor.x * scaleX) - hotX;
    CGFloat topY = videoRect.origin.y + videoRect.size.height - (_lastCursor.y * scaleY);
    CGFloat posY = topY - curH + hotY;

    _cursorLayer.frame = NSMakeRect(posX, posY, curW, curH);
    _cursorLayer.hidden = NO;
}

- (void)updateCursor:(const CursorPayload *)meta bitmap:(const uint8_t *)bgra size:(size_t)bgraLen {
    _lastCursor = *meta;
    _hasCursorMeta = YES;
    if (meta->screenW > 0 && meta->screenH > 0) {
        _videoW = meta->screenW;
        _videoH = meta->screenH;
    }

    if (meta->hasBitmap && bgra && bgraLen >= static_cast<size_t>(meta->width) * meta->height * 4) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            const_cast<uint8_t *>(bgra),
            meta->width,
            meta->height,
            8,
            meta->width * 4,
            cs,
            kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst
        );
        CGColorSpaceRelease(cs);

        if (ctx) {
            CGImageRef img = CGBitmapContextCreateImage(ctx);
            CGContextRelease(ctx);
            if (img) {
                if (_cursorImage) CGImageRelease(_cursorImage);
                _cursorImage = img;
            }
        }
    }

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    if (_cursorImage) {
        _cursorLayer.contents = (__bridge id)_cursorImage;
    }
    [self repositionCursor];
    [CATransaction commit];
}

- (void)dealloc {
    if (_cursorImage) CGImageRelease(_cursorImage);
    [super dealloc];
}
@end

// ============================================================================
// VideoToolbox Hardware Decoder Helper (H.264 + HEVC Main/Main10 HDR)
// ============================================================================
class VideoToolboxDecoder {
public:
    ~VideoToolboxDecoder() { cleanup(); }

    void cleanup() {
        if (_session) {
            VTDecompressionSessionWaitForAsynchronousFrames(_session);
            VTDecompressionSessionInvalidate(_session);
            CFRelease(_session);
            _session = nullptr;
        }
        if (_fmtDesc) {
            CFRelease(_fmtDesc);
            _fmtDesc = nullptr;
        }
        _vps.clear();
        _sps.clear();
        _pps.clear();
    }

    static void decompressionCallback(void *decompressionOutputRefCon,
                                      void *sourceFrameRefCon,
                                      OSStatus status,
                                      VTDecodeInfoFlags infoFlags,
                                      CVImageBufferRef imageBuffer,
                                      CMTime presentationTimeStamp,
                                      CMTime presentationDuration) {
        if (status != noErr || !imageBuffer) return;
        CVPixelBufferRef *outRef = static_cast<CVPixelBufferRef *>(sourceFrameRefCon);
        if (outRef) {
            *outRef = CVPixelBufferRetain(imageBuffer);
        }
    }

    bool decodeAndPresent(const uint8_t *annexB, size_t len, VideoCodec codec,
                          ColorProfile colorProfile, AVSampleBufferDisplayLayer *layer) {
        if (!annexB || len < 4 || !layer) return false;

        // Parse Annex-B NALUs (00 00 01 / 00 00 00 01)
        struct NalSpan { const uint8_t *ptr; size_t len; };
        std::vector<NalSpan> vclNals;

        bool paramChanged = false;
        if (codec != _activeCodec) {
            cleanup();
            _activeCodec = codec;
        }

        size_t i = 0;
        auto findStartCode = [&](size_t from, size_t &scLen) -> size_t {
            for (size_t p = from; p + 3 <= len; ++p) {
                if (annexB[p] == 0 && annexB[p + 1] == 0) {
                    if (annexB[p + 2] == 1) { scLen = 3; return p; }
                    if (p + 4 <= len && annexB[p + 2] == 0 && annexB[p + 3] == 1) { scLen = 4; return p; }
                }
            }
            scLen = 0;
            return len;
        };

        size_t scLen = 0;
        size_t pos = findStartCode(0, scLen);
        while (pos < len) {
            size_t nalStart = pos + scLen;
            size_t nextScLen = 0;
            size_t nextPos = findStartCode(nalStart, nextScLen);
            size_t nalLen = nextPos - nalStart;

            if (nalLen > 0) {
                const uint8_t *nal = annexB + nalStart;
                if (codec == CODEC_HEVC) {
                    uint8_t nalType = (nal[0] >> 1) & 0x3F;
                    if (nalType == 32) {
                        if (_vps.size() != nalLen || memcmp(_vps.data(), nal, nalLen) != 0) {
                            _vps.assign(nal, nal + nalLen);
                            paramChanged = true;
                        }
                    } else if (nalType == 33) {
                        if (_sps.size() != nalLen || memcmp(_sps.data(), nal, nalLen) != 0) {
                            _sps.assign(nal, nal + nalLen);
                            paramChanged = true;
                        }
                    } else if (nalType == 34) {
                        if (_pps.size() != nalLen || memcmp(_pps.data(), nal, nalLen) != 0) {
                            _pps.assign(nal, nal + nalLen);
                            paramChanged = true;
                        }
                    } else if (nalType <= 31) {
                        vclNals.push_back({ nal, nalLen });
                    }
                } else {
                    uint8_t nalType = nal[0] & 0x1F;
                    if (nalType == 7) {
                        if (_sps.size() != nalLen || memcmp(_sps.data(), nal, nalLen) != 0) {
                            _sps.assign(nal, nal + nalLen);
                            paramChanged = true;
                        }
                    } else if (nalType == 8) {
                        if (_pps.size() != nalLen || memcmp(_pps.data(), nal, nalLen) != 0) {
                            _pps.assign(nal, nal + nalLen);
                            paramChanged = true;
                        }
                    } else if (nalType == 1 || nalType == 5) {
                        vclNals.push_back({ nal, nalLen });
                    }
                }
            }
            pos = nextPos;
            scLen = nextScLen;
        }

        if (paramChanged || !_session || _activeColor != colorProfile) {
            bool ready = (codec == CODEC_HEVC)
                ? (!_vps.empty() && !_sps.empty() && !_pps.empty())
                : (!_sps.empty() && !_pps.empty());
            if (!ready) return false;

            if (!recreateSession(codec, colorProfile)) return false;
        }

        if (vclNals.empty() || !_session || !_fmtDesc) return false;

        // Pack VCL NALs into 4-byte Big-Endian AVCC/HVCC format
        size_t totalAvcc = 0;
        for (const auto &n : vclNals) totalAvcc += 4 + n.len;

        _avccBuf.resize(totalAvcc);
        size_t offset = 0;
        for (const auto &n : vclNals) {
            uint32_t l = static_cast<uint32_t>(n.len);
            _avccBuf[offset + 0] = (l >> 24) & 0xFF;
            _avccBuf[offset + 1] = (l >> 16) & 0xFF;
            _avccBuf[offset + 2] = (l >> 8) & 0xFF;
            _avccBuf[offset + 3] = l & 0xFF;
            memcpy(_avccBuf.data() + offset + 4, n.ptr, n.len);
            offset += 4 + n.len;
        }

        CMBlockBufferRef blockBuf = nullptr;
        OSStatus status = CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault,
            _avccBuf.data(),
            _avccBuf.size(),
            kCFAllocatorNull,
            nullptr,
            0,
            _avccBuf.size(),
            0,
            &blockBuf
        );
        if (status != kCMBlockBufferNoErr || !blockBuf) return false;

        CMSampleBufferRef encodedSample = nullptr;
        const size_t sampleSize = _avccBuf.size();
        status = CMSampleBufferCreateReady(
            kCFAllocatorDefault,
            blockBuf,
            _fmtDesc,
            1,
            0,
            nullptr,
            1,
            &sampleSize,
            &encodedSample
        );
        CFRelease(blockBuf);
        if (status != noErr || !encodedSample) return false;

        CVPixelBufferRef decodedPixelBuf = nullptr;
        VTDecodeInfoFlags infoFlagsOut = 0;
        status = VTDecompressionSessionDecodeFrame(
            _session,
            encodedSample,
            0, // Synchronous hardware decode on VideoToolbox thread
            &decodedPixelBuf,
            &infoFlagsOut
        );
        CFRelease(encodedSample);

        if (status != noErr || !decodedPixelBuf) {
            if (decodedPixelBuf) CVPixelBufferRelease(decodedPixelBuf);
            return false;
        }

        // Stamp exact color profile on GPU CVPixelBuffer so Mac XDR renders true RGB / BT.2020 PQ HDR
        if (colorProfile == COLOR_HDR10_PQ) {
            CVBufferSetAttachment(decodedPixelBuf, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_2020, kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(decodedPixelBuf, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ, kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(decodedPixelBuf, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_2020, kCVAttachmentMode_ShouldPropagate);
        } else {
            CVBufferSetAttachment(decodedPixelBuf, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(decodedPixelBuf, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
            CVBufferSetAttachment(decodedPixelBuf, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
        }

        CMVideoFormatDescriptionRef pixDesc = nullptr;
        status = CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, decodedPixelBuf, &pixDesc);
        if (status == noErr && pixDesc) {
            CMSampleTimingInfo timing = kCMTimingInfoInvalid;
            CMSampleBufferRef displaySample = nullptr;
            status = CMSampleBufferCreateForImageBuffer(
                kCFAllocatorDefault,
                decodedPixelBuf,
                true,
                nullptr,
                nullptr,
                pixDesc,
                &timing,
                &displaySample
            );
            CFRelease(pixDesc);

            if (status == noErr && displaySample) {
                CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(displaySample, YES);
                if (attachments && CFArrayGetCount(attachments) > 0) {
                    CFMutableDictionaryRef dict = (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
                    CFDictionarySetValue(dict, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
                }

                AVSampleBufferVideoRenderer *renderer = layer.sampleBufferRenderer;
                if (renderer.requiresFlushToResumeDecoding || renderer.status == AVQueuedSampleBufferRenderingStatusFailed) {
                    [renderer flush];
                }
                [renderer enqueueSampleBuffer:displaySample];
                CFRelease(displaySample);
            }
        }

        CVPixelBufferRelease(decodedPixelBuf);
        return true;
    }

private:
    bool recreateSession(VideoCodec codec, ColorProfile color) {
        if (_session) {
            VTDecompressionSessionWaitForAsynchronousFrames(_session);
            VTDecompressionSessionInvalidate(_session);
            CFRelease(_session);
            _session = nullptr;
        }
        if (_fmtDesc) {
            CFRelease(_fmtDesc);
            _fmtDesc = nullptr;
        }

        OSStatus status = noErr;
        if (codec == CODEC_HEVC) {
            const uint8_t *ptrs[3] = { _vps.data(), _sps.data(), _pps.data() };
            const size_t sizes[3] = { _vps.size(), _sps.size(), _pps.size() };
            status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                kCFAllocatorDefault, 3, ptrs, sizes, 4, nullptr, &_fmtDesc);
        } else {
            const uint8_t *ptrs[2] = { _sps.data(), _pps.data() };
            const size_t sizes[2] = { _sps.size(), _pps.size() };
            status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
                kCFAllocatorDefault, 2, ptrs, sizes, 4, &_fmtDesc);
        }

        if (status != noErr || !_fmtDesc) return false;

        OSType pixFmt = (color == COLOR_HDR10_PQ)
            ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
            : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;

        NSDictionary *destAttrs = @{
            (id)kCVPixelBufferPixelFormatTypeKey: @(pixFmt),
            (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
            (id)kCVPixelBufferMetalCompatibilityKey: @YES
        };

        VTDecompressionOutputCallbackRecord cb = {};
        cb.decompressionOutputCallback = decompressionCallback;
        cb.decompressionOutputRefCon = this;

        status = VTDecompressionSessionCreate(
            kCFAllocatorDefault,
            _fmtDesc,
            nullptr,
            (__bridge CFDictionaryRef)destAttrs,
            &cb,
            &_session
        );
        if (status != noErr || !_session) return false;

        VTSessionSetProperty(_session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
        _activeColor = color;
        return true;
    }

    VideoCodec _activeCodec = CODEC_H264;
    ColorProfile _activeColor = COLOR_SDR_BT709;
    CMVideoFormatDescriptionRef _fmtDesc = nullptr;
    VTDecompressionSessionRef _session = nullptr;
    std::vector<uint8_t> _vps, _sps, _pps;
    std::vector<uint8_t> _avccBuf;
};

// ============================================================================
// Bluetooth Discovery + Control Link Bridge + Window Controller
// ============================================================================
@interface MirrorWindowController : NSObject <NSWindowDelegate, NSNetServiceDelegate, IOBluetoothRFCOMMChannelDelegate, NSMenuDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) AirPlayVideoView *videoView;
@property (nonatomic, strong) MacAudioPlayer *audioPlayer;
@property (nonatomic, strong) NSNetService *bonjourService;
@property (nonatomic, strong) IOBluetoothRFCOMMChannel *btChannel;
@property (nonatomic, strong) NSPopUpButton *audioPopup;
@property (nonatomic, strong) NSMenu *topAudioMenu;
@property (nonatomic, strong) NSMutableArray<NSDictionary *> *pcAudioDevices;

@property (nonatomic, assign) NSInteger frameCount;
@property (nonatomic, assign) NSTimeInterval lastFpsTime;
@property (nonatomic, assign) double currentFps;
@property (nonatomic, assign) uint32_t currentWidth;
@property (nonatomic, assign) uint32_t currentHeight;
@property (nonatomic, assign) VideoCodec currentCodec;
@property (nonatomic, assign) ColorProfile currentColor;
@property (nonatomic, assign) double lastRttMs;
@property (nonatomic, assign) BOOL btLinked;
@property (nonatomic, copy) NSString *pcHostName;

- (void)addDiscoveredHost:(NSString *)ip port:(int)port source:(NSString *)src;
- (std::string)pickTargetHost:(int &)outPort;
- (void)updatePcAudioDevices:(NSString *)serialized;
- (int)consumePendingPcAudioSource;
@end

@implementation MirrorWindowController {
    std::mutex _targetMutex;
    std::vector<std::pair<std::string, int>> _discoveredTargets;
    std::string _cliHost;
    int _cliPort;
    BOOL _hasCliHost;
    std::string _lineBuf;
    std::atomic<int> _pendingPcAudioSrc;
}

- (instancetype)initWithCliHost:(NSString *)host port:(int)port {
    self = [super init];
    if (self) {
        _audioPlayer = [[MacAudioPlayer alloc] init];
        _pcAudioDevices = [[NSMutableArray alloc] init];
        _pendingPcAudioSrc = -999;
        _currentWidth = 1920;
        _currentHeight = 1080;
        _currentCodec = CODEC_H264;
        _currentColor = COLOR_SDR_BT709;
        _lastRttMs = 0.0;
        _btLinked = NO;
        _pcHostName = @"Searching (Bluetooth / Wi-Fi / USB)...";

        if (host.length > 0) {
            _cliHost = [host UTF8String];
            _cliPort = port;
            _hasCliHost = YES;
        } else {
            _hasCliHost = NO;
        }

        NSRect screenRect = [[NSScreen mainScreen] visibleFrame];
        NSRect frame = NSMakeRect(screenRect.origin.x + 40, screenRect.origin.y + 40,
                                  screenRect.size.width - 80, screenRect.size.height - 80);

        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        _window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        [_window setTitle:@"WinMirror AirPlay — Discovering PC via Bluetooth & LAN..."];
        [_window setBackgroundColor:[NSColor blackColor]];
        [_window setMinSize:NSMakeSize(800, 450)];
        [_window setDelegate:self];

        _videoView = [[AirPlayVideoView alloc] initWithFrame:[[_window contentView] bounds]];
        [_videoView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [[_window contentView] addSubview:_videoView];

        [self setupAudioSwitchBarAndMenus];

        [_window makeKeyAndOrderFront:nil];
        _lastFpsTime = [NSDate timeIntervalSinceReferenceDate];

        // Publish Bonjour AirPlay-style service on macOS
        _bonjourService = [[NSNetService alloc] initWithDomain:@"local."
                                                          type:@"_winmirror._tcp."
                                                          name:@""
                                                          port:AIRPLAY_PORT];
        _bonjourService.delegate = self;
        [_bonjourService publish];

        // Start native macOS Bluetooth RFCOMM probe against paired Windows PCs
        [self startBluetoothDiscovery];
    }
    return self;
}

- (void)addDiscoveredHost:(NSString *)ip port:(int)port source:(NSString *)src {
    if (!ip.length) return;
    std::string ipStr = [ip UTF8String];
    if (ipStr == "0.0.0.0" || ipStr == "127.0.0.1") return;

    std::lock_guard<std::mutex> lock(_targetMutex);
    // Keep USB/localhost or LAN unique, put lowest-latency first
    for (auto &t : _discoveredTargets) {
        if (t.first == ipStr && t.second == port) return;
    }
    _discoveredTargets.push_back({ ipStr, port });
}

- (std::string)pickTargetHost:(int &)outPort {
    std::lock_guard<std::mutex> lock(_targetMutex);
    if (_hasCliHost) {
        outPort = _cliPort;
        return _cliHost;
    }
    if (!_discoveredTargets.empty()) {
        // Prefer standard private LAN (192.168 / 10.) or USB over virtual VMware/VPN subnets
        for (const auto &t : _discoveredTargets) {
            if (t.first.rfind("192.168.", 0) == 0 || t.first.rfind("10.", 0) == 0) {
                outPort = t.second;
                return t.first;
            }
        }
        outPort = _discoveredTargets.front().second;
        return _discoveredTargets.front().first;
    }
    outPort = AIRPLAY_PORT;
    return "192.168.0.105";
}

- (void)startBluetoothDiscovery {
    static const uint8_t uuidBytes[16] = {
        0x8F, 0x4A, 0x2C, 0x10, 0x7B, 0x3E, 0x4E, 0x91,
        0x9A, 0x02, 0x57, 0x4D, 0x49, 0x52, 0x00, 0x01
    };
    IOBluetoothSDPUUID *targetUuid = [IOBluetoothSDPUUID uuidWithBytes:uuidBytes length:16];

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSArray *paired = [IOBluetoothDevice pairedDevices];
        for (IOBluetoothDevice *dev in paired) {
            if (!dev) continue;
            // Query SDP on paired computers cleanly
            [dev performSDPQuery:nil];
            usleep(600000);

            IOBluetoothSDPServiceRecord *rec = [dev getServiceRecordForUUID:targetUuid];
            if (!rec) continue;

            BluetoothRFCOMMChannelID chanId = 0;
            if ([rec getRFCOMMChannelID:&chanId] == kIOReturnSuccess && chanId > 0) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    IOBluetoothRFCOMMChannel *ch = nil;
                    if ([dev openRFCOMMChannelSync:&ch withChannelID:chanId delegate:self] == kIOReturnSuccess && ch) {
                        self.btChannel = ch;
                        self.btLinked = YES;
                        const char *req = "GET_INFO\n";
                        [ch writeSync:const_cast<char *>(req) length:9];
                        [self refreshTitle];
                    }
                });
                break;
            }
        }
    });
}

- (void)rfcommChannelData:(IOBluetoothRFCOMMChannel *)rfcommChannel data:(void *)dataPointer length:(size_t)dataLength {
    _lineBuf.append(static_cast<const char *>(dataPointer), dataLength);
    size_t nl = 0;
    while ((nl = _lineBuf.find('\n')) != std::string::npos) {
        std::string line = _lineBuf.substr(0, nl);
        _lineBuf.erase(0, nl + 1);
        if (line.rfind("WINMIRROR_BT_HELLO", 0) == 0) {
            NSString *nsLine = [NSString stringWithUTF8String:line.c_str()];
            int port = AIRPLAY_PORT;
            for (NSString *part in [nsLine componentsSeparatedByString:@"|"]) {
                if ([part hasPrefix:@"port="]) {
                    port = [[part substringFromIndex:5] intValue];
                } else if ([part hasPrefix:@"host="]) {
                    self.pcHostName = [part substringFromIndex:5];
                } else if ([part hasPrefix:@"ips="]) {
                    NSString *ips = [part substringFromIndex:4];
                    for (NSString *ip in [ips componentsSeparatedByString:@","]) {
                        [self addDiscoveredHost:ip port:port source:@"Bluetooth"];
                    }
                }
            }
            self.btLinked = YES;
            [self refreshTitle];
        }
    }
}

- (void)rfcommChannelClosed:(IOBluetoothRFCOMMChannel *)rfcommChannel {
    self.btLinked = NO;
    self.btChannel = nil;
    [self refreshTitle];
}

- (void)setupAudioSwitchBarAndMenus {
    // 1. Trailing Titlebar "Switch Audio" control right on top window bar
    NSView *accContainer = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 215, 26)] autorelease];
    _audioPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(4, 2, 204, 22) pullsDown:NO];
    _audioPopup.controlSize = NSControlSizeSmall;
    _audioPopup.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
    _audioPopup.toolTip = @"Switch PC Audio Output Device or Mute (Press M)";
    _audioPopup.menu.delegate = self;

    NSTitlebarAccessoryViewController *accVC = [[[NSTitlebarAccessoryViewController alloc] init] autorelease];
    accVC.view = accContainer;
    accVC.layoutAttribute = NSLayoutAttributeTrailing;
    [accContainer addSubview:_audioPopup];
    [_window addTitlebarAccessoryViewController:accVC];

    // 2. Native macOS Top Menu Bar ("WinMirror AirPlay" + "Audio")
    NSMenu *mainMenu = [[[NSMenu alloc] initWithTitle:@""] autorelease];

    NSMenuItem *appMenuItem = [[[NSMenuItem alloc] initWithTitle:@"WinMirror AirPlay" action:nil keyEquivalent:@""] autorelease];
    NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:@"WinMirror AirPlay"] autorelease];
    NSMenuItem *fsItem = [[[NSMenuItem alloc] initWithTitle:@"Toggle Fullscreen"
                                                    action:@selector(toggleFullscreenAction:)
                                             keyEquivalent:@"f"] autorelease];
    fsItem.target = self;
    [appMenu addItem:fsItem];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quitItem = [[[NSMenuItem alloc] initWithTitle:@"Quit WinMirror AirPlay"
                                                      action:@selector(terminate:)
                                               keyEquivalent:@"q"] autorelease];
    [appMenu addItem:quitItem];
    appMenuItem.submenu = appMenu;
    [mainMenu addItem:appMenuItem];

    NSMenuItem *audioBarItem = [[[NSMenuItem alloc] initWithTitle:@"Switch Audio" action:nil keyEquivalent:@""] autorelease];
    _topAudioMenu = [[NSMenu alloc] initWithTitle:@"Switch Audio"];
    _topAudioMenu.delegate = self;
    audioBarItem.submenu = _topAudioMenu;
    [mainMenu addItem:audioBarItem];

    [NSApp setMainMenu:mainMenu];
    [self refreshAudioSwitchControls];

    // 3. Window keyboard shortcuts (F = Fullscreen, M = Mute Audio)
    __unsafe_unretained MirrorWindowController *weakSelf = self;
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent *(NSEvent *ev) {
        if (ev.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagControl)) {
            return ev;
        }
        NSString *c = [ev.charactersIgnoringModifiers lowercaseString];
        if ([c isEqualToString:@"f"]) {
            [weakSelf.window toggleFullScreen:nil];
            return nil;
        }
        if ([c isEqualToString:@"m"]) {
            [weakSelf toggleAudioMuteAction:nil];
            return nil;
        }
        return ev;
    }];
}

- (void)populateAudioMenu:(NSMenu *)menu {
    [menu removeAllItems];
    BOOL isPopup = (menu == _audioPopup.menu);
    NSString *selUid = _audioPlayer.selectedDeviceUID;
    BOOL isMuted = _audioPlayer.muted;
    std::vector<MacAudioDeviceInfo> devs = enumerateMacOutputDevices();

    NSString *signalTxt = _audioPlayer.hasActiveSignal ? @"Active" : @"Idle/Silent";
    NSString *statusTitle = [NSString stringWithFormat:@"PC Audio (%u Hz) • %@",
                             _audioPlayer.activeSampleRate, signalTxt];
    NSMenuItem *statusItem = [[[NSMenuItem alloc] initWithTitle:statusTitle action:nil keyEquivalent:@""] autorelease];
    statusItem.enabled = NO;
    [menu addItem:statusItem];
    [menu addItem:[NSMenuItem separatorItem]];

    NSMenuItem *muteItem = [[[NSMenuItem alloc] initWithTitle:@"🔇 Mute PC Audio"
                                                        action:@selector(onAudioMenuAction:)
                                                 keyEquivalent:@""] autorelease];
    muteItem.target = self;
    muteItem.tag = -1;
    muteItem.state = isMuted ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:muteItem];

    NSMenuItem *defItem = [[[NSMenuItem alloc] initWithTitle:@"🔊 System Default"
                                                       action:@selector(onAudioMenuAction:)
                                                keyEquivalent:@""] autorelease];
    defItem.target = self;
    defItem.tag = 0;
    defItem.state = (!isMuted && selUid.length == 0) ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:defItem];

    [menu addItem:[NSMenuItem separatorItem]];

    NSInteger selectedIndex = isMuted ? 2 : 3;
    std::vector<std::pair<AudioDeviceID, NSString *>> mutedDevs;

    for (size_t i = 0; i < devs.size(); ++i) {
        const auto &d = devs[i];
        NSMutableString *label = [NSMutableString stringWithFormat:@"🔊 %@", d.name];
        if (d.isDefault) [label appendString:@" ★"];
        if (d.isMuted || (d.volume >= 0.0f && d.volume <= 0.01f)) {
            [label appendString:@" (0% Muted)"];
            mutedDevs.push_back({ d.deviceId, d.name });
        } else if (d.volume > 0.0f) {
            [label appendFormat:@" (%.0f%%)", d.volume * 100.0f];
        }

        NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:label
                                                       action:@selector(onAudioMenuAction:)
                                                keyEquivalent:@""] autorelease];
        item.target = self;
        item.tag = 100 + static_cast<NSInteger>(i);
        item.representedObject = d.uid;
        BOOL match = (!isMuted && selUid.length > 0 && [selUid isEqualToString:d.uid]);
        item.state = match ? NSControlStateValueOn : NSControlStateValueOff;
        if (match) selectedIndex = menu.numberOfItems;
        [menu addItem:item];
    }

    if (_pcAudioDevices.count > 0) {
        auto addPcSection = [&](NSString *sectionTitle, BOOL wantInput) {
            BOOL addedHeader = NO;
            for (NSDictionary *pcDev in _pcAudioDevices) {
                NSInteger idx = [pcDev[@"index"] integerValue];
                NSString *rawName = pcDev[@"name"];
                BOOL isInput = (idx == -2 || idx >= 1000 || [rawName hasPrefix:@"IN:"]);
                if (isInput != wantInput) continue;

                if (!addedHeader) {
                    [menu addItem:[NSMenuItem separatorItem]];
                    NSMenuItem *hdr = [[[NSMenuItem alloc] initWithTitle:sectionTitle action:nil keyEquivalent:@""] autorelease];
                    hdr.enabled = NO;
                    [menu addItem:hdr];
                    addedHeader = YES;
                }

                NSString *cleanName = rawName;
                if ([cleanName hasPrefix:@"OUT:"]) cleanName = [cleanName substringFromIndex:4];
                else if ([cleanName hasPrefix:@"IN:"]) cleanName = [cleanName substringFromIndex:3];

                NSString *pcTitle = [NSString stringWithFormat:@"%@ %@", wantInput ? @"🎤" : @"🖥️", cleanName];
                NSMenuItem *pcItem = [[[NSMenuItem alloc] initWithTitle:pcTitle
                                                                 action:@selector(onPcAudioSourceAction:)
                                                          keyEquivalent:@""] autorelease];
                pcItem.target = self;
                pcItem.tag = idx;
                pcItem.state = [pcDev[@"selected"] boolValue] ? NSControlStateValueOn : NSControlStateValueOff;
                [menu addItem:pcItem];
            }
        };

        addPcSection(@"PC Output Source (Speakers / Loopback)", NO);
        addPcSection(@"PC Input Source (Mic / Line / Virtual)", YES);
    }

    if (!mutedDevs.empty()) {
        [menu addItem:[NSMenuItem separatorItem]];
        for (const auto &md : mutedDevs) {
            NSString *fixTitle = [NSString stringWithFormat:@"⚡ Unmute %@ (60%%)", md.second];
            NSMenuItem *fixItem = [[[NSMenuItem alloc] initWithTitle:fixTitle
                                                              action:@selector(onUnmuteDeviceAction:)
                                                       keyEquivalent:@""] autorelease];
            fixItem.target = self;
            fixItem.tag = static_cast<NSInteger>(md.first);
            [menu addItem:fixItem];
        }
    }

    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *restartItem = [[[NSMenuItem alloc] initWithTitle:@"🔄 Restart Audio Output"
                                                           action:@selector(onAudioMenuAction:)
                                                    keyEquivalent:@""] autorelease];
    restartItem.target = self;
    restartItem.tag = -2;
    [menu addItem:restartItem];

    if (isPopup && selectedIndex < _audioPopup.numberOfItems) {
        [_audioPopup selectItemAtIndex:selectedIndex];
    }
}

- (void)updatePcAudioDevices:(NSString *)serialized {
    [_pcAudioDevices removeAllObjects];
    for (NSString *line in [serialized componentsSeparatedByString:@"\n"]) {
        if (!line.length) continue;
        NSArray<NSString *> *parts = [line componentsSeparatedByString:@"|"];
        if (parts.count >= 3) {
            [_pcAudioDevices addObject:@{
                @"index": @([parts[0] integerValue]),
                @"selected": @([parts[1] integerValue] != 0),
                @"name": parts[2]
            }];
        }
    }
    [self refreshAudioSwitchControls];
}

- (int)consumePendingPcAudioSource {
    return _pendingPcAudioSrc.exchange(-999);
}

- (void)onPcAudioSourceAction:(NSMenuItem *)sender {
    NSInteger chosen = sender.tag;
    _pendingPcAudioSrc.store(static_cast<int>(chosen));
    for (NSUInteger i = 0; i < _pcAudioDevices.count; ++i) {
        NSDictionary *d = _pcAudioDevices[i];
        _pcAudioDevices[i] = @{
            @"index": d[@"index"],
            @"selected": @([d[@"index"] integerValue] == chosen),
            @"name": d[@"name"]
        };
    }
    [self refreshAudioSwitchControls];
}

- (void)refreshAudioSwitchControls {
    [self populateAudioMenu:_audioPopup.menu];
    [self populateAudioMenu:_topAudioMenu];
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    [self populateAudioMenu:menu];
}

- (void)onAudioMenuAction:(NSMenuItem *)sender {
    if (sender.tag == -1) {
        BOOL nextMute = (sender.menu == _topAudioMenu) ? !_audioPlayer.muted : YES;
        [_audioPlayer setMuted:nextMute];
    } else if (sender.tag == -2) {
        [_audioPlayer restartAudio];
    } else if (sender.tag == 0) {
        [_audioPlayer switchToDeviceUID:nil];
    } else if (sender.tag >= 100) {
        NSString *uid = [sender.representedObject isKindOfClass:[NSString class]] ? sender.representedObject : nil;
        [_audioPlayer switchToDeviceUID:uid];
    }
    [self refreshAudioSwitchControls];
    [self refreshTitle];
}

- (void)onUnmuteDeviceAction:(NSMenuItem *)sender {
    unmuteMacDevice(static_cast<AudioDeviceID>(sender.tag), 0.6f);
    [_audioPlayer restartAudio];
    [self refreshAudioSwitchControls];
}

- (void)toggleAudioMuteAction:(id)sender {
    [_audioPlayer setMuted:!_audioPlayer.muted];
    [self refreshAudioSwitchControls];
    [self refreshTitle];
}

- (void)toggleFullscreenAction:(id)sender {
    [_window toggleFullScreen:nil];
}

- (void)refreshTitle {
    NSString *codecStr = (_currentCodec == CODEC_HEVC) ? @"HEVC" : @"H.264";
    NSString *hdrStr = (_currentColor == COLOR_HDR10_PQ) ? @"HDR10" : @"SDR";
    NSString *btStr = _btLinked ? @"BT + LAN" : @"LAN/USB";

    NSString *title = [NSString stringWithFormat:@"WinMirror AirPlay — %@ | %ux%u %@ | %@ %.0f FPS | %.1f ms | %@",
                       _pcHostName, _currentWidth, _currentHeight, hdrStr, codecStr,
                       _currentFps, _lastRttMs, btStr];
    [_window setTitle:title];
}

- (void)updateRenderStats:(uint32_t)w height:(uint32_t)h codec:(VideoCodec)codec color:(ColorProfile)color fps:(double)fps {
    _currentWidth = w;
    _currentHeight = h;
    _currentCodec = codec;
    _currentColor = color;
    _currentFps = fps;
    [self refreshTitle];
}

- (void)keyDown:(NSEvent *)event {
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars isEqualToString:@"f"] || [chars isEqualToString:@"F"]) {
        [self.window toggleFullScreen:nil];
    }
}

- (void)windowWillClose:(NSNotification *)notification {
    [self.audioPlayer stop];
    if (self.btChannel) {
        [self.btChannel closeChannel];
    }
    [self.bonjourService stop];
    exit(0);
}
@end

// ============================================================================
// Network UDP Beacon Listener + Raw AirPlay Media Worker
// ============================================================================
static void udpDiscoveryLoop(MirrorWindowController *controller, std::atomic<bool> *running) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct timeval tv = { 1, 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BEACON_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return;
    }

    char buf[512];
    while (*running) {
        sockaddr_in from = {};
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
        if (n > 0) {
            buf[n] = '\0';
            std::string msg(buf, n);
            if (msg.rfind("WINMIRROR2:", 0) == 0 || msg.rfind("WINMIRROR:", 0) == 0) {
                char ipBuf[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, ipBuf, sizeof(ipBuf));
                int port = AIRPLAY_PORT;
                size_t c1 = msg.find(':');
                size_t c2 = msg.find(':', c1 + 1);
                if (c1 != std::string::npos) {
                    std::string pStr = (c2 != std::string::npos) ? msg.substr(c1 + 1, c2 - c1 - 1) : msg.substr(c1 + 1);
                    if (!pStr.empty()) port = std::atoi(pStr.c_str());
                }
                NSString *nsIp = [NSString stringWithUTF8String:ipBuf];
                dispatch_async(dispatch_get_main_queue(), ^{
                    [controller addDiscoveredHost:nsIp port:port source:@"UDP"];
                });
            }
        }
    }
    close(sock);
}

static bool recvExact(int sock, void *buf, size_t len) {
    uint8_t *p = static_cast<uint8_t *>(buf);
    size_t total = 0;
    while (total < len) {
        ssize_t r = recv(sock, p + total, len - total, 0);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

static void mediaStreamLoop(MirrorWindowController *controller, std::atomic<bool> *running) {
    VideoToolboxDecoder decoder;
    std::vector<uint8_t> payloadBuf;
    payloadBuf.reserve(4 * 1024 * 1024);

    while (*running) {
        int port = AIRPLAY_PORT;
        std::string host = [controller pickTargetHost:port];

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            usleep(300000);
            continue;
        }

        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        int rcvbuf = 4 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        struct timeval tv = { 3, 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        sockaddr_in saddr = {};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, host.c_str(), &saddr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr *>(&saddr), sizeof(saddr)) < 0) {
            close(sock);
            usleep(400000);
            continue;
        }

        auto lastPing = std::chrono::steady_clock::now();
        auto lastFpsTick = lastPing;
        uint32_t decodedFpsFrames = 0;

        while (*running) {
            int reqAudioIdx = [controller consumePendingPcAudioSource];
            if (reqAudioIdx != -999) {
                PacketHeader req = {};
                req.magic = PACKET_MAGIC;
                req.type = PACKET_TYPE_SET_AUDIO_SRC;
                req.extra1 = static_cast<uint32_t>(static_cast<int32_t>(reqAudioIdx));
                send(sock, &req, sizeof(req), 0);
            }

            // Periodic 1Hz RTT ping
            auto now = std::chrono::steady_clock::now();
            if (now - lastPing >= std::chrono::seconds(1)) {
                uint32_t msNow = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
                PacketHeader ping = {};
                ping.magic = PACKET_MAGIC;
                ping.type = PACKET_TYPE_PING;
                ping.extra1 = msNow;
                send(sock, &ping, sizeof(ping), 0);
                lastPing = now;
            }

            PacketHeader hdr = {};
            if (!recvExact(sock, &hdr, sizeof(hdr))) break;
            if (hdr.magic != PACKET_MAGIC || hdr.payloadSize > 24 * 1024 * 1024) break;

            if (hdr.payloadSize > 0) {
                if (payloadBuf.size() < hdr.payloadSize) {
                    payloadBuf.resize(hdr.payloadSize);
                }
                if (!recvExact(sock, payloadBuf.data(), hdr.payloadSize)) break;
            }

            if (hdr.type == PACKET_TYPE_HELLO && hdr.payloadSize >= sizeof(HelloPayload)) {
                HelloPayload hello = {};
                memcpy(&hello, payloadBuf.data(), sizeof(HelloPayload));
                hello.deviceName[63] = '\0';

                NSString *name = [NSString stringWithUTF8String:hello.deviceName];
                uint32_t w = hello.width;
                uint32_t h = hello.height;
                VideoCodec codec = static_cast<VideoCodec>(hello.codec);
                ColorProfile color = static_cast<ColorProfile>(hello.colorProfile);

                dispatch_async(dispatch_get_main_queue(), ^{
                    controller.pcHostName = name.length ? name : [NSString stringWithUTF8String:host.c_str()];
                    controller.currentWidth = w;
                    controller.currentHeight = h;
                    controller.currentCodec = codec;
                    controller.currentColor = color;
                    [controller.videoView setVideoDimensions:w height:h hdr:(color == COLOR_HDR10_PQ)];
                    [controller refreshTitle];
                });
            } else if (hdr.type == PACKET_TYPE_VIDEO && hdr.payloadSize > 0) {
                VideoCodec codec = static_cast<VideoCodec>((hdr.extra1 >> 24) & 0xFF);
                ColorProfile color = static_cast<ColorProfile>((hdr.extra1 >> 16) & 0xFF);
                if (codec != CODEC_H264 && codec != CODEC_HEVC) codec = controller.currentCodec;
                if (color != COLOR_SDR_BT709 && color != COLOR_HDR10_PQ) color = controller.currentColor;

                if (decoder.decodeAndPresent(payloadBuf.data(), hdr.payloadSize, codec, color, controller.videoView.displayLayer)) {
                    decodedFpsFrames++;
                    auto fpsNow = std::chrono::steady_clock::now();
                    double dt = std::chrono::duration<double>(fpsNow - lastFpsTick).count();
                    if (dt >= 0.5) {
                        double fps = decodedFpsFrames / dt;
                        decodedFpsFrames = 0;
                        lastFpsTick = fpsNow;
                        uint32_t w = controller.currentWidth;
                        uint32_t h = controller.currentHeight;
                        dispatch_async(dispatch_get_main_queue(), ^{
                            [controller updateRenderStats:w height:h codec:codec color:color fps:fps];
                        });
                    }
                }
            } else if (hdr.type == PACKET_TYPE_AUDIO && hdr.payloadSize > 0) {
                [controller.audioPlayer playPcm:payloadBuf.data() length:hdr.payloadSize sampleRate:hdr.extra1];
            } else if (hdr.type == PACKET_TYPE_AUDIO_DEVICES && hdr.payloadSize > 0) {
                std::string raw(reinterpret_cast<const char *>(payloadBuf.data()), hdr.payloadSize);
                NSString *nsDevs = [NSString stringWithUTF8String:raw.c_str()];
                if (nsDevs.length > 0) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [controller updatePcAudioDevices:nsDevs];
                    });
                }
            } else if (hdr.type == PACKET_TYPE_CURSOR && hdr.payloadSize >= sizeof(CursorPayload)) {
                CursorPayload meta = {};
                memcpy(&meta, payloadBuf.data(), sizeof(CursorPayload));
                std::vector<uint8_t> bmpCopy;
                if (meta.hasBitmap && hdr.payloadSize > sizeof(CursorPayload)) {
                    bmpCopy.assign(payloadBuf.data() + sizeof(CursorPayload), payloadBuf.data() + hdr.payloadSize);
                }
                dispatch_async(dispatch_get_main_queue(), ^{
                    [controller.videoView updateCursor:&meta
                                                bitmap:(bmpCopy.empty() ? nullptr : bmpCopy.data())
                                                  size:bmpCopy.size()];
                });
            } else if (hdr.type == PACKET_TYPE_PONG) {
                uint32_t msNow = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                double rtt = static_cast<double>(msNow - hdr.extra1);
                if (rtt >= 0.0 && rtt < 5000.0) {
                    dispatch_async(dispatch_get_main_queue(), ^{
                        controller.lastRttMs = rtt;
                    });
                }
            }
        }

        close(sock);
        decoder.cleanup();
        usleep(250000);
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSString *cliHost = (argc > 1) ? [NSString stringWithUTF8String:argv[1]] : nil;
        int port = (argc > 2) ? std::atoi(argv[2]) : AIRPLAY_PORT;

        MirrorWindowController *controller = [[MirrorWindowController alloc] initWithCliHost:cliHost port:port];

        std::atomic<bool> running{true};
        std::thread udpThread(udpDiscoveryLoop, controller, &running);
        std::thread mediaThread(mediaStreamLoop, controller, &running);
        udpThread.detach();
        mediaThread.detach();

        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];

        running = false;
    }
    return 0;
}
