#include "ruby.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "tokenizer.h"

VALUE rb_mTokdiff;
VALUE rb_cChangeSet;
VALUE rb_cToken;
VALUE rb_eTokdiffError;

static ID id_eq;
static ID id_add;
static ID id_del;
static ID id_mod;
static ID id_c;
static ID id_cpp;
static ID id_java;
static ID id_javascript;
static ID id_python;
static ID id_ruby;
static ID id_php;
static ID id_go;


#ifndef MAX
#define MAX(a,b) (((a)<(b))?(b):(a))
#endif

#ifndef MIN
#define MIN(a,b) (((a)>(b))?(b):(a))
#endif

typedef struct {
  uint32_t capa;
  uint32_t len;
  st_data_t *entries;
} IndexValue;

typedef struct {
  VALUE rb_input;
  Token token;
} RbToken;

typedef struct {
  uint32_t start_byte;
  uint32_t end_byte;
  const char *input;
} IndexKey;

typedef struct {
  VALUE rb_old_input;
  VALUE rb_new_input;
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
  rb_gc_mark(token->rb_input);
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
  rb_gc_mark(change_set->rb_old_input);
  rb_gc_mark(change_set->rb_old_input);
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

static bool
token_eql(Token *x, const char *input_x, Token *y, const char *input_y) {
  uint32_t start_byte_x = x->start_byte;
  uint32_t end_byte_x = x->end_byte;

  uint32_t start_byte_y = y->start_byte;
  uint32_t end_byte_y = y->end_byte;

  uint32_t len_x = end_byte_x - start_byte_x;
  uint32_t len_y = end_byte_y - start_byte_y;

  if(len_x != len_y) {
    return false;
  } else {
    const char *str_x = input_x + start_byte_x;
    const char *str_y = input_y + start_byte_y;
    return !memcmp(str_x, str_y, len_x);
  }
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

typedef struct {
  uint32_t *data;
  uint32_t min_index;
  uint32_t max_index;
} IntIntMap;

static void
int_int_map_init(IntIntMap *map, uint32_t capa) {
  map->data = RB_ZALLOC_N(uint32_t, capa);
  map->min_index = capa;
  map->max_index = 0;
}

static void
int_int_map_destroy(IntIntMap *map) {
  xfree(map->data);
}

static void
int_int_map_insert(IntIntMap *map, uint32_t index, uint32_t value) {
  map->data[index] = value;
  map->max_index = MAX(map->max_index, index);
  map->min_index = MIN(map->min_index, index);
}

static void
int_int_map_reset(IntIntMap *map) {
  if(map->min_index <= map->max_index) {
     memset(map->data + map->min_index, 0, (map->max_index - map->min_index + 1) * sizeof(uint32_t));
  }
  // memset(map->data, 0, map->max_index * sizeof(uint32_t));
}

static VALUE
rb_change_set_new_full(ChangeType change_type, VALUE rb_old_input, VALUE rb_new_input,
                       Token *old_tokens, size_t old_start, size_t old_len,
                       Token *new_tokens, size_t new_start, size_t new_len)
{
  ChangeSet *change_set = RB_ALLOC(ChangeSet);
  change_set->change_type = change_type;
  change_set->rb_old_input = rb_old_input;
  change_set->rb_new_input = rb_new_input;
  change_set->len = MAX(old_len, new_len);

  assert(old_len == 0 || new_len == 0 || old_len == new_len);
  change_set->tokens = RB_ALLOC_N(Token, old_len + new_len);

  memcpy(change_set->tokens, old_tokens + old_start, old_len * sizeof(Token));
  memcpy(change_set->tokens + old_len, new_tokens + new_start, new_len * sizeof(Token));
  
  return TypedData_Wrap_Struct(rb_cChangeSet, &change_set_type, change_set);
}

static VALUE
rb_change_set_new(ChangeType change_type, VALUE rb_input,
                  Token *tokens, size_t start, size_t len)
{
  switch(change_type) {
    case CHANGE_TYPE_DEL:
      return rb_change_set_new_full(change_type, rb_input, Qnil,
                        tokens, start, len,
                        NULL, 0, 0);
    case CHANGE_TYPE_ADD:
      return rb_change_set_new_full(change_type, Qnil, rb_input,
                        NULL, 0, 0,
                        tokens, start, len);
    default:
      return Qnil;                        
  }
}


// Loosely based on https://github.com/paulgb/simplediff
static void
token_diff(Token *tokens_old, Token *tokens_new, IndexValue *index_list, IndexKey *table_entries_old, IntIntMap *overlap,
          IntIntMap *_overlap, st_table *old_index_map, VALUE rb_input_old, VALUE rb_input_new,
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

  // for(size_t i = 0; i < tokens_new_len; i++) {
  //   Token *token = &tokens_new[i];
  //   fprintf(stderr, "TOKEN OLD: %.*s\n", token->end_byte - token->start_byte, input_new + token->start_byte);
  //   if(token->before_newline) {
  //     fprintf(stderr, "BEFORE NEWLINE\n");
  //   }
  // }

  if(len_old > 0) {
    for(size_t inew = start_new; inew < start_new + len_new; inew++) {
      Token *token = &tokens_new[inew];

    //  fprintf(stderr, "TOKEN NEW: %.*s %d\n", token->end_byte - token->start_byte, input_new + token->start_byte, inew);
      // if(token->before_newline) {
      //    fprintf(stderr, "HAVE FOUND NEWLINE TOKEN\n");
      //    abort();
      // }

      // assert(_overlap->num_entries == 0);

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

          uint32_t prev_sub_len = 0;

          if(iold > start_old) {
            prev_sub_len = overlap->data[iold - 1];
            // st_lookup(overlap, (st_data_t) (iold - 1), (st_data_t *) &prev_sub_len);
          }

          /*if(!(prev_sub_len == 0 && token->dont_start))*/ {
            uint32_t new_sub_len = prev_sub_len + 1;
            uint32_t new_sub_start_old = iold - new_sub_len + 1;
            uint32_t new_sub_start_new = inew - new_sub_len + 1;

            bool new_sub_ends_at_newline = false;
            bool cur_sub_ends_at_newline = false;

            if(sub_length > 0) {
              int64_t cur_sub_last_token_index = sub_start_new + sub_length - 1;
              int64_t new_sub_last_token_index = new_sub_start_new + new_sub_len - 1;

              new_sub_ends_at_newline = new_sub_last_token_index > 0 && tokens_new[new_sub_last_token_index].before_newline;
              cur_sub_ends_at_newline = cur_sub_last_token_index > 0 && tokens_new[cur_sub_last_token_index].before_newline;
            }


            if(new_sub_len > sub_length ||
               (new_sub_len == sub_length && !cur_sub_ends_at_newline && new_sub_ends_at_newline)) {

                // if(new_sub_len == sub_length && (new_sub_ends_at_newline || !cur_sub_ends_at_newline)) {
                //   fprintf(stderr, "HAVE FOUND NEWLINE TOKEN\n");
                //   fprintf(stderr, "preferring (%d, %d) over (%d, %d)\n", new_sub_start_old, new_sub_len, sub_start_old, sub_length);
                // }

                sub_length = new_sub_len;
                sub_start_old = new_sub_start_old;
                sub_start_new = new_sub_start_new;

                // fprintf(stderr, "%d => %d\n", new_sub_start_old, iold - sub_length + 1);
                // fprintf(stderr, "%d => %d\n", new_sub_start_new, inew - sub_length + 1);
                // sub_start_old = iold - sub_length + 1;
                // sub_start_new = inew - sub_length + 1;
            }
            assert(sub_length <= len_old);
            assert(sub_length <= len_new);
            assert(sub_start_old >= start_old);
            assert(sub_start_new >= start_new);
            // st_insert(_overlap, (st_data_t) iold, new_sub_len);
            int_int_map_insert(_overlap, iold, new_sub_len);
          }

          if(next_index == UINT32_MAX) {
            break;
          } else {
            pair = index_list->entries[next_index];
          }
        }
      }

      {
        IntIntMap *tmp = overlap;
        overlap = _overlap;
        _overlap = tmp;

        int_int_map_reset(_overlap);
        // st_clear(_overlap);
      }
    }
  }

  if(sub_length == 0) {
    size_t common_len = MIN(len_old, len_new);
    if(common_len > 0) {
      rb_ary_push(rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_MOD, rb_input_old, rb_input_new,
                                                     tokens_old, start_old, common_len,
                                                     tokens_new, start_new, common_len));
      start_old += common_len;
      start_new += common_len;
      len_old -= common_len;
      len_new -= common_len;
    }

    if(len_old > 0) {
      rb_ary_push(rb_out_ary, rb_change_set_new(CHANGE_TYPE_DEL, rb_input_old, tokens_old, start_old, len_old));
    }

    if(len_new > 0) {
      rb_ary_push(rb_out_ary, rb_change_set_new(CHANGE_TYPE_ADD, rb_input_new, tokens_new, start_new, len_new));
    }
  } else {
    // st_clear(_overlap);
    // st_clear(overlap);
    int_int_map_reset(_overlap);
    int_int_map_reset(overlap);

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
      rb_ary_push(rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_EQL, rb_input_old, rb_input_new,
                                                     tokens_old, sub_start_old, sub_length,
                                                     tokens_new, sub_start_new, sub_length));
    }

    // st_clear(_overlap);
    // st_clear(overlap);
    int_int_map_reset(_overlap);
    int_int_map_reset(overlap);
    st_clear(old_index_map);
    index_list->len = 0;

    token_diff(tokens_old, tokens_new, index_list, table_entries_old, overlap, _overlap, old_index_map, rb_input_old, rb_input_new,
              sub_start_old + sub_length, (start_old + len_old) - (sub_start_old + sub_length),
              sub_start_new + sub_length, (start_new + len_new) - (sub_start_new + sub_length),
              rb_out_ary, output_eq);

  }
}

static VALUE
rb_token_new(VALUE rb_input, Token *token_) {
  RbToken *token = RB_ALLOC(RbToken);
  Check_Type(rb_input, T_STRING);
  token->rb_input = rb_input;
  token->token = *token_;
  return TypedData_Wrap_Struct(rb_cToken, &token_type, token);
}


static void
rb_change_set_get(ChangeSet *change_set, long index, VALUE *rb_old_token, VALUE *rb_new_token) {
  *rb_old_token = Qnil;
  *rb_new_token = Qnil;

  switch(change_set->change_type) {
    case CHANGE_TYPE_EQL:
    case CHANGE_TYPE_MOD:
      *rb_old_token = rb_token_new(change_set->rb_old_input, &change_set->tokens[index]);
      *rb_new_token = rb_token_new(change_set->rb_new_input, &change_set->tokens[change_set->len + index]);
      break;
    case CHANGE_TYPE_DEL:
      *rb_old_token = rb_token_new(change_set->rb_old_input, &change_set->tokens[index]);
      break;
    case CHANGE_TYPE_ADD:
      *rb_new_token = rb_token_new(change_set->rb_new_input, &change_set->tokens[index]);
      break;
  }
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

  VALUE rb_old_token;
  VALUE rb_new_token;

  rb_change_set_get(change_set, index, &rb_old_token, &rb_new_token);
  if(!NIL_P(rb_old_token) && !NIL_P(rb_new_token)) {
    return rb_assoc_new(rb_old_token, rb_new_token);
  } else if(!NIL_P(rb_old_token)) {
    return rb_old_token;
  } else {
    return rb_new_token;
  }
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
    case CHANGE_TYPE_MOD:
      type_id = id_mod;
      break;
    default:
      return Qnil;
  }

  return ID2SYM(type_id);
}

static TokenizerLanguage
sym2language(VALUE rb_language) {
  ID id = SYM2ID(rb_language);
 
  if(id == id_c) return TOKENIZER_LANGUAGE_C;
  if(id == id_cpp) return TOKENIZER_LANGUAGE_CPP;
  if(id == id_java) return TOKENIZER_LANGUAGE_JAVA;
  if(id == id_javascript) return TOKENIZER_LANGUAGE_JAVASCRIPT;
  if(id == id_python) return TOKENIZER_LANGUAGE_PYTHON;
  if(id == id_ruby) return TOKENIZER_LANGUAGE_RUBY;
  if(id == id_php) return TOKENIZER_LANGUAGE_PHP;
  if(id == id_go) return TOKENIZER_LANGUAGE_GO;
  if(id == id_c) return TOKENIZER_LANGUAGE_C;

  rb_raise(rb_eArgError, "invalid language '%" PRIsVALUE "'", rb_language);
  return TOKENIZER_LANGUAGE_INVALID;
}

static VALUE
rb_tokdiff_tokenize_s(VALUE self, VALUE rb_language, VALUE rb_input, VALUE rb_ignore_whitespace, VALUE rb_ignore_comments) {
  Check_Type(rb_input, T_STRING);
  TokenizerLanguage language = sym2language(rb_language);

  size_t tokens_capa = 512;
  size_t tokens_len = 0;
  bool ignore_whitespace = RB_TEST(rb_ignore_whitespace);
  bool ignore_comments = RB_TEST(rb_ignore_comments);

  Token *tokens = RB_ALLOC_N(Token, tokens_capa);

  {
    Tokenizer tokenizer = {
      .input = RSTRING_PTR(rb_input),
      .input_len = RSTRING_LEN(rb_input),
      .tokens = tokens,
      .tokens_len = tokens_len,
      .tokens_capa = tokens_capa,
      .ignore_whitespace = ignore_whitespace,
      .ignore_comments = ignore_comments,
      .language = language
    };

    tokenizer_run(&tokenizer);
    // tokenize(RSTRING_PTR(rb_old), RSTRING_LEN(rb_old), &tokens, &tokens_len, &tokens_capa, ignore_whitespace);

    tokens = tokenizer.tokens;
    tokens_len = tokenizer.tokens_len;
    tokens_capa = tokenizer.tokens_capa;
  }

  VALUE rb_ary = rb_ary_new_capa(tokens_len);

  for(size_t i = 0; i < tokens_len; i++) {
    rb_ary_push(rb_ary, rb_token_new(rb_input, &tokens[i]));
  }

  xfree(tokens);

  return rb_ary;
}

#undef NDEBUG
#include <assert.h>

static VALUE
rb_tokdiff_diff_s(VALUE self, VALUE rb_language, VALUE rb_old, VALUE rb_new, VALUE rb_output_eq, VALUE rb_ignore_whitespace, VALUE rb_ignore_comments) {
  Check_Type(rb_old, T_STRING);
  Check_Type(rb_new, T_STRING);
  TokenizerLanguage language = sym2language(rb_language);

  size_t tokens_capa = 512;
  size_t tokens_len = 0;
  ssize_t tokens_old_len = 0;
  ssize_t tokens_new_len = 0;

  bool output_eq = RB_TEST(rb_output_eq);
  bool ignore_whitespace = RB_TEST(rb_ignore_whitespace);
  bool ignore_comments = RB_TEST(rb_ignore_comments);

  const char *input_old = RSTRING_PTR(rb_old);
  const char *input_new = RSTRING_PTR(rb_new);
  size_t input_old_len = RSTRING_LEN(rb_old);
  size_t input_new_len = RSTRING_LEN(rb_new);

  VALUE rb_out_ary = rb_ary_new();

  if(input_old_len == input_new_len && !memcmp(input_old, input_new, input_new_len)) {
    return rb_out_ary;
  }

  Token *tokens = RB_ALLOC_N(Token, tokens_capa);

  {
    Tokenizer tokenizer = {
      .input = input_old,
      .input_len = input_old_len,
      .tokens = tokens,
      .tokens_len = tokens_len,
      .tokens_capa = tokens_capa,
      .ignore_whitespace = ignore_whitespace,
      .ignore_comments = ignore_comments,
      .language = language
    };

    tokenizer_run(&tokenizer);
    // tokenize(RSTRING_PTR(rb_old), RSTRING_LEN(rb_old), &tokens, &tokens_len, &tokens_capa, ignore_whitespace);

    tokens = tokenizer.tokens;
    tokens_len = tokenizer.tokens_len;
    tokens_capa = tokenizer.tokens_capa;
  }

  tokens_old_len = tokens_len;

  {
    Tokenizer tokenizer = {
      .input = input_new,
      .input_len = input_new_len,
      .tokens = tokens,
      .tokens_len = tokens_len,
      .tokens_capa = tokens_capa,
      .ignore_whitespace = ignore_whitespace,
      .ignore_comments = ignore_comments,
      .language = language
    };

    tokenizer_run(&tokenizer);

    tokens = tokenizer.tokens;
    tokens_len = tokenizer.tokens_len;
    tokens_capa = tokenizer.tokens_capa;
  // tokenize(RSTRING_PTR(rb_new), RSTRING_LEN(rb_new), &tokens, &tokens_len, &tokens_capa, ignore_whitespace);
  }

  tokens_new_len = tokens_len - tokens_old_len;

  Token *tokens_old = tokens;
  Token *tokens_new = tokens + tokens_old_len;

  IndexKey *table_entries_old = RB_ALLOC_N(IndexKey, tokens_old_len);

  IndexValue index_list;
  index_list.capa = tokens_old_len;
  index_list.len = 0;
  index_list.entries = RB_ALLOC_N(st_data_t, index_list.capa);

  // for(size_t i = 0; i < tokens_new_len; i++) {
  //   Token *token = &tokens_new[i];
  //   fprintf(stderr, "TOKEN OLD: %.*s\n", token->end_byte - token->start_byte, input_new + token->start_byte);
  //   if(token->before_newline) {
  //     fprintf(stderr, "BEFORE NEWLINE\n");
  //   }
  // }

  IntIntMap overlap;
  IntIntMap _overlap;

  int_int_map_init(&overlap, tokens_old_len);
  int_int_map_init(&_overlap, tokens_old_len);

  st_table *old_index_map = st_init_table(&type_index_key_hash);

  ssize_t prefix_len = 0;
  ssize_t tokens_min_len = MIN(tokens_old_len, tokens_new_len);
  for(ssize_t i = 0; i < tokens_min_len; i++) {
    Token *old_token = &tokens_old[i];
    Token *new_token = &tokens_new[i];

    assert(old_token->end_byte <= input_old_len);
    assert(new_token->end_byte <= input_new_len);

    if(!token_eql(old_token, input_old, new_token, input_new)) break;

    if(old_token->before_newline && new_token->before_newline) {
      prefix_len = MIN(tokens_min_len, i + 1);
    }
  }

  if(prefix_len == tokens_old_len && prefix_len == tokens_new_len) {
    goto done;
  }


  ssize_t suffix_len = 0;
  for(ssize_t i = 0; tokens_old_len - i > prefix_len && tokens_new_len - i > prefix_len; i++) {
    // assert(tokens_old[i - 1].end_byte <= input_old_len);
    // assert(tokens_new[i - 1].end_byte <= input_new_len);
    Token *old_token = &tokens_old[tokens_old_len - i - 1];
    Token *new_token = &tokens_new[tokens_new_len - i - 1];
    if(!token_eql(old_token, input_old, new_token, input_new)) break;

    if(old_token->before_newline && new_token->before_newline) {
      suffix_len = i;
    }
  }

  // suffix_len = 0;
  // prefix_len = 0;

  assert(suffix_len + prefix_len <= tokens_old_len);
  assert(suffix_len + prefix_len <= tokens_new_len);

  if(suffix_len + prefix_len >= MAX(tokens_old_len, tokens_new_len)) {
    rb_p(rb_new);
    rb_p(rb_old);
    fprintf(stderr, "%d\n", rb_eql(rb_new, rb_old));
  }

  assert(suffix_len + prefix_len < MAX(tokens_old_len, tokens_new_len));

  token_diff(tokens_old, tokens_new, &index_list, table_entries_old, &overlap, &_overlap, old_index_map,
            rb_old, rb_new, prefix_len, tokens_old_len - suffix_len - prefix_len, prefix_len, tokens_new_len - suffix_len - prefix_len, rb_out_ary, output_eq);

  // token_diff(tokens_old, tokens_new, &index_list, table_entries_old, overlap, _overlap, old_index_map,
  //           rb_old, rb_new, 0, tokens_old_len, 0, tokens_new_len, rb_out_ary, output_eq);

done:
  // st_free_table(overlap);
  // st_free_table(_overlap);
  int_int_map_destroy(&overlap);
  int_int_map_destroy(&_overlap);
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
rb_token_start_byte(VALUE self) {
  RbToken *token;
  TypedData_Get_Struct(self, RbToken, &token_type, token);

  uint32_t start_byte = token->token.start_byte;
  return INT2FIX(start_byte);
}

static VALUE
rb_token_end_byte(VALUE self) {
  RbToken *token;
  TypedData_Get_Struct(self, RbToken, &token_type, token);

  uint32_t end_byte = token->token.end_byte;
  return INT2FIX(end_byte);
}

// static VALUE
// rb_token_eql(VALUE self, VALUE rb_other) {
//   RbToken *token;
//   TypedData_Get_Struct(self, RbToken, &token_type, token);

//   if(!RB_TYPE_P(rb_other, RUBY_T_DATA)) {
//     return Qfalse;
//   }

//   if(!rb_typeddata_is_kind_of(rb_other, &token_type)) {
//     return Qfalse;
//   }

//   RbToken *other;
//   TypedData_Get_Struct(rb_other, RbToken, &token_type, other);

//   const char *other_input = RSTRING_PTR(rb_input);

//   size_t input_len = RSTRING_LEN(rb_input);

//   size_t other_input_len = RSTRING_LEN(rb_input);

//   return ;

//   const char *input = RSTRING_PTR(rb_input);
//   size_t input_len = RSTRING_LEN(rb_input);

//   return !memcmp()
// }

static VALUE
rb_token_text(VALUE self) {
  RbToken *token;
  TypedData_Get_Struct(self, RbToken, &token_type, token);

  uint32_t start_byte = token->token.start_byte;
  uint32_t end_byte = token->token.end_byte;

  VALUE rb_input = token->rb_input;
  const char *input = RSTRING_PTR(rb_input);
  size_t input_len = RSTRING_LEN(rb_input);

  if(start_byte == end_byte) {
    return rb_str_new("", 0);
  }

  if(start_byte >= input_len || end_byte > input_len) {
    rb_raise(rb_eTokdiffError, "text range exceeds input length (%d-%d > %zu)", start_byte, end_byte, input_len);
    return Qnil;
  }
  return rb_utf8_str_new(input + start_byte, end_byte - start_byte);
}

static VALUE
rb_token_starts_with_p(int argc, VALUE *argv, VALUE self) {
  RbToken *token;
  TypedData_Get_Struct(self, RbToken, &token_type, token);

  uint32_t start_byte = token->token.start_byte;
  uint32_t end_byte = token->token.end_byte;
  VALUE rb_input = token->rb_input;
  const char *input = RSTRING_PTR(rb_input);

  for(int i = 0; i < argc; i++) {
    VALUE rb_text = argv[i];
    if(RB_TYPE_P(rb_text, T_STRING)) {
      size_t text_len = RSTRING_LEN(rb_text);
      if(text_len > end_byte - start_byte) return Qfalse;
      if(text_len == 0) return Qtrue;
      if(!rb_memcmp(input + start_byte, RSTRING_PTR(rb_text), text_len)) return Qtrue;
    }
  }
  return Qfalse;
}

static VALUE
rb_token_ends_with_p(int argc, VALUE *argv, VALUE self) {
  RbToken *token;
  TypedData_Get_Struct(self, RbToken, &token_type, token);

  uint32_t start_byte = token->token.start_byte;
  uint32_t end_byte = token->token.end_byte;
  VALUE rb_input = token->rb_input;
  const char *input = RSTRING_PTR(rb_input);

  for(int i = 0; i < argc; i++) {
    VALUE rb_text = argv[i];
    if(RB_TYPE_P(rb_text, T_STRING)) {
      size_t text_len = RSTRING_LEN(rb_text);
      if(text_len > end_byte - start_byte) return Qfalse;
      if(text_len == 0) return Qtrue;
      if(!rb_memcmp(input + end_byte - text_len, RSTRING_PTR(rb_text), text_len)) return Qtrue;
    }
  }
  return Qfalse;
}

static VALUE
rb_token_text_p(int argc, VALUE *argv, VALUE self) {
  RbToken *token;
  TypedData_Get_Struct(self, RbToken, &token_type, token);

  uint32_t start_byte = token->token.start_byte;
  uint32_t end_byte = token->token.end_byte;
  VALUE rb_input = token->rb_input;
  const char *input = RSTRING_PTR(rb_input);
  size_t input_len = RSTRING_LEN(rb_input);

  // this should never happen?
  if(start_byte >= input_len || end_byte > input_len) {
    rb_raise(rb_eTokdiffError, "text range exceeds input length (%d-%d > %zu)", start_byte, end_byte, input_len);
    return Qnil;
  }

  for(int i = 0; i < argc; i++) {
    VALUE rb_text = argv[i];
    if(RB_TYPE_P(rb_text, T_STRING)) {
      size_t text_len = RSTRING_LEN(rb_text);

      if(text_len != end_byte - start_byte) continue;
      if(start_byte == end_byte && text_len == 0) return Qtrue;

      if(!rb_memcmp(input + start_byte, RSTRING_PTR(rb_text), end_byte - start_byte)) {
        return Qtrue;
      }
    }
  }
  return Qfalse;
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
    VALUE rb_old_token;
    VALUE rb_new_token;
    rb_change_set_get(change_set, i, &rb_old_token, &rb_new_token);
    rb_yield_values(2, rb_old_token, rb_new_token);
  }
  return self;
}

static VALUE
rb_change_set_old(VALUE self)
{
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  VALUE rb_ary = rb_ary_new_capa(change_set->len);
  for(uint32_t i = 0; i < change_set->len; i++) {
    VALUE rb_old_token;
    VALUE rb_new_token_;
    rb_change_set_get(change_set, i, &rb_old_token, &rb_new_token_);
    rb_ary_push(rb_ary, rb_old_token);
  }
  return rb_ary;
}

static VALUE
rb_change_set_new_m(VALUE self)
{
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  VALUE rb_ary = rb_ary_new_capa(change_set->len);
  for(uint32_t i = 0; i < change_set->len; i++) {
    VALUE rb_old_token_;
    VALUE rb_new_token;
    rb_change_set_get(change_set, i, &rb_old_token_, &rb_new_token);
    rb_ary_push(rb_ary, rb_new_token);
  }
  return rb_ary;
}

void
Init_core()
{
  id_add = rb_intern("+");
  id_del = rb_intern("-");
  id_eq = rb_intern("=");
  id_mod = rb_intern("!");

  id_c = rb_intern("c");
  id_cpp = rb_intern("cpp");
  id_java = rb_intern("java");
  id_javascript = rb_intern("javascript");
  id_python = rb_intern("python");
  id_ruby = rb_intern("ruby");
  id_php = rb_intern("php");
  id_go = rb_intern("go");


  rb_mTokdiff = rb_define_module("Tokdiff");
  rb_eTokdiffError = rb_define_class_under(rb_mTokdiff, "Error", rb_eStandardError);

  rb_define_singleton_method(rb_mTokdiff, "__diff__", rb_tokdiff_diff_s, 6);
  rb_define_singleton_method(rb_mTokdiff, "__tokenize__", rb_tokdiff_tokenize_s, 4);

  rb_cChangeSet = rb_define_class_under(rb_mTokdiff, "ChangeSet", rb_cObject);
  rb_cToken = rb_define_class_under(rb_mTokdiff, "Token", rb_cObject);

  rb_define_method(rb_cChangeSet, "[]", rb_change_set_aref, 1);
  rb_define_method(rb_cChangeSet, "size", rb_change_set_size, 0);
  rb_define_method(rb_cChangeSet, "type", rb_change_set_type, 0);
  rb_define_method(rb_cChangeSet, "old", rb_change_set_old, 0);
  rb_define_method(rb_cChangeSet, "new", rb_change_set_new_m, 0);
  rb_define_method(rb_cChangeSet, "each", rb_change_set_each, 0);
  rb_include_module(rb_cChangeSet, rb_mEnumerable);


  rb_define_method(rb_cToken, "byte_range", rb_token_byte_range, 0);
  rb_define_method(rb_cToken, "end_byte", rb_token_end_byte, 0);
  rb_define_method(rb_cToken, "start_byte", rb_token_start_byte, 0);

  rb_define_method(rb_cToken, "text", rb_token_text, 0);
  rb_define_method(rb_cToken, "text?", rb_token_text_p, -1);
  rb_define_method(rb_cToken, "starts_with?", rb_token_starts_with_p, -1);
  rb_define_method(rb_cToken, "ends_with?", rb_token_ends_with_p, -1);
  // rb_define_method(rb_cToken, "==", rb_token_eql, 1);
  // rb_define_method(rb_cToken, "eql?", rb_token_eql, 1);


}