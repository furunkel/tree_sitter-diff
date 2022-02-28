#include "tokenizer.h"
#include "ruby.h"

typedef enum {
  STACK_TOKEN_DBL_QUOTE,
  STACK_TOKEN_SNGL_QUOTE,
} StackToken;

typedef enum {
  QUOTE_TYPE_INVALID,
  QUOTE_TYPE_SNGL,
  QUOTE_TYPE_DBL,
  QUOTE_TYPE_BACKTICK,
  QUOTE_TYPE_MAX
} QuoteType;

typedef enum {
  COMMENT_TYPE_INVALID,
  COMMENT_TYPE_DOUBLE_SLASH,
  COMMENT_TYPE_SHARP,
  COMMENT_TYPE_SLASH_STAR,
} CommentType;

#define TOKEN_STACK_CAPA 1024

typedef struct {
  uint8_t stack_data[TOKEN_STACK_CAPA];
  uint8_t *data;
  uint16_t index;
  uint16_t capa;
} TokenStack;

typedef struct {
  uint16_t bracket_type;
  uint16_t prev_bracket_type;
  TokenStack stack;

  // re2c stuff
  uint8_t *padded_input;
  uint8_t *yymarker;
  uint8_t *yyctxmarker;
  uint8_t *yycursor;
  uint8_t *yylimit;

} TokenizerState;

/*
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
*/

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

static void
tokenizer_newline(Tokenizer *tokenizer, TokenizerState *state) {
  if(tokenizer->tokens_len > 0) {
    Token *last_token = &tokenizer->tokens[tokenizer->tokens_len - 1];
    last_token->before_newline = true;
    //fprintf(stderr, "TOKEN NEWLINE: %.*s\n", last_token->end_byte - last_token->start_byte, state->padded_input + last_token->start_byte);
  }
}

static void
tokenizer_add_token_(Tokenizer *tokenizer, TokenizerState *state, Token token) {
  token.end_byte = state->yycursor - state->padded_input;

  //fprintf(stderr, "TOKEN: %.*s (%d-%d)\n", token.end_byte - token.start_byte,
  //        state->padded_input + token.start_byte, token.start_byte, token.end_byte);

  if(token.end_byte <= tokenizer->input_len) {
    tokenizer_add_token(tokenizer, token);
  }
}

/*!max:re2c*/

static void
tokenizer_state_init(TokenizerState *s, Tokenizer *tokenizer) {
  TokenizerState empty_s = {0,};
  *s = empty_s;
  s->stack.capa = TOKEN_STACK_CAPA;
  s->stack.data = s->stack.stack_data;

  s->padded_input = (uint8_t *) xmalloc(tokenizer->input_len + YYMAXFILL);
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
  re2c:define:YYCTYPE = "uint8_t";
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
  DIGITS = [0-9]+;
  FLOAT_LITERAL = DIGITS+ ('.' DIGITS+)? (('E' | 'e') ('+' | '-')? DIGITS+ )? ;
  PUNCT = [!#$%&,\.\?@:;^_|~];
  ALPHA = [a-zA-Z]+;
  ARITH = "+" | "-" | "*" | "/" | "=";
*/

/*!include:re2c "unicode_categories.re" */

static uint8_t
quote_type_to_char(QuoteType t) {
  switch(t) {
    case QUOTE_TYPE_DBL: return '"';
    case QUOTE_TYPE_SNGL: return '\'';
    case QUOTE_TYPE_BACKTICK: return '`';
    case QUOTE_TYPE_INVALID:
    default:
      return '\0';
  }
}

/*!rules:re2c:c_comments

  "//" [^\n\r]* / EOL_WS {
    if(tokenizer->ignore_comments) {
      goto redo;
    } else {
      t.type = TOKEN_TYPE_COMMENT;
      goto end;
      //tokenizer_add_token_(tokenizer, s, t);
      //if(!tokenizer_swallow_comment(tokenizer, s, COMMENT_TYPE_DOUBLE_SLASH)) return false;
      //goto redo;
    }
  }

  "/*" ([^*] | ("*" [^/]))* "*""/" {
    if(tokenizer->ignore_comments) {
      goto redo;
    } else {
      t.type = TOKEN_TYPE_COMMENT;
      goto end;
      //tokenizer_add_token_(tokenizer, s, t);
      //if(!tokenizer_swallow_comment(tokenizer, s, COMMENT_TYPE_SLASH_STAR)) return false;
      //goto redo;
    }
  }
*/

/*!rules:re2c:sharp_comments
  "#" [^\n\r]+ / EOL_WS {
    if(tokenizer->ignore_comments) {
      goto redo;
    } else {
      t.type = TOKEN_TYPE_COMMENT;
      goto end;
      //tokenizer_add_token_(tokenizer, s, t);
      //if(!tokenizer_swallow_comment(tokenizer, s, COMMENT_TYPE_SHARP)) return false;
      //goto redo;
    }
  }
*/


/*!rules:re2c:c_preproc_directive
  '#' [a-z]+ {
    t.type = TOKEN_TYPE_PREPROC_DIRECTIVE;
    goto end;
  }
*/

/*!rules:re2c:underscore_numbers
  DIGITS ("_" DIGITS)+ {
    t.type = TOKEN_TYPE_DIGIT;
    goto end;
  }
*/

/*!rules:re2c:increment_decrement_operator
  "++" | "--" {
    t.type = TOKEN_TYPE_OPERATOR;
    goto end;
  }
*/

/*!rules:re2c:exp_operator
  "**=" | "**" {
    t.type = TOKEN_TYPE_OPERATOR;
    goto end;
  }
*/

/*!rules:re2c:c_identifier
  [a-zA-Z_][a-zA-Z_0-9]* {
    t.type = TOKEN_TYPE_ID;
    goto end;
  }
*/

/*!rules:re2c:compound_logical_assignment
  "||=" | "&&=" {
    t.type = TOKEN_TYPE_OPERATOR;
    goto end;
  }
*/

/*!rules:re2c:general
  [\n\r]+ { 
    tokenizer_newline(tokenizer, s);
    if(tokenizer->ignore_whitespace) {
      goto redo;
    } else {
      t.type = TOKEN_TYPE_LINE;
      goto end;
    }
  }

  [\t ]+ {
    if(tokenizer->ignore_whitespace) {
       goto redo;
    } else {
      t.type = TOKEN_TYPE_SPACE;
      goto end;
    }
  }

  "<" | ">" | ">=" | "<=" | "==" | "!=" { 
    t.type = TOKEN_TYPE_CMP_OPERATOR;
    goto end;
  }

  "+=" | "-=" | "/=" | "*=" | "%=" | "^=" | "|=" | "&=" | "<<=" | ">>=" { 
    t.type = TOKEN_TYPE_OPERATOR;
    goto end;
  }

  "<<" | ">>" | "||" | "&&" { 
    t.type = TOKEN_TYPE_OPERATOR;
    goto end;
  }

  PUNCT {
    t.type = TOKEN_TYPE_PUNCT;
    goto end;
  }

  OPEN_BRACKET {
    t.type = TOKEN_TYPE_BRACKET;
    goto end;
  }

  CLOSED_BRACKET {
    t.type = TOKEN_TYPE_BRACKET;
    goto end;
  }

  ARITH {
    t.type = TOKEN_TYPE_ARITH;
    goto end;
  }

  FLOAT_LITERAL {
    t.type = TOKEN_TYPE_FLOAT_LITERAL;
    goto end;
  }

  DIGITS {
    t.type = TOKEN_TYPE_DIGIT;
    goto end;
  }

  ALPHA {
    t.type = TOKEN_TYPE_ALPHA;
    goto end;
  }

  ["] {
    t.type = TOKEN_TYPE_QUOTE;
    tokenizer_add_token_(tokenizer, s, t);
    if(!tokenizer_swallow_string(tokenizer, s, QUOTE_TYPE_DBL)) return false;
    goto redo;
  }

  ['] {
    t.type = TOKEN_TYPE_QUOTE;
    tokenizer_add_token_(tokenizer, s, t);
    if(!tokenizer_swallow_string(tokenizer, s, QUOTE_TYPE_SNGL)) return false;
    goto redo;
  }

  [`] {
    t.type = TOKEN_TYPE_QUOTE;
    tokenizer_add_token_(tokenizer, s, t);
    if(!tokenizer_swallow_string(tokenizer, s, QUOTE_TYPE_BACKTICK)) return false;
    goto redo;
  }

  [\x00-\x06\a\b\v\f\x0E-\x1F\x7F]+ { 
    t.type = TOKEN_TYPE_OTHER;
    goto end;
  }

  Mn | Mc | Nd | Pc | [\u200D\u05F3] {
    t.type = TOKEN_TYPE_OTHER;
    goto end;
  }  

  * { 
    t.type = TOKEN_TYPE_OTHER;
    goto redo;
  }
*/

#define TOKENIZER_NEXT_FUNC_START(n) \
static bool tokenizer_next_ ## n (Tokenizer *tokenizer, TokenizerState *s) { \
  Token t = {0, }; \
redo: \
  t.start_byte = s->yycursor - s->padded_input;

#define TOKENIZER_NEXT_FUNC_END \
end: \
  tokenizer_add_token_(tokenizer, s, t); \
  return true; \
}


static bool
tokenizer_swallow_comment(Tokenizer *tokenizer, TokenizerState *s, CommentType c) {
  size_t start_byte = s->yycursor - s->padded_input;
  size_t end_token_start_byte = 0;
  while(true) {
    //FIXME: better way do get the start/length of a match?
    end_token_start_byte = s->yycursor - s->padded_input;

  /*!re2c
    "*/" { 
      if(c == COMMENT_TYPE_SLASH_STAR) {
        goto done;
      } else {
        goto end;
      }
    }

    [\n\r] {
      if(c == COMMENT_TYPE_SHARP || c == COMMENT_TYPE_DOUBLE_SLASH) {
        goto done;
      } else {
        goto end;
      }
    }
  */
end:  
  }
done:
  {
    Token t = {0, };
    t.start_byte = start_byte;
    t.end_byte = end_token_start_byte;
    //t.type = TOKEN_TYPE_COMMENT_CONTENT;
    tokenizer_add_token(tokenizer, t);
  }

  {
    Token t = {0, };
    t.start_byte = end_token_start_byte;
    t.end_byte = s->yycursor - s->padded_input;
    //t.type = TOKEN_TYPE_COMMENT_END;
    tokenizer_add_token(tokenizer, t);
  }


  //fprintf(stderr, "LEAVING COMMENT\n");
  return true;
}

static bool
tokenizer_tokenize_comment(Tokenizer *tokenizer, TokenizerState *s, CommentType c) {
  while(true) {
redo:    
    Token t = {0, };
    t.start_byte = s->yycursor - s->padded_input;
    //fprintf(stderr, "INSIDE COMMENT %c\n", *(s->yycursor));

  /*!re2c
    * {
      t.type = TOKEN_TYPE_OTHER;
      goto end;
    }

    [ \t]+ {
      if(tokenizer->ignore_whitespace) {
        goto redo;
      } else {
        t.type = TOKEN_TYPE_SPACE;
        goto end;
      }
    }
    
    "*/" { 
      if(c == COMMENT_TYPE_SLASH_STAR) {
        goto done;
      } else {
        t.type = TOKEN_TYPE_PUNCT;
        goto end;
      }
    }

    [\n\r] {
      if(c == COMMENT_TYPE_SHARP || c == COMMENT_TYPE_DOUBLE_SLASH) {
        goto done;
      } else {
        if(tokenizer->ignore_whitespace) {
          goto redo;
        } else {
          t.type = TOKEN_TYPE_LINE;
          goto end;
        }
      }
    }

    DIGITS {
      t.type = TOKEN_TYPE_DIGIT;
      goto end;
    }

    ALPHA {
      t.type = TOKEN_TYPE_ALPHA;
      goto end;
    }

    PUNCT {
      t.type = TOKEN_TYPE_PUNCT;
      goto end;
    }

    ARITH {
      t.type = TOKEN_TYPE_ARITH;
      goto end;
    }
  */
end:
    tokenizer_add_token_(tokenizer, s, t);
  }
done:
  //fprintf(stderr, "LEAVING COMMENT\n");
  return true;
}


static bool
tokenizer_swallow_string(Tokenizer *tokenizer, TokenizerState *s, QuoteType q) {
  uint8_t quote_char = quote_type_to_char(q);
  size_t start_byte = s->yycursor - s->padded_input;
  size_t last_match_start_byte = 0;
  while(true) {
    uint8_t *cursor = s->yycursor;
    last_match_start_byte = s->yycursor - s->padded_input;
    //fprintf(stderr, "INSIDE STR '%c'|'%c'\n", *cursor, quote_char);

  /*!re2c

    "\\'" | "\\`" | "\\\"" { 
      goto end;
    }

    ['"`] {
      if (*cursor == quote_char) {
        // empty string
        {
          Token t = {0, };
          t.type = TOKEN_TYPE_STR_CONTENT;
          t.start_byte = start_byte;
          t.end_byte = last_match_start_byte;
          tokenizer_add_token(tokenizer, t);
        }

        {
          Token t = {0, };
          t.type = TOKEN_TYPE_QUOTE;
          t.start_byte = last_match_start_byte;
          tokenizer_add_token_(tokenizer, s, t);
        }
        goto done;
      } else {
        goto end;
      }
    }
  */

end:
  }
done:
  //fprintf(stderr, "LEAVING STR %c\n", *(s->yycursor));
  return true;
}


static bool
tokenizer_tokenize_string(Tokenizer *tokenizer, TokenizerState *s, QuoteType q) {
  uint8_t quote_char = quote_type_to_char(q);
  while(true) {
    Token t = {0, };
    uint8_t *cursor = s->yycursor;
    t.start_byte = s->yycursor - s->padded_input;
    //fprintf(stderr, "INSIDE STR '%c'|'%c'\n", *cursor, quote_char);

  /*!re2c

    "\\'" | "\\`" | "\\\"" { 
      t.type = TOKEN_TYPE_QUOTE;
      goto end;
    }

    * {
      if (*cursor == quote_char) {

        // empty string
        if(t.start_byte > 0 && cursor[-1] == quote_char) {
          Token tt = {0, };
          tt.type = TOKEN_TYPE_STR_CONTENT;
          tt.start_byte = t.start_byte - 1;
          tt.end_byte = tt.start_byte;
          tokenizer_add_token(tokenizer, tt);
        }

        t.type = TOKEN_TYPE_QUOTE;
        tokenizer_add_token_(tokenizer, s, t);
        goto done;
      } else {
        t.type = TOKEN_TYPE_OTHER;
        goto end;
      }
    }

    "\\a" | "\\b" | "\\f" | "\\n" | "\\r" | "\\t" | "\\v" | "\\\\" | "\\" [0-7]{1,3} | "\\u" [0-9a-fA-F]{4} | "\\U" [0-9a-fA-F]{8} | "\\x" [0-9a-fA-F]+  {
      t.type = TOKEN_TYPE_ESCAPE_STR;
      goto end;
    }

    DIGITS {
      t.type = TOKEN_TYPE_DIGIT;
      goto end;
    }

    ALPHA {
      t.type = TOKEN_TYPE_ALPHA;
      goto end;
    }

    PUNCT {
      t.type = TOKEN_TYPE_PUNCT;
      goto end;
    }

    ARITH {
      t.type = TOKEN_TYPE_ARITH;
      goto end;
    }

    [ \t]+ {
      t.type = TOKEN_TYPE_SPACE;
      goto end;
    }
  */
end:
    tokenizer_add_token_(tokenizer, s, t);
  }
done:
  //fprintf(stderr, "LEAVING STR %c\n", *(s->yycursor));
  return true;
}


static bool
tokenizer_next(Tokenizer *tokenizer, TokenizerState *s) {

redo:
  Token t = {0, };
  t.start_byte = s->yycursor - s->padded_input;

  /*!re2c
  !use:general;
  */

end:

  tokenizer_add_token_(tokenizer, s, t);
  return true;
}

TOKENIZER_NEXT_FUNC_START(c)
  /*!re2c
  !use:c_comments;
  !use:c_identifier;
  !use:c_preproc_directive;
  !use:increment_decrement_operator;
  !use:general;
  */
TOKENIZER_NEXT_FUNC_END

TOKENIZER_NEXT_FUNC_START(cpp)
  /*!re2c

  DIGITS (['] DIGITS)+ {
    t.type = TOKEN_TYPE_DIGIT;
    goto end;
  }

  !use:c_preproc_directive;
  !use:c_identifier;
  !use:c_comments;
  !use:increment_decrement_operator;
  !use:general;
  */
TOKENIZER_NEXT_FUNC_END

TOKENIZER_NEXT_FUNC_START(java)
  /*!re2c
  !use:c_comments;
  !use:c_identifier;
  !use:increment_decrement_operator;

  "<<<" | ">>>" | "<<<=" | ">>>=" { 
    t.type = TOKEN_TYPE_OPERATOR;
    goto end;
  }

  !use:general;
  */
TOKENIZER_NEXT_FUNC_END

TOKENIZER_NEXT_FUNC_START(javascript)
  /*!re2c
  !use:c_comments;
  !use:compound_logical_assignment;
  !use:increment_decrement_operator;
  !use:exp_operator;

  "!==" | "===" { 
    t.type = TOKEN_TYPE_CMP_OPERATOR;
    goto end;
  }

  !use:c_identifier;
  !use:general;
  */
TOKENIZER_NEXT_FUNC_END

TOKENIZER_NEXT_FUNC_START(python)
  /*!re2c
  !use:underscore_numbers;
  !use:c_identifier;
  !use:sharp_comments;

  !use:exp_operator;

  "//=" | "//" { 
    t.type = TOKEN_TYPE_OPERATOR;
    goto end;
  }

  !use:general;
  */
TOKENIZER_NEXT_FUNC_END

TOKENIZER_NEXT_FUNC_START(ruby)
  /*!re2c
  !use:underscore_numbers;
  RUBY_ID = [a-zA-Z_][a-zA-Z_0-9]*[\?!]?;

  "$\'" {
    t.type = TOKEN_TYPE_RUBY_GLOBAL;
    goto end;
  }

  "===" { 
    t.type = TOKEN_TYPE_CMP_OPERATOR;
    goto end;
  }

  ":" RUBY_ID {
    t.type = TOKEN_TYPE_RUBY_SYMBOL;
    goto end;
  }

  RUBY_ID {
    t.type = TOKEN_TYPE_ID;
    goto end;
  }

  !use:compound_logical_assignment;
  !use:exp_operator;
  
  !use:sharp_comments;
  !use:general;
  */
TOKENIZER_NEXT_FUNC_END

TOKENIZER_NEXT_FUNC_START(php)
  /*!re2c
  !use:c_comments;

  '$' [a-zA-Z_][a-zA-Z_0-9]* {
    t.type = TOKEN_TYPE_ID;
    goto end;
  }

  '<?php' | '<?' | '<?=' | '?>' {
    t.type = TOKEN_TYPE_PHP_TAG;
    goto end;
  }

  '$' [a-zA-Z_][a-zA-Z_0-9]* {
    t.type = TOKEN_TYPE_ID;
    goto end;
  }

  !use:increment_decrement_operator;
  !use:exp_operator;

  "!==" | "===" { 
    t.type = TOKEN_TYPE_CMP_OPERATOR;
    goto end;
  }

  !use:c_identifier;
  !use:general;
  */
TOKENIZER_NEXT_FUNC_END

TOKENIZER_NEXT_FUNC_START(go)
  /*!re2c
  !use:c_comments;
  !use:c_identifier;

  ":=" {
    t.type = TOKEN_TYPE_GO_SHORT_VAR_DECL;
    goto end;
  }

  !use:increment_decrement_operator;
  !use:general;
  */
TOKENIZER_NEXT_FUNC_END


typedef bool (*TokenizerNextFunc)(Tokenizer *, TokenizerState *);

void
tokenizer_run(Tokenizer *tokenizer) {
  TokenizerState s;
  tokenizer_state_init(&s, tokenizer);

  TokenizerNextFunc f;

  switch(tokenizer->language) {
    case TOKENIZER_LANGUAGE_C:
      f = tokenizer_next_c;
      break;
    case TOKENIZER_LANGUAGE_CPP:
      f = tokenizer_next_cpp;
      break;
    case TOKENIZER_LANGUAGE_JAVA:
      f = tokenizer_next_java;
      break;
    case TOKENIZER_LANGUAGE_JAVASCRIPT:
      f = tokenizer_next_javascript;
      break;
    case TOKENIZER_LANGUAGE_PYTHON:
      f = tokenizer_next_python;
      break;
    case TOKENIZER_LANGUAGE_RUBY:
      f = tokenizer_next_ruby;
      break;
    case TOKENIZER_LANGUAGE_PHP:
      f = tokenizer_next_php;
      break;
    case TOKENIZER_LANGUAGE_GO:
      f = tokenizer_next_go;
      break;
    case TOKENIZER_LANGUAGE_INVALID:
    default:
      abort();
      break;
  }
  

  while(f(tokenizer, &s)) {}

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