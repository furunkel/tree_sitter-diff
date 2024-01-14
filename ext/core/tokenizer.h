#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
  CHANGE_TYPE_ADD,
  CHANGE_TYPE_DEL,
  CHANGE_TYPE_EQL,
  CHANGE_TYPE_MOD,
} ChangeType;

typedef struct {
  uint32_t start_byte;
  uint32_t end_byte;
  bool before_newline;
  uint16_t type;
} Token;
