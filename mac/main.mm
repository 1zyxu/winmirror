#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AudioToolbox/AudioToolbox.h>
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
    PACKET_TYPE_HELLO        = 0x01,
    PACKET_TYPE_VIDEO        = 0x02,
    PACKET_TYPE_AUDIO        = 0x03,
    PACKET_TYPE_CURSOR       = 0x04,
    PACKET_TYPE_PING         = 0x05,
    PACKET_TYPE_PONG         = 0x06,
    PACKET_TYPE_KEYFRAME_REQ = 0x07
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
// Low-Latency Adaptive SampleRate CoreAudio Output
// ============================================================================
#define NUM_AQ_BUFS 12
#define AQ_BUF_SIZE 16384

@interface MacAudioPlayer : NSObject
- (void)playPcm:(const uint8_t *)data length:(size_t)len sampleRate:(uint32_t)sampleRate;
- (void)recycleBuffer:(AudioQueueBufferRef)buf;
- (void)stop;
@end

@implementation MacAudioPlayer {
    AudioQueueRef _audioQueue;
    std::vector<AudioQueueBufferRef> _freeBuffers;
    std::mutex _bufMutex;
    uint32_t _sampleRate;
    BOOL _started;
    int _inFlight;
}

static void AirPlayAudioCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    MacAudioPlayer *player = (__bridge MacAudioPlayer *)inUserData;
    [player recycleBuffer:inBuffer];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _sampleRate = 0;
        _started = NO;
        _inFlight = 0;
    }
    return self;
}

- (void)configureForSampleRate:(uint32_t)sr {
    [self stop];
    if (sr < 8000 || sr > 192000) sr = 48000;
    _sampleRate = sr;

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

    if (status == noErr) {
        std::lock_guard<std::mutex> lock(_bufMutex);
        _freeBuffers.clear();
        _inFlight = 0;
        for (int i = 0; i < NUM_AQ_BUFS; ++i) {
            AudioQueueBufferRef b = nullptr;
            if (AudioQueueAllocateBuffer(_audioQueue, AQ_BUF_SIZE, &b) == noErr) {
                _freeBuffers.push_back(b);
            }
        }
    }
}

- (void)recycleBuffer:(AudioQueueBufferRef)buf {
    std::lock_guard<std::mutex> lock(_bufMutex);
    _freeBuffers.push_back(buf);
    if (_inFlight > 0) _inFlight--;
}

- (void)playPcm:(const uint8_t *)data length:(size_t)len sampleRate:(uint32_t)sampleRate {
    if (len == 0) return;
    if (!_audioQueue || _sampleRate != sampleRate) {
        [self configureForSampleRate:sampleRate];
    }
    if (!_audioQueue) return;

    AudioQueueBufferRef buf = nullptr;
    {
        std::lock_guard<std::mutex> lock(_bufMutex);
        // Anti-drift latency cap: drop excess packets if >= 8 queued (~80ms)
        if (_inFlight >= 8 || _freeBuffers.empty()) {
            return;
        }
        buf = _freeBuffers.back();
        _freeBuffers.pop_back();
        _inFlight++;
    }

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
        _started = NO;
    }
    std::lock_guard<std::mutex> lock(_bufMutex);
    _freeBuffers.clear();
    _inFlight = 0;
}

- (void)dealloc {
    [self stop];
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
@interface MirrorWindowController : NSObject <NSWindowDelegate, NSNetServiceDelegate, IOBluetoothRFCOMMChannelDelegate>
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) AirPlayVideoView *videoView;
@property (nonatomic, strong) MacAudioPlayer *audioPlayer;
@property (nonatomic, strong) NSNetService *bonjourService;
@property (nonatomic, strong) IOBluetoothRFCOMMChannel *btChannel;

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
@end

@implementation MirrorWindowController {
    std::mutex _targetMutex;
    std::vector<std::pair<std::string, int>> _discoveredTargets;
    std::string _cliHost;
    int _cliPort;
    BOOL _hasCliHost;
    std::string _lineBuf;
}

- (instancetype)initWithCliHost:(NSString *)host port:(int)port {
    self = [super init];
    if (self) {
        _audioPlayer = [[MacAudioPlayer alloc] init];
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

- (void)refreshTitle {
    NSString *codecStr = (_currentCodec == CODEC_HEVC) ? @"HEVC" : @"H.264";
    NSString *hdrStr = (_currentColor == COLOR_HDR10_PQ) ? @"HDR10" : @"SDR";
    NSString *btStr = _btLinked ? @"BT + LAN" : @"LAN/USB";

    NSString *title = [NSString stringWithFormat:@"WinMirror AirPlay — %@ | %ux%u %@ | %@ %.0f FPS | %.1f ms | %@",
                       _pcHostName, _currentWidth, _currentHeight, hdrStr, codecStr,
                       _currentFps, _lastRttMs, btStr];
    [_window setTitle:title];
}

- (void)noteFrameRendered:(uint32_t)w height:(uint32_t)h codec:(VideoCodec)codec color:(ColorProfile)color {
    _currentWidth = w;
    _currentHeight = h;
    _currentCodec = codec;
    _currentColor = color;

    self.frameCount++;
    NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
    if (now - self.lastFpsTime >= 0.5) {
        self.currentFps = self.frameCount / (now - self.lastFpsTime);
        self.frameCount = 0;
        self.lastFpsTime = now;
        [self refreshTitle];
    }
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

        while (*running) {
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
                    uint32_t w = controller.currentWidth;
                    uint32_t h = controller.currentHeight;
                    dispatch_async(dispatch_get_main_queue(), ^{
                        [controller noteFrameRendered:w height:h codec:codec color:color];
                    });
                }
            } else if (hdr.type == PACKET_TYPE_AUDIO && hdr.payloadSize > 0) {
                [controller.audioPlayer playPcm:payloadBuf.data() length:hdr.payloadSize sampleRate:hdr.extra1];
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
