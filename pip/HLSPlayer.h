#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>

@protocol HLSPlayerDelegate <NSObject>
- (void)hlsPlayerDidUpdateFrame:(CIImage *)image;
- (void)hlsPlayerDidChangeStatus:(AVPlayerItemStatus)status;
- (void)hlsPlayerDidChangeTime:(CMTime)time;
@end

@interface HLSPlayer : NSObject

@property (nonatomic, weak) id<HLSPlayerDelegate> delegate;
@property (nonatomic, readonly) AVPlayer *player;
@property (nonatomic, readonly) BOOL isPlaying;
@property (nonatomic, readonly) CMTime duration;

- (instancetype)initWithURL:(NSURL *)url;
- (void)play;
- (void)pause;
- (void)stop;
- (void)seekToTime:(CMTime)time;
- (void)setVolume:(float)volume;

@end
