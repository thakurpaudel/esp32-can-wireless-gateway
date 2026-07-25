#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t id;
  uint8_t dlc;
  uint8_t data[8];
  bool extended;
  bool remote;
  int64_t timestamp_us;
} gateway_frame_t;

size_t gateway_frame_to_json(const gateway_frame_t *frame, char *buffer,
                             size_t size);
