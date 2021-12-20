#include "tokenizer.h"
#include "ruby.h"

typedef enum {
  STACK_TOKEN_DBL_QUOTE,
  STACK_TOKEN_SNGL_QUOTE,
} StackToken;


#define TOKEN_STACK_CAPA 1024

typedef struct {
  uint8_t stack_data[TOKEN_STACK_CAPA];
  uint8_t *data;
  uint16_t index;
  uint16_t capa;
} TokenStack;

typedef struct {
  bool data[QUOTE_TYPE_MAX];
} InsideStringAry;

typedef struct {
  uint16_t bracket_type;
  uint16_t prev_bracket_type;
  InsideStringAry inside_string;
  TokenStack stack;

  // re2c stuff
  char *padded_input;
  char *yymarker;
  char *yyctxmarker;
  char *yycursor;
  char *yylimit;

} TokenizerState;


static void
tokenizer_state_push(TokenizerState *s, StackToken t) {
  TokenStack *st = &s->stack;
  if(st->index >= st->capa) {
    size_t old_capa = st->capa;
    st->capa = st->capa + st->capa;
    if(st->data == st->stack_data) {
      uint8_t *heap_data = ALLOC_N(uint8_t, st->capa);
      MEMCPY(heap_data, st->stack_data, uint8_t, old_capa);
      st->data = heap_data;
    } else {
      REALLOC_N(st->data, uint8_t, st->capa);
    }
  }
  st->data[st->index++] = t;
}

static void
tokenizer_add_token(Tokenizer *tokenizer, Token token) {
    if (tokenizer->tokens_len >= tokenizer->tokens_capa) {
      size_t new_tokens_capa = (tokenizer->tokens_capa) * 2;
      RB_REALLOC_N(tokenizer->tokens, Token, new_tokens_capa);
      tokenizer->tokens_capa = new_tokens_capa;
    }

    tokenizer->tokens[tokenizer->tokens_len] = token;
    tokenizer->tokens_len++;
}

/*!max:re2c*/

static void
tokenizer_state_init(TokenizerState *s, Tokenizer *tokenizer) {
  TokenizerState empty_s = {0,};
  *s = empty_s;
  s->stack.capa = TOKEN_STACK_CAPA;
  s->stack.data = s->stack.stack_data;

  s->padded_input = (char *) xmalloc(tokenizer->input_len + YYMAXFILL);
  memcpy(s->padded_input, tokenizer->input, tokenizer->input_len);
  memset(s->padded_input + tokenizer->input_len, 0, YYMAXFILL);
  s->yycursor = s->padded_input;
  s->yylimit = s->yycursor + tokenizer->input_len + YYMAXFILL;
}

static void
tokenizer_state_destroy(TokenizerState *s) {
  xfree(s->padded_input);
}

static void
tokenizer_state_save(TokenizerState *s) {
}

/*!re2c
  re2c:define:YYCTYPE = "unsigned char";
  re2c:flags:utf-8 = 1;
  re2c:define:YYCURSOR = s->yycursor;
  re2c:define:YYMARKER = s->yymarker;
  re2c:define:YYCTXMARKER = s->yyctxmarker;
  re2c:define:YYLIMIT = s->yylimit;
  re2c:define:YYFILL = "return false;";
  re2c:yyfill:parameter = 0;

  EOL_WS = [ \t]* [\n\r]+;
  OPEN_BRACKET = "(" | "[" | "{" | "<";
  CLOSED_BRACKET = ")" | "]" | "}" | ">";

*/

/*!include:re2c "unicode_categories.re" */

static bool
tokenizer_state_inside_string(TokenizerState *s) {
  return s->inside_string.data[QUOTE_TYPE_SNGL] || s->inside_string.data[QUOTE_TYPE_DBL];
}

static bool
tokenizer_next(Tokenizer *tokenizer, TokenizerState *s) {
  Token t = {0, };

redo:
  t.start_byte = s->yycursor - s->padded_input;

  /*!re2c
  [\n\r]+ { 
    bool inside_string = tokenizer_state_inside_string(s);
    if(!inside_string) {
       goto redo;
    } else {
      t.type = CHAR_TYPE_LINE;
      goto end;
    }
  }

  [\t ]+ {
    bool inside_string = tokenizer_state_inside_string(s);
    if(!inside_string) {
       goto redo;
    } else {
      t.type = CHAR_TYPE_SPACE;
      goto end;
    }
  }

  [;] / EOL_WS+ {
    t.type = CHAR_TYPE_PUNCT;
    t.dont_start = true;
    goto end;
  }

  [!#$%&,\.\?@:;^`_\|~\\]+ {
    t.type = CHAR_TYPE_PUNCT;
    goto end;
  }

  OPEN_BRACKET {
    t.type = CHAR_TYPE_BRACKET;
    goto end;
  }

  CLOSED_BRACKET / EOL_WS {
    t.type = CHAR_TYPE_BRACKET;
    t.dont_start = true;
    goto end;
  }
  CLOSED_BRACKET {
    t.type = CHAR_TYPE_BRACKET;
    goto end;
  }

  "+" | "-" | "*" | "/" | "=" {
    t.type = CHAR_TYPE_ARITH;
    goto end;
  }

  [0-9]+ {
    t.type = CHAR_TYPE_DIGIT;
    goto end;
  }

  [a-zA-Z]+ {
    t.type = CHAR_TYPE_ALPHA;
    goto end;
  }

  ["] {
    t.type = CHAR_TYPE_QUOTE;
    if(!s->inside_string.data[QUOTE_TYPE_SNGL]) {
      s->inside_string.data[QUOTE_TYPE_DBL] = !s->inside_string.data[QUOTE_TYPE_DBL];
    }
    goto end;
  }

  ['] {
    t.type = CHAR_TYPE_QUOTE;
    if(!s->inside_string.data[QUOTE_TYPE_DBL]) {
      s->inside_string.data[QUOTE_TYPE_SNGL] = !s->inside_string.data[QUOTE_TYPE_SNGL];
    }
    goto end;
  }

  [\x00-\x06\a\b\v\f\x0E-\x1F\x7F]+ { 
    t.type = CHAR_TYPE_OTHER;
    goto end;
  }

  * { 
    t.type = CHAR_TYPE_OTHER;
    goto redo;
  }

*/

end:

  t.end_byte = s->yycursor - s->padded_input;
  if(t.end_byte <= tokenizer->input_len) {
    tokenizer_add_token(tokenizer, t);
  }
  return true;
}

void
tokenizer_run(Tokenizer *tokenizer) {
  TokenizerState s;
  tokenizer_state_init(&s, tokenizer);

  while(tokenizer_next(tokenizer, &s)) {}

  // size_t i;
  // for(i = 0; i < tokenizer->input_len; i++) {
  //   char c = tokenizer->input[i];
  //   tokenizer_feed(tokenizer, &s, c, i);
  // }

  // {
  //   tokenizer_feed(tokenizer, &s, '\0', i + 1);
  // }

  tokenizer_state_destroy(&s);
}