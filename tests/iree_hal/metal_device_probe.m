#import <Metal/Metal.h>

#include <stddef.h>

bool xmojo_metal_has_default_device(void) {
  return MTLCreateSystemDefaultDevice() != nil;
}

size_t xmojo_metal_device_count(void) {
  NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
  size_t count = devices.count;
  [devices release];
  return count;
}
