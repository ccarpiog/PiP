//
//  airplaySender.h
//  pip
//
//  Created by Amit Verma on 25/04/22.
//  Copyright © 2022 boggyb. All rights reserved.
//

#ifndef NO_AIRPLAY

#ifndef airplaySender_h
#define airplaySender_h

#import <Foundation/Foundation.h>

@class AirPlayReceiver;

@protocol AirPlayDiscoveryDelegate <NSObject>
- (void)receiverAdded:(AirPlayReceiver *)receiver;
- (void)receiverRemoved:(AirPlayReceiver *)receiver;
@end

@interface AirPlayReceiver : NSObject
@property (nonatomic, readonly) NSString *name;
@property (nonatomic, readonly) NSString *host;
@property (nonatomic, readonly) uint16_t port;
@property (nonatomic, readonly) NSString *deviceId;
@property (nonatomic, readonly) uint64_t features;
@property (nonatomic, readonly) NSString *model;
@property (nonatomic, readonly) BOOL requiresPassword;
@property (nonatomic, readonly) BOOL supportsScreenMirroring;
@end

@interface AirPlayDiscovery : NSObject
@property (nonatomic, weak) id<AirPlayDiscoveryDelegate> delegate;

+ (instancetype)sharedDiscovery;
- (void)start;
- (void)stop;
- (NSArray<AirPlayReceiver *> *)receivers;
- (void)updateReceivers;  // Sync receivers from C layer
@end

#endif /* airplaySender_h */

#endif
