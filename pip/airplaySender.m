//
//  airplaySender.m
//  pip
//
//  Created by Amit Verma on 25/04/22.
//  Copyright © 2022 boggyb. All rights reserved.
//

#ifndef NO_AIRPLAY

#import "airplaySender.h"
#include "../airplay_sender/discovery.h"
#include <stdbool.h>

@interface AirPlayReceiver ()
@property (nonatomic, readwrite) NSString *name;
@property (nonatomic, readwrite) NSString *host;
@property (nonatomic, readwrite) uint16_t port;
@property (nonatomic, readwrite) NSString *deviceId;
@property (nonatomic, readwrite) uint64_t features;
@property (nonatomic, readwrite) NSString *model;
@property (nonatomic, readwrite) BOOL requiresPassword;
@property (nonatomic, readwrite) BOOL supportsScreenMirroring;
@end

@implementation AirPlayReceiver

- (instancetype)initWithReceiver:(airplay_receiver_t *)receiver {
  self = [super init];
  if (self) {
    _name = [NSString stringWithUTF8String:receiver->name];
    _host = [NSString stringWithUTF8String:receiver->host];
    _port = receiver->port;
    _deviceId = [NSString stringWithUTF8String:receiver->deviceId];
    _features = receiver->features;
    _model = [NSString stringWithUTF8String:receiver->model];
    _requiresPassword = receiver->requiresPassword;
    _supportsScreenMirroring = receiver->supportsScreenMirroring;
  }
  return self;
}

@end

static void discovery_log_callback(void *cls, int level, const char *msg) {
  NSLog(@"%s", msg);
}

static void receiver_callback(airplay_receiver_t *receiver, bool added) {
  AirPlayDiscovery *discovery = [AirPlayDiscovery sharedDiscovery];
  if (!discovery || !discovery.delegate) {
    return;
  }

  AirPlayReceiver *objcReceiver = [[AirPlayReceiver alloc] initWithReceiver:receiver];

  dispatch_async(dispatch_get_main_queue(), ^{
    if (added) {
      [discovery.delegate receiverAdded:objcReceiver];
    } else {
      [discovery.delegate receiverRemoved:objcReceiver];
    }
  });
}

@interface AirPlayDiscovery () {
  NSMutableArray<AirPlayReceiver *> *_receivers;
  dispatch_queue_t _eventProcessingQueue;
  BOOL _shouldStopProcessing;
}
@end

@implementation AirPlayDiscovery

+ (instancetype)sharedDiscovery {
  static AirPlayDiscovery *shared = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    shared = [[AirPlayDiscovery alloc] init];
  });
  return shared;
}

- (instancetype)init {
  self = [super init];
  if (self) {
    _receivers = [[NSMutableArray alloc] init];
    // Create a background queue that won't prevent app termination
    _eventProcessingQueue = dispatch_queue_create("com.pip.airplay.discovery.events", DISPATCH_QUEUE_SERIAL);
    dispatch_set_target_queue(_eventProcessingQueue, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_BACKGROUND, 0));
    _shouldStopProcessing = NO;
  }
  return self;
}

- (void)start {
  airplay_discovery_start(receiver_callback);
  // Set up NSLog callback for logging
  airplay_discovery_set_log_callback(discovery_log_callback, NULL);

  // Process DNS-SD events periodically to ensure callbacks are invoked
  // On macOS, DNS-SD should integrate with run loop, but we'll process events manually
  // to ensure callbacks are called
  _shouldStopProcessing = NO;
  __weak typeof(self) weakSelf = self;
  dispatch_async(_eventProcessingQueue, ^{
    @autoreleasepool {
      while (1) {
        // Get strong reference to avoid race condition
        AirPlayDiscovery *strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_shouldStopProcessing) {
          break;
        }

        // Process DNS-SD events - this will invoke callbacks
        // This will return early if discovery is stopped
        airplay_discovery_process_events();

        // Check flag again - exit immediately if stopped
        strongSelf = weakSelf;
        if (!strongSelf || strongSelf->_shouldStopProcessing) {
          break;
        }

        // Use shorter sleep intervals and check flag more frequently
        for (int i = 0; i < 5; i++) {
          strongSelf = weakSelf;
          if (!strongSelf || strongSelf->_shouldStopProcessing) {
            break;
          }
          usleep(10000); // Sleep 10ms, check 5 times = 50ms total
        }
      }
    }
  });

  // Initial sync to populate receivers list on UI thread
  // After this, delegate callbacks will maintain the list
  dispatch_async(dispatch_get_main_queue(), ^{
    [self updateReceivers];
  });
}

- (void)stop {
  // Signal the event processing thread to stop FIRST
  // This ensures the loop will exit on its next iteration
  _shouldStopProcessing = YES;

  // Stop discovery - this will clean up resources and set running = false
  // This makes airplay_discovery_process_events() return immediately
  airplay_discovery_stop();

  // The background thread will exit on its next loop iteration when it checks the flag
  // Since discovery is stopped, process_events() will return immediately
  // The thread should exit within 50ms (5 * 10ms sleep intervals)
  // We don't wait for it - the app can quit and the thread will finish asynchronously

  [_receivers removeAllObjects];
}

- (void)updateReceivers {
  int count = 0;
  airplay_receiver_t *receivers = airplay_discovery_get_receivers(&count);

  // Build a set of deviceIds from C layer
  NSMutableSet<NSString *> *cDeviceIds = [NSMutableSet set];
  for (int i = 0; i < count; i++) {
    NSString *deviceId = [NSString stringWithUTF8String:receivers[i].deviceId];
    [cDeviceIds addObject:deviceId];
  }

  // Remove receivers that are no longer in C layer
  NSMutableArray *toRemove = [NSMutableArray array];
  for (AirPlayReceiver *receiver in _receivers) {
    if (![cDeviceIds containsObject:receiver.deviceId]) {
      [toRemove addObject:receiver];
    }
  }
  [_receivers removeObjectsInArray:toRemove];

  // Add new receivers from C layer
  for (int i = 0; i < count; i++) {
    NSString *deviceId = [NSString stringWithUTF8String:receivers[i].deviceId];
    BOOL exists = NO;
    for (AirPlayReceiver *existing in _receivers) {
      if ([existing.deviceId isEqualToString:deviceId]) {
        exists = YES;
        break;
      }
    }
    if (!exists) {
      AirPlayReceiver *receiver = [[AirPlayReceiver alloc] initWithReceiver:&receivers[i]];
      [_receivers addObject:receiver];
    }
  }

  if (receivers) {
    free(receivers);
  }
}

- (NSArray<AirPlayReceiver *> *)receivers {
  // Don't automatically update - let delegate callbacks maintain the list
  // Only sync on initial start or when explicitly needed
  return [_receivers copy];
}

- (void)receiverAdded:(AirPlayReceiver *)receiver {
  // Check if receiver already exists by deviceId
  BOOL exists = NO;
  for (AirPlayReceiver *existing in _receivers) {
    if ([existing.deviceId isEqualToString:receiver.deviceId]) {
      exists = YES;
      break;
    }
  }
  if (!exists) {
    [_receivers addObject:receiver];
  }
}

- (void)receiverRemoved:(AirPlayReceiver *)receiver {
  // Remove by deviceId
  NSMutableArray *toRemove = [NSMutableArray array];
  for (AirPlayReceiver *existing in _receivers) {
    if ([existing.deviceId isEqualToString:receiver.deviceId]) {
      [toRemove addObject:existing];
    }
  }
  [_receivers removeObjectsInArray:toRemove];
}

@end

#endif
