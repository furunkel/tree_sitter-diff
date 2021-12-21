#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef enum {
  TOKEN_TYPE_INVALID,
  TOKEN_TYPE_OTHER,
  TOKEN_TYPE_ALPHA,
  TOKEN_TYPE_DIGIT,
  TOKEN_TYPE_BLANK,
  TOKEN_TYPE_SPACE,
  TOKEN_TYPE_LINE,
  TOKEN_TYPE_PUNCT,
  TOKEN_TYPE_QUOTE,
  TOKEN_TYPE_BRACKET,
  TOKEN_TYPE_ARITH,
  TOKEN_TYPE_COMMENT,
} TokenType;

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
  TOKENIZER_LANGUAGE_INVALID,
  TOKENIZER_LANGUAGE_C,
  TOKENIZER_LANGUAGE_CPP,
  TOKENIZER_LANGUAGE_JAVASCRIPT,
  TOKENIZER_LANGUAGE_JAVA,
  TOKENIZER_LANGUAGE_RUBY,
  TOKENIZER_LANGUAGE_PYTHON,
  TOKENIZER_LANGUAGE_PHP,
  TOKENIZER_LANGUAGE_GO,
} TokenizerLanguage;


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
  bool ignore_comments;
  uint8_t language;
} Tokenizer;

void tokenizer_run(Tokenizer *tokenizer);
