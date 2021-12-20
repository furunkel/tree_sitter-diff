#include "tokenizer.h"
#include "ruby.h"

typedef struct {
  bool data[QUOTE_TYPE_MAX];
} InsideStringAry;

typedef struct {
  uint32_t last_token_pos;
  uint16_t char_type;
  uint16_t prev_char_type;
  uint16_t bracket_type;
  uint16_t prev_bracket_type;
  bool flush;
  InsideStringAry inside_string;
  InsideStringAry prev_inside_string;
} TokenizerState;

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
tokenizer_state_save(TokenizerState *s) {
  s->prev_char_type = s->char_type;
  s->prev_bracket_type = s->bracket_type;
  s->prev_inside_string = s->inside_string;
}

static void
tokenizer_feed_(Tokenizer *tokenizer, TokenizerState *s, size_t i) {
    if(s->flush || (s->prev_char_type != s->char_type && i > 0)) {
      bool inside_string = s->prev_inside_string.data[QUOTE_TYPE_SNGL] || s->prev_inside_string.data[QUOTE_TYPE_DBL];

      bool ignore_token = !inside_string && tokenizer->ignore_whitespace &&
                           (s->prev_char_type == CHAR_TYPE_SPACE || s->prev_char_type == CHAR_TYPE_LINE);

      if(!ignore_token) {
        bool dont_start = (s->prev_char_type == CHAR_TYPE_PUNCT ||
                          (s->prev_char_type == CHAR_TYPE_BRACKET && s->prev_bracket_type == BRACKET_TYPE_CLOSED)) && s->char_type == CHAR_TYPE_LINE;

        Token token = {
          .start_byte = s->last_token_pos,
          .end_byte = i,
          .dont_start = dont_start,
          .type = s->prev_char_type
        };

        tokenizer_add_token(tokenizer, token);
      } 
      s->flush = false;
      s->last_token_pos = i;
    }
    tokenizer_state_save(s);
}

static void
tokenizer_feed(Tokenizer *tokenizer, TokenizerState *s, char c, size_t i) {

  switch(c) {
    case '\x00':
    case '\x01':
    case '\x02':
    case '\x03':
    case '\x04':
    case '\x05':
    case '\x06':
    case '\a':
    case '\b':
      s->char_type = CHAR_TYPE_OTHER;
      break;
      break;
    case '\v':
    case '\f':
      s->char_type = CHAR_TYPE_OTHER;
      break;
    case '\r':
    case '\n':
      s->char_type = CHAR_TYPE_LINE;
      break;
    case '\x0E':
    case '\x0F':
    case '\x10':
    case '\x11':
    case '\x12':
    case '\x13':
    case '\x14':
    case '\x15':
    case '\x16':
    case '\x17':
    case '\x18':
    case '\x19':
    case '\x1A':
    case '\x1B':
    case '\x1C':
    case '\x1D':
    case '\x1E':
    case '\x1F':
      s->char_type = CHAR_TYPE_OTHER;
      break;
    case '\t':
    case ' ':
      s->char_type = CHAR_TYPE_SPACE;
      break;
    case '!':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case '"':
      s->char_type = CHAR_TYPE_QUOTE;
      s->inside_string.data[QUOTE_TYPE_DBL] = !s->inside_string.data[QUOTE_TYPE_DBL];
      break;
    case '#':
    case '$':
    case '%':
    case '&':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case '\'':
      s->char_type = CHAR_TYPE_QUOTE;
      s->inside_string.data[QUOTE_TYPE_SNGL] = !s->inside_string.data[QUOTE_TYPE_SNGL];
      break;
    case '(':
    case '[':
    case '{':
      s->char_type = CHAR_TYPE_BRACKET;
      s->bracket_type = BRACKET_TYPE_OPEN;
      break;
    case ')':
    case ']':
    case '}':
      s->char_type = CHAR_TYPE_BRACKET;
      s->bracket_type = BRACKET_TYPE_CLOSED;
      break;
    case '*':
    case '+':
      s->char_type = CHAR_TYPE_ARITH;
      break;
    case ',':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case '-':
      s->char_type = CHAR_TYPE_ARITH;
      break;
    case '.':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case '/':
      s->char_type = CHAR_TYPE_ARITH;
      break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
      s->char_type = CHAR_TYPE_DIGIT;
      break;
    case '<':
      s->char_type = CHAR_TYPE_BRACKET;
      break;
    case '=':
      s->char_type = CHAR_TYPE_ARITH;
      break;
    case '>':
      s->char_type = CHAR_TYPE_BRACKET;
      break;
    case '?':
    case '@':
    case ':':
    case ';':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case 'A':
    case 'B':
    case 'C':
    case 'D':
    case 'E':
    case 'F':
    case 'G':
    case 'H':
    case 'I':
    case 'J':
    case 'K':
    case 'L':
    case 'M':
    case 'N':
    case 'O':
    case 'P':
    case 'Q':
    case 'R':
    case 'S':
    case 'T':
    case 'U':
    case 'V':
    case 'W':
    case 'X':
    case 'Y':
    case 'Z':
      s->char_type = CHAR_TYPE_ALPHA;
      break;
    case '\\':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case '^':
    case '_':
    case '`':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case 'a':
    case 'b':
    case 'c':
    case 'd':
    case 'e':
    case 'f':
    case 'g':
    case 'h':
    case 'i':
    case 'j':
    case 'k':
    case 'l':
    case 'm':
    case 'n':
    case 'o':
    case 'p':
    case 'q':
    case 'r':
    case 's':
    case 't':
    case 'u':
    case 'v':
    case 'w':
    case 'x':
    case 'y':
    case 'z':
      s->char_type = CHAR_TYPE_ALPHA;
      break;
    case '|':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case '~':
      s->char_type = CHAR_TYPE_PUNCT;
      break;
    case '\x7F':
    default:
      s->char_type = CHAR_TYPE_INVALID;
      break;
  }

  switch(s->char_type) {
    case CHAR_TYPE_BRACKET:
    case CHAR_TYPE_QUOTE:
      s->flush = true;
      break;
    default:
      break;
  }

  tokenizer_feed_(tokenizer, s, i);
}

#define TOKENIZER_LOOKBEHIND 2

void
tokenizer_run(Tokenizer *tokenizer) {
  TokenizerState s = {0,};

  size_t i;
  for(i = 0; i < tokenizer->input_len; i++) {
    char c = tokenizer->input[i];
    tokenizer_feed(tokenizer, &s, c, i);
  }

  {
    tokenizer_feed(tokenizer, &s, '\0', i + 1);
  }
}