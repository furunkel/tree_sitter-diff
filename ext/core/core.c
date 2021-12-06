#include "ruby.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

VALUE rb_mTokdiff;
VALUE rb_cChangeSet;
VALUE rb_cToken;
VALUE rb_eTokdiffError;

static ID id_eq;
static ID id_add;
static ID id_del;

#ifndef MAX
#define MAX(a,b) (((a)<(b))?(b):(a))
#endif

typedef struct {
  uint32_t capa;
  uint32_t len;
  st_data_t *entries;
} IndexValue;

typedef struct {
  uint32_t start_byte;
  uint32_t end_byte;
  uint16_t type;
} Token;

typedef struct {
  VALUE rb_change_set;
  Token token;
} RbToken;

typedef struct {
  uint32_t start_byte;
  uint32_t end_byte;
  const char *input;
} IndexKey;

typedef struct {
  VALUE rb_input;
  Token *tokens;
  uint32_t len;
  uint8_t change_type;
} ChangeSet;

static void token_free(void *ptr)
{
  xfree(ptr);
}

static void token_mark(void *ptr) {
  RbToken *token = (RbToken *) ptr;
  rb_gc_mark(token->rb_change_set);
}

static const rb_data_type_t token_type = {
    .wrap_struct_name = "Token",
    .function = {
        .dmark = token_mark,
        .dfree = token_free,
        .dsize = NULL,
    },
    .data = NULL,
    .flags = RUBY_TYPED_FREE_IMMEDIATELY,
};


static void change_set_free(void *ptr)
{
  ChangeSet *change_set = (ChangeSet *) ptr;
  xfree(change_set->tokens);
  xfree(ptr);
}

static void change_set_mark(void *ptr) {
  ChangeSet *change_set = (ChangeSet *) ptr;
  rb_gc_mark(change_set->rb_input);
}

static const rb_data_type_t change_set_type = {
    .wrap_struct_name = "ChangeSet",
    .function = {
        .dmark = change_set_mark,
        .dfree = change_set_free,
        .dsize = NULL,
    },
    .data = NULL,
    .flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

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
} char_type_t;

typedef enum {
  CHANGE_TYPE_ADD,
  CHANGE_TYPE_DEL,
  CHANGE_TYPE_EQL,
} change_type_t;

static void
tokenize(const char *input, size_t input_len, Token **tokens, size_t *tokens_len, size_t *tokens_capa) {
  uint32_t last_token_pos = 0;
  uint16_t prev_char_type = CHAR_TYPE_INVALID;
  bool flush = false;
  size_t i;
  for(i = 0; i <= input_len; i++) {
    uint16_t char_type = CHAR_TYPE_INVALID;

    if(i == input_len) goto add_token;

    char c = input[i];

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
        char_type = CHAR_TYPE_OTHER;
        break;
      case '\t':
        char_type = CHAR_TYPE_SPACE;
        break;
      case '\n':
        char_type = CHAR_TYPE_LINE;
        break;
      case '\v':
      case '\f':
        char_type = CHAR_TYPE_OTHER;
        break;
      case '\r':
        char_type = CHAR_TYPE_LINE;
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
        char_type = CHAR_TYPE_OTHER;
        break;
      case ' ':
        char_type = CHAR_TYPE_SPACE;
        break;
      case '!':
        char_type = CHAR_TYPE_PUNCT;
        break;
      case '"':
        char_type = CHAR_TYPE_QUOTE;
        break;
      case '#':
      case '$':
      case '%':
      case '&':
        char_type = CHAR_TYPE_PUNCT;
        break;
      case '\'':
        char_type = CHAR_TYPE_QUOTE;
        break;
      case '(':
      case ')':
        char_type = CHAR_TYPE_BRACKET;
        break;
      case '*':
      case '+':
        char_type = CHAR_TYPE_ARITH;
        break;
      case ',':
        char_type = CHAR_TYPE_PUNCT;
        break;
      case '-':
        char_type = CHAR_TYPE_ARITH;
        break;
      case '.':
        char_type = CHAR_TYPE_PUNCT;
        break;
      case '/':
        char_type = CHAR_TYPE_ARITH;
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
        char_type = CHAR_TYPE_DIGIT;
        break;
      case ':':
      case ';':
      case '<':
        char_type = CHAR_TYPE_BRACKET;
        break;
      case '=':
        char_type = CHAR_TYPE_ARITH;
        break;
      case '>':
        char_type = CHAR_TYPE_BRACKET;
        break;
      case '?':
      case '@':
        char_type = CHAR_TYPE_PUNCT;
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
        char_type = CHAR_TYPE_ALPHA;
        break;
      case '[':
        char_type = CHAR_TYPE_BRACKET;
        break;
      case '\\':
        char_type = CHAR_TYPE_PUNCT;
        break;
      case ']':
        char_type = CHAR_TYPE_BRACKET;
        break;
      case '^':
      case '_':
      case '`':
        char_type = CHAR_TYPE_PUNCT;
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
        char_type = CHAR_TYPE_ALPHA;
        break;
      case '{':
        char_type = CHAR_TYPE_BRACKET;
        break;
      case '|':
        char_type = CHAR_TYPE_PUNCT;
        break;
      case '}':
        char_type = CHAR_TYPE_BRACKET;
        break;
      case '~':
        char_type = CHAR_TYPE_PUNCT;
        break;
      case '\x7F':
      default:
        char_type = CHAR_TYPE_INVALID;
        break;
    }

    switch(char_type) {
      case CHAR_TYPE_BRACKET:
      case CHAR_TYPE_QUOTE:
        flush = true;
        break;
      default:
        break;
    }

    if(flush || (prev_char_type != char_type && i > 0)) {
add_token:      
      if (*tokens_len >= *tokens_capa) {
        size_t new_tokens_capa = (*tokens_capa) * 2;
        RB_REALLOC_N(*tokens, Token, new_tokens_capa);
        *tokens_capa = new_tokens_capa;
      }

      Token *token = &(*tokens)[*tokens_len];
      (*tokens_len)++;

      token->start_byte = last_token_pos;
      token->end_byte = i;

      // fprintf(stderr, "TOKEN: %.*s (%d-%d)\n", i - last_token_pos, input + last_token_pos, last_token_pos, i);
      
      token->type = prev_char_type;

      flush = false;
      last_token_pos = i;
    }

    prev_char_type = char_type;
  }
}


static st_index_t
index_key_hash(st_data_t arg) {
  IndexKey *table_entry = (IndexKey *) arg;

  uint32_t start_byte = table_entry->start_byte;
  uint32_t end_byte = table_entry->end_byte;
  const char *str = table_entry->input + start_byte;
  size_t len = end_byte - start_byte;

  //return st_hash(str, len, FNV1_32A_INIT);
  return rb_memhash(str, len); // ^ len;
}

static int
index_key_cmp(st_data_t x, st_data_t y) {
  IndexKey *table_entry_x = (IndexKey *) x;
  IndexKey *table_entry_y = (IndexKey *) y;

  uint32_t start_byte_x = table_entry_x->start_byte;
  uint32_t end_byte_x = table_entry_x->end_byte;

  uint32_t start_byte_y = table_entry_y->start_byte;
  uint32_t end_byte_y = table_entry_y->end_byte;

  uint32_t len_x = end_byte_x - start_byte_x;
  uint32_t len_y = end_byte_y - start_byte_y;

  if(len_x != len_y) {
    return 1;
  } else {
    const char *str_x = table_entry_x->input + start_byte_x;
    const char *str_y = table_entry_y->input + start_byte_y;
    return memcmp(str_x, str_y, len_x);
  }
}

static const struct st_hash_type type_index_key_hash = {
    index_key_cmp,
    index_key_hash,
};

// static IndexListEntry *
// index_list_request_entry(IndexValue *index_list) {
//   if(!(index_list->len < index_list->capa)) {
//     uint32_t new_capa = 2 * index_list->capa;
//     RB_REALLOC_N(index_list->entries, IndexListEntry, new_capa);
//     index_list->capa = new_capa;
//   }
//   IndexListEntry *entry = &index_list->entries[index_list->len];
//   index_list->len++;
//   return entry;
// }


static uint32_t
index_list_append(IndexValue *index_list, st_data_t pair) {
  if(!(index_list->len < index_list->capa)) {
    uint32_t new_capa = 2 * index_list->capa;
    RB_REALLOC_N(index_list->entries, st_data_t, new_capa);
    index_list->capa = new_capa;
  }
  uint32_t index = index_list->len;
  index_list->entries[index] = pair;
  index_list->len++;
  return index;
}

//   if(entry == NULL) {
//       ret_entry = index_list_request_entry(index_list);
//       ret_entry->next = UINT32_MAX;
//       ret_entry->len = 1;
//       ret_entry->data[0] = value;
//   } else {
//     if(!(entry->len < INDEX_LIST_ENTRY_CAPA)) {
//       ret_entry = index_list_request_entry(index_list);
//       size_t entry_index = entry - index_list->entries;
//       ret_entry->next = entry_index;
//       ret_entry->len = 1;
//       ret_entry->data[0] = value;
//     } else {
//       entry->data[entry->len] = value;
//       entry->len++;
//       ret_entry = entry;
//     }
//   }

//   assert(ret_entry->next == UINT32_MAX || ret_entry->next < index_list->len);
//   return ret_entry;
// }

typedef struct {
  IndexValue *index_list;
  uint32_t value;
} UpdateArg;

_Static_assert(sizeof(st_data_t) >= 2 * sizeof(uint32_t));

#define MAKE_PAIR64(f, s) ((((st_data_t)(s)) << 32) | ((st_data_t)(f)))
#define PAIR64_FIRST(p) ((p) & 0xFFFFFFFF)
#define PAIR64_SECOND(p) ((p) >> 32)

static int
update_callback(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
  UpdateArg *update_arg = (UpdateArg *) arg;
  IndexValue *index_list = update_arg->index_list;
  uint32_t insert_value = update_arg->value;

  if(!existing) {
    *value = MAKE_PAIR64(insert_value, UINT32_MAX);
  } else {
    uint32_t inserted_index = index_list_append(index_list, *value);
    *value = MAKE_PAIR64(insert_value, inserted_index);
  }

  return ST_CONTINUE;
}

static VALUE
rb_new_change_set(change_type_t change_type, VALUE rb_input, Token *tokens, size_t start, size_t len)
{
  ChangeSet *change_set = RB_ALLOC(ChangeSet);
  change_set->change_type = change_type;
  change_set->rb_input = rb_input;
  change_set->len = len;
  change_set->tokens = RB_ALLOC_N(Token, len);
  memcpy(change_set->tokens, tokens + start, len * sizeof(Token));
  
  return TypedData_Wrap_Struct(rb_cChangeSet, &change_set_type, change_set);
}

// Loosely based on https://github.com/paulgb/simplediff
static void
token_diff(Token *tokens_old, Token *tokens_new, IndexValue *index_list, IndexKey *table_entries_old, st_table *overlap,
          st_table *_overlap, st_table *old_index_map, VALUE rb_input_old, VALUE rb_input_new,
          uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new, VALUE rb_out_ary, bool output_eq) {

  if(len_old == 0 && len_new == 0) return;

  const char *input_old = RSTRING_PTR(rb_input_old);
  const char *input_new = RSTRING_PTR(rb_input_new);

  uint32_t sub_start_old = start_old;
  uint32_t sub_start_new = start_new;
  uint32_t sub_length = 0;

  for(size_t iold = start_old; iold < start_old + len_old; iold++) {
    Token *token = &tokens_old[iold];

    IndexKey *key = &table_entries_old[iold];
    key->start_byte = token->start_byte;
    key->end_byte = token->end_byte;
    key->input = input_old;

    UpdateArg arg = {
      .index_list = index_list,
      .value = iold,
    };

    st_update(old_index_map, (st_data_t) key, update_callback, (st_data_t) &arg);
  }

  if(len_old > 0) {
    for(size_t inew = start_new; inew < start_new + len_new; inew++) {
      Token *token = &tokens_new[inew];

      assert(_overlap->num_entries == 0);

      IndexKey key = {
        .start_byte = token->start_byte,
        .end_byte = token->end_byte,
        .input = input_new
      };

      st_data_t pair;

      if(st_lookup(old_index_map, (st_data_t)&key, &pair)) {
        while(true) {
          uint32_t value = PAIR64_FIRST(pair);
          uint32_t next_index = PAIR64_SECOND(pair);

          uint32_t iold = value;

          st_data_t prev_sub_len = 0;

          if(iold > start_old) {
            st_lookup(overlap, (st_data_t) (iold - 1), (st_data_t *) &prev_sub_len);
          }

          uint32_t new_sub_len = prev_sub_len + 1;
          if(new_sub_len > sub_length) {
              sub_length = new_sub_len;
              sub_start_old = iold - sub_length + 1;
              sub_start_new = inew - sub_length + 1;
          }
          assert(sub_length <= len_old);
          assert(sub_length <= len_new);
          assert(sub_start_old >= start_old);
          assert(sub_start_new >= start_new);
          st_insert(_overlap, (st_data_t) iold, new_sub_len);

          if(next_index == UINT32_MAX) {
            break;
          } else {
            pair = index_list->entries[next_index];
          }
        }
      }

      {
        st_table *tmp = overlap;
        overlap = _overlap;
        _overlap = tmp;
        st_clear(_overlap);
      }
    }
  }

  if(sub_length == 0) {
    if(len_old > 0) {
      rb_ary_push(rb_out_ary, rb_new_change_set(CHANGE_TYPE_DEL, rb_input_old, tokens_old, start_old, len_old));
    }

    if(len_new > 0) {
      rb_ary_push(rb_out_ary, rb_new_change_set(CHANGE_TYPE_ADD, rb_input_new, tokens_new, start_new, len_new));
    }
  } else {
    st_clear(_overlap);
    st_clear(overlap);
    st_clear(old_index_map);
    index_list->len = 0;

    assert(sub_length <= len_old);
    assert(sub_length <= len_new);
    assert(sub_start_old >= start_old);
    assert(sub_start_new >= start_new);

    token_diff(tokens_old, tokens_new, index_list, table_entries_old, overlap, _overlap, old_index_map, rb_input_old, rb_input_new,
              start_old, sub_start_old - start_old,
              start_new, sub_start_new - start_new,
              rb_out_ary, output_eq);


    if(output_eq) {
      rb_ary_push(rb_out_ary, rb_new_change_set(CHANGE_TYPE_EQL, rb_input_new, tokens_new, sub_start_new, sub_length));
    }

    st_clear(_overlap);
    st_clear(overlap);
    st_clear(old_index_map);
    index_list->len = 0;

    token_diff(tokens_old, tokens_new, index_list, table_entries_old, overlap, _overlap, old_index_map, rb_input_old, rb_input_new,
              sub_start_old + sub_length, (start_old + len_old) - (sub_start_old + sub_length),
              sub_start_new + sub_length, (start_new + len_new) - (sub_start_new + sub_length),
              rb_out_ary, output_eq);

  }
}

static VALUE
rb_new_token(VALUE rb_change_set, Token *token_) {
  RbToken *token = RB_ALLOC(RbToken);
  token->rb_change_set = rb_change_set;
  token->token = *token_;
  return TypedData_Wrap_Struct(rb_cToken, &token_type, token);
}

static VALUE
rb_change_set_aref(VALUE self, VALUE rb_index) {
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  long index = FIX2LONG(rb_index);
  if(index < 0) {
    index = change_set->len + index;
  }
  if(index < 0 || index >= change_set->len) {
    rb_raise(rb_eIndexError, "index %ld outside bounds: %d...%u", index, 0,  (unsigned) change_set->len);
    return Qnil;
  }

  return rb_new_token(self, &change_set->tokens[FIX2INT(rb_index)]);
}

static VALUE
rb_change_set_size(VALUE self) {
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  return LONG2FIX((long) change_set->len);
}

static VALUE
rb_change_set_type(VALUE self) {
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  ID type_id;

  switch(change_set->change_type) {
    case CHANGE_TYPE_ADD:
      type_id = id_add;
      break;
    case CHANGE_TYPE_DEL:
      type_id = id_del;
      break;
    case CHANGE_TYPE_EQL:
      type_id = id_eq;
      break;
    default:
      return Qnil;
  }

  return ID2SYM(type_id);
}

static VALUE
rb_tokdiff_diff_s(VALUE self, VALUE rb_old, VALUE rb_new, VALUE rb_output_eq) {
  Check_Type(rb_old, T_STRING);
  Check_Type(rb_new, T_STRING);

  size_t tokens_capa = 512;
  size_t tokens_len = 0;
  size_t tokens_old_len = 0;
  size_t tokens_new_len = 0;
  Token *tokens = RB_ALLOC_N(Token, tokens_capa);
  tokenize(RSTRING_PTR(rb_old), RSTRING_LEN(rb_old), &tokens, &tokens_len, &tokens_capa);
  bool output_eq = RB_TEST(rb_output_eq);

  tokens_old_len = tokens_len;

  tokenize(RSTRING_PTR(rb_new), RSTRING_LEN(rb_new), &tokens, &tokens_len, &tokens_capa);

  tokens_new_len = tokens_len - tokens_old_len;

  Token *tokens_old = tokens;
  Token *tokens_new = tokens + tokens_old_len;

  IndexKey *table_entries_old = RB_ALLOC_N(IndexKey, tokens_old_len);

  IndexValue index_list;
  index_list.capa = tokens_old_len;
  index_list.len = 0;
  index_list.entries = RB_ALLOC_N(st_data_t, index_list.capa);

  VALUE rb_out_ary = rb_ary_new();

  st_table *overlap = st_init_numtable();
  st_table *_overlap = st_init_numtable();
  st_table *old_index_map = st_init_table(&type_index_key_hash);

  token_diff(tokens_old, tokens_new, &index_list, table_entries_old, overlap, _overlap, old_index_map,
            rb_old, rb_new, 0, tokens_old_len, 0, tokens_new_len, rb_out_ary, output_eq);

  st_free_table(overlap);
  st_free_table(_overlap);
  st_free_table(old_index_map);
  xfree(table_entries_old);
  xfree(index_list.entries);
  xfree(tokens);

  return rb_out_ary;
}

static VALUE
rb_token_byte_range(VALUE self) {
  RbToken *token;
  TypedData_Get_Struct(self, RbToken, &token_type, token);

  uint32_t start_byte = token->token.start_byte;
  uint32_t end_byte = token->token.end_byte;
  return rb_range_new(INT2FIX(start_byte), INT2FIX(end_byte - 1), false);
}

static VALUE
rb_token_text(VALUE self) {
  RbToken *token;
  TypedData_Get_Struct(self, RbToken, &token_type, token);

  uint32_t start_byte = token->token.start_byte;
  uint32_t end_byte = token->token.end_byte;

  ChangeSet *change_set;
  TypedData_Get_Struct(token->rb_change_set, ChangeSet, &change_set_type, change_set);

  VALUE rb_input = change_set->rb_input;
  const char *input = RSTRING_PTR(rb_input);
  size_t input_len = RSTRING_LEN(rb_input);

  if(start_byte == end_byte) {
    return Qnil;
  }

  if(start_byte >= input_len || end_byte > input_len) {
    rb_raise(rb_eTokdiffError, "text range exceeds input length (%d-%d > %zu)", start_byte, end_byte, input_len);
    return Qnil;
  }
  return rb_str_new(input + start_byte, end_byte - start_byte);
}

static VALUE
change_set_enum_length(VALUE rb_change_set, VALUE args, VALUE eobj)
{
  ChangeSet *change_set;
  TypedData_Get_Struct(rb_change_set, ChangeSet, &change_set_type, change_set);
  return UINT2NUM(change_set->len);
}

static VALUE
rb_change_set_each(VALUE self)
{
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);
  RETURN_SIZED_ENUMERATOR(self, 0, 0, change_set_enum_length);

  for(uint32_t i = 0; i < change_set->len; i++) {
    VALUE rb_token = rb_new_token(self, &change_set->tokens[i]);
    rb_yield(rb_token);
  }
  return self;
}

void
Init_core()
{
  id_add = rb_intern("+");
  id_del = rb_intern("-");
  id_eq = rb_intern("=");

  rb_mTokdiff = rb_define_module("Tokdiff");
  rb_eTokdiffError = rb_define_class_under(rb_mTokdiff, "Error", rb_eStandardError);

  rb_define_singleton_method(rb_mTokdiff, "diff", rb_tokdiff_diff_s, 3);

  rb_cChangeSet = rb_define_class_under(rb_mTokdiff, "ChangeSet", rb_cObject);
  rb_cToken = rb_define_class_under(rb_mTokdiff, "Token", rb_cObject);

  rb_define_method(rb_cChangeSet, "[]", rb_change_set_aref, 1);
  rb_define_method(rb_cChangeSet, "size", rb_change_set_size, 0);
  rb_define_method(rb_cChangeSet, "type", rb_change_set_type, 0);

  rb_define_method(rb_cChangeSet, "each", rb_change_set_each, 0);


  rb_define_method(rb_cToken, "byte_range", rb_token_byte_range, 0);
  rb_define_method(rb_cToken, "text", rb_token_text, 0);


}