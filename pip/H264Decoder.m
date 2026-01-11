//
//  H264Decoder.m
//  PiP
//
//  Created by Amit Verma on 25/04/22.
//  Copyright © 2022 boggyb. All rights reserved.
//

#import "H264Decoder.h"

@interface H264Decoder(){
  uint8_t *mSPS, *mPPS;
  long mSPSSize, mPPSSize;
  VTDecompressionSessionRef   mDecodeSession;
  CMFormatDescriptionRef      mFormatDescription;
  bool should_reset;
}
@end

@implementation H264Decoder
- (instancetype)init {
  self = [super init];
  should_reset = false;
  return self;
}

void didDecompress(void *decompressionOutputRefCon, void *sourceFrameRefCon, OSStatus status, VTDecodeInfoFlags infoFlags, CVImageBufferRef pixelBuffer, CMTime presentationTimeStamp, CMTime presentationDuration){
  static int decompress_count = 0;
  decompress_count++;

  if(status != noErr) {
    NSLog(@"didDecompress: [LOOPBACK DEBUG] failed with code: %d, count=%d, sourceFrameRefCon=%p, pixelBuffer=%p, flags=0x%x", (int)status, decompress_count, sourceFrameRefCon, pixelBuffer, (unsigned int)infoFlags);
  } else {
    if (decompress_count == 1 || decompress_count % 30 == 0) {
      NSLog(@"didDecompress: [LOOPBACK DEBUG] succeeded, count=%d, sourceFrameRefCon=%p, pixelBuffer=%p", decompress_count, sourceFrameRefCon, pixelBuffer);
    }
  }
  CVPixelBufferRef *outputPixelBuffer = (CVPixelBufferRef *)sourceFrameRefCon;
  *outputPixelBuffer = CVPixelBufferRetain(pixelBuffer);
}

-(void)decode:(uint8_t*)data withLength:(size_t)length andReturnDecodedData:(ReturnDecodedVideoDataBlock)block{
  static int decode_count = 0;
  decode_count++;

  self.returnDataBlock = block;
  uint32_t nalSize = (uint32_t)(length - 4);
  uint32_t *pNalSize = (uint32_t *)data;
  *pNalSize = CFSwapInt32HostToBig(nalSize);

  CVPixelBufferRef pixelBuffer = NULL;
  int nalType = data[4] & 0x1F;

  if (decode_count == 1 || decode_count % 30 == 0 || nalType == 5) {
    NSLog(@"H264Decoder: [LOOPBACK DEBUG] decode NAL type=%d (IDR=%d), length=%zu, count=%d", nalType, (nalType == 5), length, decode_count);
  }

  switch (nalType){
    case 0x07:
      should_reset = true;
      if(mSPS) free(mSPS);
      mSPSSize = length - 4;
      mSPS = malloc(mSPSSize);
      memcpy(mSPS, data + 4, mSPSSize);
      NSLog(@"H264Decoder: [LOOPBACK DEBUG] received SPS, size=%ld", mSPSSize);
      break;
    case 0x08:
      if(mPPS) free(mPPS);
      mPPSSize = length - 4;
      mPPS = malloc(mPPSSize);
      memcpy(mPPS, data + 4, mPPSSize);
      NSLog(@"H264Decoder: [LOOPBACK DEBUG] received PPS, size=%ld", mPPSSize);
      break;
    case 0x05:
      if(should_reset && mDecodeSession){
        NSLog(@"H264Decoder: [LOOPBACK DEBUG] resetting session, invalidating old session");
        VTDecompressionSessionInvalidate(mDecodeSession);
        CFRelease(mDecodeSession);
        mDecodeSession = NULL;
        NSLog(@"H264Decoder: [LOOPBACK DEBUG] old session invalidated");
      }
      should_reset = false;
      [self initVideoToolBox];
      if (decode_count == 1 || decode_count % 30 == 0) {
        NSLog(@"H264Decoder: [LOOPBACK DEBUG] initialized VideoToolbox for IDR frame");
      }
    default:{
      CMBlockBufferRef blockBuffer = NULL;
      NSLog(@"H264Decoder: [LOOPBACK DEBUG] creating block buffer, data=%p, length=%zu", data, length);
      OSStatus status  = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, data, length, kCFAllocatorNull, NULL, 0, length, 0, &blockBuffer);
      NSLog(@"H264Decoder: [LOOPBACK DEBUG] block buffer created, status=%d, blockBuffer=%p", (int)status, blockBuffer);
      if(status == kCMBlockBufferNoErr) {
        CMSampleBufferRef sampleBuffer = NULL;
        const size_t sampleSizeArray[] = {length};
        status = CMSampleBufferCreateReady(kCFAllocatorDefault, blockBuffer, mFormatDescription, 1, 0, NULL, 1, sampleSizeArray, &sampleBuffer);
        if(status == kCMBlockBufferNoErr && sampleBuffer) {
          NSLog(@"H264Decoder: [LOOPBACK DEBUG] calling VTDecompressionSessionDecodeFrame, session=%p, sampleBuffer=%p, pixelBuffer=%p", mDecodeSession, sampleBuffer, &pixelBuffer);
          OSStatus decodeStatus = VTDecompressionSessionDecodeFrame(mDecodeSession, sampleBuffer, 0, &pixelBuffer, NULL);
          NSLog(@"H264Decoder: [LOOPBACK DEBUG] VTDecompressionSessionDecodeFrame returned status=%d, pixelBuffer=%p", (int)decodeStatus, pixelBuffer);
          if(decodeStatus != noErr) {
            NSLog(@"H264Decoder: [LOOPBACK DEBUG] decode failed status=%d for NAL type=%d", (int)decodeStatus, nalType);
          } else if (decode_count == 1 || decode_count % 30 == 0 || nalType == 5) {
            NSLog(@"H264Decoder: [LOOPBACK DEBUG] decode succeeded for NAL type=%d", nalType);
          }
          CFRelease(sampleBuffer);
          NSLog(@"H264Decoder: [LOOPBACK DEBUG] released sampleBuffer and blockBuffer, data buffer may be freed by caller");
        }
        CFRelease(blockBuffer);
      }
      break;
    }
  }
  if(!pixelBuffer) {
    if (decode_count == 1 || decode_count % 30 == 0) {
      NSLog(@"H264Decoder: [LOOPBACK DEBUG] no pixel buffer returned for NAL type=%d", nalType);
    }
    return;
  }
  self.returnDataBlock(pixelBuffer);
  CVPixelBufferRelease(pixelBuffer);
  if (decode_count == 1 || decode_count % 30 == 0) {
    NSLog(@"H264Decoder: [LOOPBACK DEBUG] decoded frame successfully, pixel buffer returned");
  }
}

-(void)initVideoToolBox{
  if(mDecodeSession) {
    NSLog(@"H264Decoder: [LOOPBACK DEBUG] VideoToolbox session already exists, skipping init");
    return;
  }
  const uint8_t* parameterSetPointers[2] = {mSPS, mPPS};
  const size_t parameterSetSizes[2] = {mSPSSize, mPPSSize};
  OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, parameterSetPointers, parameterSetSizes, 4, &mFormatDescription);
  if(status != noErr){
    NSLog(@"H264Decoder: [LOOPBACK DEBUG] CMVideoFormatDescriptionCreateFromH264ParameterSets failed with code: %d", (int)status);
    return;
  }
  NSLog(@"H264Decoder: [LOOPBACK DEBUG] format description created successfully");
  VTDecompressionOutputCallbackRecord callBackRecord;
  callBackRecord.decompressionOutputCallback = didDecompress;
  callBackRecord.decompressionOutputRefCon = NULL;

  status = VTDecompressionSessionCreate(kCFAllocatorDefault, mFormatDescription, NULL, NULL, &callBackRecord, &mDecodeSession);
  if(status != noErr){//kVTVideoDecoderBadDataErr
    NSLog(@"H264Decoder: [LOOPBACK DEBUG] VTDecompressionSessionCreate failed with code: %d", (int)status);
  } else {
    NSLog(@"H264Decoder: [LOOPBACK DEBUG] VideoToolbox decompression session created successfully, session=%p", mDecodeSession);
  }
}

-(void)destroy{
  if(mDecodeSession) {
    VTDecompressionSessionInvalidate(mDecodeSession);
    CFRelease(mDecodeSession);
    mDecodeSession = NULL;
  }

  if(mFormatDescription) {
    CFRelease(mFormatDescription);
    mFormatDescription = NULL;
  }

  if(mSPS) free(mSPS);
  if(mPPS) free(mPPS);
  mSPS = mPPS = NULL;
  mSPSSize = mPPSSize = 0;
}

@end
