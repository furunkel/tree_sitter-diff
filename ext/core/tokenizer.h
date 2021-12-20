#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
  CHAR_TYPE_INVALID,
  CHAR_TYPE_OTHER,
  CHAR_TYPE_ALPHA,
  CHAR_TYPE_DIGIT,
  CHAR_TYPE_BLANK,
  CHAR_TYPE_SPACE,
  CHAR_TYPE_LINE,
  CHAR_TYPE_PUNCT,
  CHAR_TYPE_QUOTE,
  CHAR_TYPE_BRACKET,
  CHAR_TYPE_ARITH,
} CharType;

typedef enum {
  CHANGE_TYPE_ADD,
  CHANGE_TYPE_DEL,
  CHANGE_TYPE_EQL,
  CHANGE_TYPE_MOD,
} ChangeType;

typedef enum {
  BRACKET_TYPE_INVALID,
  BRACKET_TYPE_OPEN,
  BRACKET_TYPE_CLOSED
} BracketType;

typedef enum {
  QUOTE_TYPE_SNGL,
  QUOTE_TYPE_DBL,
  QUOTE_TYPE_MAX
} QuoteType;

typedef struct {
  uint32_t start_byte;
  uint32_t end_byte;
  bool dont_start;
  uint16_t type;
} Token;

typedef struct {
  const char *input;
  size_t input_len;
  Token *tokens;
  size_t tokens_len;
  size_t tokens_capa;
  bool ignore_whitespace;
} Tokenizer;

void tokenizer_run(Tokenizer *tokenizer);
