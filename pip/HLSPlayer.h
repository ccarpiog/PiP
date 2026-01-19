#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>

@protocol HLSPlayerDelegate <NSObject>
- (void)hlsPlayerDidUpdateFrame:(CIImage *)image;
- (void)hlsPlayerDidChangeStatus:(AVPlayerItemStatus)status;
- (void)hlsPlayerDidChangeTime:(CMTime)time;
@optional
- (void)hlsPlayerDidEncounterError:(NSError *)error;
- (void)hlsPlayerDidChangeLoadingStatus:(BOOL)isLoading;
- (void)hlsPlayerDidChangePlaybackRate:(float)rate;
- (void)hlsPlayerDidChangeDuration:(CMTime)duration;
- (void)hlsPlayerDidChangeResolution:(CGSize)resolution bitrate:(double)bitrate;
- (void)hlsPlayerDidChangeSeekableRanges:(NSArray<NSValue *> *)seekableTimeRanges;
@end

@interface HLSPlayer : NSObject

@property (nonatomic, weak) id<HLSPlayerDelegate> delegate;
@property (nonatomic, readonly) AVPlayer *player;
@property (nonatomic, readonly) BOOL isPlaying;
@property (nonatomic, readonly) CMTime duration;

- (instancetype)initWithURL:(NSURL *)url;
- (instancetype)initWithURL:(NSURL *)url headers:(NSDictionary<NSString *, NSString *> *)headers;
- (void)play;
- (void)pause;
- (void)stop;
- (void)seekToTime:(CMTime)time;
- (void)setVolume:(float)volume;
- (NSArray<NSDictionary *> *)getAvailableQualities;
- (void)setQuality:(NSDictionary *)quality;
- (NSDictionary *)getCurrentQuality;
- (void)setViewportSize:(CGSize)size;

@end
