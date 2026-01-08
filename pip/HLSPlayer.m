#import "HLSPlayer.h"

@interface HLSPlayer ()
@property (nonatomic, strong) AVPlayer *player;
@property (nonatomic, strong) AVPlayerItem *playerItem;
@property (nonatomic, strong) AVPlayerItemVideoOutput *videoOutput;
@property (nonatomic, strong) NSTimer *frameTimer;
@property (nonatomic, assign) BOOL isPlaying;
@end

@implementation HLSPlayer

- (instancetype)initWithURL:(NSURL *)url {
  return [self initWithURL:url headers:nil];
}

- (instancetype)initWithURL:(NSURL *)url headers:(NSDictionary<NSString *, NSString *> *)headers {
  self = [super init];
  if (self) {
    NSLog(@"[HLSPlayer] url: %@, headers: %@", url, headers);
    AVURLAsset *asset;
    if (headers && headers.count > 0) {
      // Create asset with custom headers
      // AVURLAsset uses AVURLAssetHTTPHeaderFieldsKey to set custom HTTP headers
      NSDictionary *options = @{@"AVURLAssetHTTPHeaderFieldsKey": headers};
      asset = [[AVURLAsset alloc] initWithURL:url options:options];
    } else {
      asset = [[AVURLAsset alloc] initWithURL:url options:nil];
    }

    _playerItem = [AVPlayerItem playerItemWithAsset:asset];
    _player = [AVPlayer playerWithPlayerItem:_playerItem];

    // Setup video output for frame extraction
    NSDictionary *pixBuffAttributes = @{
      (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    _videoOutput = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:pixBuffAttributes];
    _videoOutput.suppressesPlayerRendering = YES;
    [_playerItem addOutput:_videoOutput];

    // Observe player status and other properties
    [_playerItem addObserver:self forKeyPath:@"status" options:NSKeyValueObservingOptionNew context:nil];
    [_playerItem addObserver:self forKeyPath:@"error" options:NSKeyValueObservingOptionNew context:nil];
    [_playerItem addObserver:self forKeyPath:@"loadedTimeRanges" options:NSKeyValueObservingOptionNew context:nil];
    [_playerItem addObserver:self forKeyPath:@"playbackBufferEmpty" options:NSKeyValueObservingOptionNew context:nil];
    [_playerItem addObserver:self forKeyPath:@"playbackLikelyToKeepUp" options:NSKeyValueObservingOptionNew context:nil];
    [_playerItem addObserver:self forKeyPath:@"duration" options:NSKeyValueObservingOptionNew context:nil];

    // Observe playback rate
    [_player addObserver:self forKeyPath:@"rate" options:NSKeyValueObservingOptionNew context:nil];

    // Observe time changes
    __weak typeof(self) weakSelf = self;
    CMTime interval = CMTimeMakeWithSeconds(0.1, NSEC_PER_SEC);
    [_player addPeriodicTimeObserverForInterval:interval queue:dispatch_get_main_queue() usingBlock:^(CMTime time) {
      [weakSelf.delegate hlsPlayerDidChangeTime:time];
    }];

    // Setup timer for frame extraction (60fps for smooth playback)
    _frameTimer = nil;

    _isPlaying = NO;
  }
  return self;
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
  if ([keyPath isEqualToString:@"status"]) {
    AVPlayerItemStatus status = _playerItem.status;
    if ([self.delegate respondsToSelector:@selector(hlsPlayerDidChangeStatus:)]) {
      [self.delegate hlsPlayerDidChangeStatus:status];
    }

    // Report error if status is failed
    if (status == AVPlayerItemStatusFailed && _playerItem.error) {
      if ([self.delegate respondsToSelector:@selector(hlsPlayerDidEncounterError:)]) {
        [self.delegate hlsPlayerDidEncounterError:_playerItem.error];
      }
      NSLog(@"[HLSPlayer] Status failed with error: %@", _playerItem.error);
    }
  } else if ([keyPath isEqualToString:@"error"]) {
    if (_playerItem.error) {
      if ([self.delegate respondsToSelector:@selector(hlsPlayerDidEncounterError:)]) {
        [self.delegate hlsPlayerDidEncounterError:_playerItem.error];
      }
      NSLog(@"[HLSPlayer] Error: %@", _playerItem.error);
    }
  } else if ([keyPath isEqualToString:@"loadedTimeRanges"] ||
             [keyPath isEqualToString:@"playbackBufferEmpty"] ||
             [keyPath isEqualToString:@"playbackLikelyToKeepUp"]) {
    // Report loading status
    BOOL isLoading = _playerItem.playbackBufferEmpty || !_playerItem.playbackLikelyToKeepUp;
    if ([self.delegate respondsToSelector:@selector(hlsPlayerDidChangeLoadingStatus:)]) {
      [self.delegate hlsPlayerDidChangeLoadingStatus:isLoading];
    }
  } else if ([keyPath isEqualToString:@"duration"]) {
    if ([self.delegate respondsToSelector:@selector(hlsPlayerDidChangeDuration:)]) {
      [self.delegate hlsPlayerDidChangeDuration:_playerItem.duration];
    }
  } else if ([keyPath isEqualToString:@"rate"]) {
    if ([self.delegate respondsToSelector:@selector(hlsPlayerDidChangePlaybackRate:)]) {
      [self.delegate hlsPlayerDidChangePlaybackRate:_player.rate];
    }
  }
}

- (void)frameTimerCallback:(NSTimer *)timer {
  CMTime outputItemTime = [self.videoOutput itemTimeForHostTime:CACurrentMediaTime()];

  if ([self.videoOutput hasNewPixelBufferForItemTime:outputItemTime]) {
    CVPixelBufferRef pixelBuffer = [self.videoOutput copyPixelBufferForItemTime:outputItemTime itemTimeForDisplay:NULL];
    if (pixelBuffer) {
      CIImage *image = [CIImage imageWithCVPixelBuffer:pixelBuffer];
      if ([self.delegate respondsToSelector:@selector(hlsPlayerDidUpdateFrame:)]) {
        [self.delegate hlsPlayerDidUpdateFrame:image];
      }
      CVPixelBufferRelease(pixelBuffer);
    }
  }
}

- (void)play {
  [self.player play];
  // Start frame timer at 60fps for smooth playback
  if (!self.frameTimer) {
    self.frameTimer = [NSTimer timerWithTimeInterval:1.0/60.0 target:self selector:@selector(frameTimerCallback:) userInfo:nil repeats:YES];
    [[NSRunLoop mainRunLoop] addTimer:self.frameTimer forMode:NSRunLoopCommonModes];
  }
  self.isPlaying = YES;
}

- (void)pause {
  [self.player pause];
  if (self.frameTimer) {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
  }
  self.isPlaying = NO;
}

- (void)stop {
  [self pause];
  [self.player seekToTime:kCMTimeZero];
}

- (void)seekToTime:(CMTime)time {
  [self.player seekToTime:time toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
}

- (void)setVolume:(float)volume {
  self.player.volume = volume;
}

- (CMTime)duration {
  return self.playerItem.duration;
}

- (void)dealloc {
  [self.playerItem removeObserver:self forKeyPath:@"status"];
  [self.playerItem removeObserver:self forKeyPath:@"error"];
  [self.playerItem removeObserver:self forKeyPath:@"loadedTimeRanges"];
  [self.playerItem removeObserver:self forKeyPath:@"playbackBufferEmpty"];
  [self.playerItem removeObserver:self forKeyPath:@"playbackLikelyToKeepUp"];
  [self.playerItem removeObserver:self forKeyPath:@"duration"];
  [self.player removeObserver:self forKeyPath:@"rate"];
  if (self.frameTimer) {
    [self.frameTimer invalidate];
    self.frameTimer = nil;
  }
  [self.playerItem removeOutput:self.videoOutput];
}

@end
