#include "ruby.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

VALUE rb_mTSDiff;
VALUE rb_cChangeSet;
VALUE rb_eTsDiffError;

static ID id_eq;
static ID id_add;
static ID id_del;
static ID id_mod;

#ifndef MAX
#define MAX(a,b) (((a)<(b))?(b):(a))
#endif

#ifndef MIN
#define MIN(a,b) (((a)>(b))?(b):(a))
#endif


#undef NDEBUG
#include <assert.h>

typedef struct {
  uint32_t context[4];
  const void *id;
  const void *tree;
} TSNode;

typedef struct {
  TSNode ts_node;
  VALUE rb_tree;
  uint32_t start_byte;
  uint32_t end_byte;
  bool implicit;
  bool before_newline;
} Token;

typedef struct TokenArray {
  Token *data;
  size_t len;
  size_t capa;
} TokenArray;

TokenArray rb_node_tokenize_(VALUE self, VALUE rb_ignore_whitespace, VALUE rb_ignore_comments);
const char *rb_node_input_(VALUE self, uint32_t *start, uint32_t *len);
VALUE rb_new_token_from_ptr(Token *orig_token);
void tree_sitter_token_mark(Token *token);

typedef struct {
  uint32_t capa;
  uint32_t len;
  st_data_t *entries;
} IndexValue;

typedef struct {
  uint32_t start_byte;
  uint32_t end_byte;
  const char *input;
} IndexKey;

typedef struct {
  VALUE rb_old;
  VALUE rb_new;
  Token *old_tokens;
  Token *new_tokens;
  uint32_t old_len;
  uint32_t new_len;
  uint8_t change_type;
} ChangeSet;

typedef enum {
  CHANGE_TYPE_ADD,
  CHANGE_TYPE_DEL,
  CHANGE_TYPE_EQL,
  CHANGE_TYPE_MOD,
} ChangeType;

typedef struct {
  uint32_t *data;
  uint32_t min_index;
  uint32_t max_index;
} IntIntMap;

typedef struct {
  TokenArray tokens_old;
  TokenArray tokens_new;
  IntIntMap overlap;
  IntIntMap _overlap;
  IndexValue index_list;
  IndexKey *table_entries_old;
  st_table *old_index_map;
  const char *input_old;
  const char *input_new;
  VALUE rb_old;
  VALUE rb_new;
  VALUE rb_out_ary;
  bool output_eq;
  bool split_lines;
} DiffContext;

static void change_set_free(void *ptr)
{
  ChangeSet *change_set = (ChangeSet *) ptr;
  xfree(change_set->old_tokens);
  xfree(change_set->new_tokens);
  xfree(ptr);
}

static void change_set_mark(void *ptr) {
  ChangeSet *change_set = (ChangeSet *) ptr;
  rb_gc_mark(change_set->rb_old);
  rb_gc_mark(change_set->rb_new);

  for(size_t i = 0; i < change_set->old_len; i++) {
    tree_sitter_token_mark(&change_set->old_tokens[i]);
  }

  for(size_t i = 0; i < change_set->new_len; i++) {
    tree_sitter_token_mark(&change_set->new_tokens[i]);
  }
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
rb_change_set_new_full(ChangeType change_type, VALUE rb_old, VALUE rb_new,
                       TokenArray *old_tokens, size_t old_start, size_t old_len,
                       TokenArray *new_tokens, size_t new_start, size_t new_len)
{
  ChangeSet *change_set = RB_ALLOC(ChangeSet);
  change_set->change_type = change_type;
  change_set->rb_old = rb_old;
  change_set->rb_new = rb_new;
  change_set->old_len = old_len;
  change_set->new_len = new_len;

  assert(old_len == 0 || new_len == 0 || old_len == new_len);
  change_set->old_tokens = RB_ALLOC_N(Token, old_len);
  change_set->new_tokens = RB_ALLOC_N(Token, new_len);

  // fprintf(stderr, "TOKEN SET %d/%d  %d/%d\n", old_start, old_len, new_start, new_len);

  memcpy(change_set->old_tokens, old_tokens->data + old_start, old_len * sizeof(Token));
  memcpy(change_set->new_tokens, new_tokens->data + new_start, new_len * sizeof(Token));
  
  return TypedData_Wrap_Struct(rb_cChangeSet, &change_set_type, change_set);
}

static VALUE
rb_change_set_new(ChangeType change_type, VALUE rb_input,
                  TokenArray *tokens, size_t start, size_t len)
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

static void
output_change_set(DiffContext *ctx, ChangeType change_type, size_t start_old, size_t len_old, size_t start_new, size_t len_new) {

  //FIXME: splitting modification is tricky...                    
  if(change_type == CHANGE_TYPE_MOD || change_type == CHANGE_TYPE_EQL || !ctx->split_lines) {
    rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(change_type, ctx->rb_old, ctx->rb_new,
                                                        &ctx->tokens_old, start_old, len_old,
                                                        &ctx->tokens_new, start_new, len_new));
  } else {
    size_t start, len;
    TokenArray *tokens;
    VALUE rb_input;
    switch(change_type) {
      case CHANGE_TYPE_ADD: {
        start = start_new;
        len = len_new;
        tokens = &ctx->tokens_new;
        rb_input = ctx->rb_new;
        break;
      }
      case CHANGE_TYPE_DEL: {
        start = start_old;
        len = len_old;
        tokens = &ctx->tokens_old;
        rb_input = ctx->rb_old;
        break;
      }
      default: {
        rb_raise(rb_eRuntimeError, "unexpected change type");
      }
    }

    size_t end = start + len;
    size_t next_start = start;

    // for(size_t i = start; i < end; i++) {
    //   Token *token = &tokens->data[i];
    //   if(token->before_newline) {
    //     rb_ary_push(rb_out_ary, rb_change_set_new(change_type, rb_input, tokens, next_start, i - next_start + 1));
    //     next_start = i + 1;
    //   }
    // }

    if(next_start < end) {
      rb_ary_push(ctx->rb_out_ary, rb_change_set_new(change_type, rb_input, tokens, next_start, end - next_start));
    }
  }
}


static void
index_old(DiffContext *ctx, uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new) {
  for(size_t iold = start_old; iold < start_old + len_old; iold++) {
    Token *token = &ctx->tokens_old.data[iold];

    IndexKey *key = &ctx->table_entries_old[iold];
    key->start_byte = token->start_byte;
    key->end_byte = token->end_byte;
    key->input = ctx->input_old;

    UpdateArg arg = {
      .index_list = &ctx->index_list,
      .value = iold,
    };

    st_update(ctx->old_index_map, (st_data_t) key, update_callback, (st_data_t) &arg);
  }
}

// Loosely based on https://github.com/paulgb/simplediff
static void
token_diff(DiffContext *ctx, uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new) {

  if(len_old == 0 && len_new == 0) return;

  uint32_t sub_start_old = start_old;
  uint32_t sub_start_new = start_new;
  uint32_t sub_length = 0;

  index_old(ctx, start_old, len_old, start_new, len_new);

  // for(size_t i = 0; i < ctx->tokens_new.len; i++) {
  //   Token *token = &ctx->tokens_new.data[i];
  //   fprintf(stderr, "TOKEN NEW (%d): %.*s\n", token->implicit, token->end_byte - token->start_byte, ctx->input_new + token->start_byte);
  //   if(token->before_newline) {
  //     fprintf(stderr, "BEFORE NEWLINE\n");
  //   }
  // }

  // for(size_t i = 0; i < ctx->tokens_old.len; i++) {
  //   Token *token = &ctx->tokens_old.data[i];
  //   fprintf(stderr, "TOKEN OLD (%d): %.*s\n", token->implicit, token->end_byte - token->start_byte, ctx->input_old + token->start_byte);
  //   if(token->before_newline) {
  //     fprintf(stderr, "BEFORE NEWLINE\n");
  //   }
  // }

  if(len_old > 0) {
    for(size_t inew = start_new; inew < start_new + len_new; inew++) {
      Token *token = &ctx->tokens_new.data[inew];

    //  fprintf(stderr, "TOKEN NEW: %.*s %d\n", token->end_byte - token->start_byte, input_new + token->start_byte, inew);
    //   if(token->before_newline) {
    //      fprintf(stderr, "HAVE FOUND NEWLINE TOKEN\n");
    //     //  abort();
    //   }

      // assert(_overlap->num_entries == 0);

      IndexKey key = {
        .start_byte = token->start_byte,
        .end_byte = token->end_byte,
        .input = ctx->input_new
      };

      st_data_t pair;

      if(st_lookup(ctx->old_index_map, (st_data_t)&key, &pair)) {
        while(true) {
          uint32_t value = PAIR64_FIRST(pair);
          uint32_t next_index = PAIR64_SECOND(pair);

          uint32_t iold = value;

          uint32_t prev_sub_len = 0;

          if(iold > start_old) {
            prev_sub_len = ctx->overlap.data[iold - 1];
            // st_lookup(overlap, (st_data_t) (iold - 1), (st_data_t *) &prev_sub_len);
          }

          /*if(!(prev_sub_len == 0 && token->dont_start))*/ {
            uint32_t new_sub_len = prev_sub_len + 1;
            uint32_t new_sub_start_old = iold - new_sub_len + 1;
            uint32_t new_sub_start_new = inew - new_sub_len + 1;

            // bool new_sub_ends_at_newline = false;
            // bool cur_sub_ends_at_newline = false;

            // if(sub_length > 0) {
            //   int64_t cur_sub_last_token_index = sub_start_new + sub_length - 1;
            //   int64_t new_sub_last_token_index = new_sub_start_new + new_sub_len - 1;

            //   new_sub_ends_at_newline = new_sub_last_token_index > 0 && tokens_new->data[new_sub_last_token_index].before_newline;
            //   cur_sub_ends_at_newline = cur_sub_last_token_index > 0 && tokens_new->data[cur_sub_last_token_index].before_newline;
            // }


            if(new_sub_len > sub_length) { // || (new_sub_len == sub_length && !cur_sub_ends_at_newline && new_sub_ends_at_newline)) {

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
            int_int_map_insert(&ctx->_overlap, iold, new_sub_len);
          }

          if(next_index == UINT32_MAX) {
            break;
          } else {
            pair = ctx->index_list.entries[next_index];
          }
        }
      }

      {
        IntIntMap tmp = ctx->overlap;
        ctx->overlap = ctx->_overlap;
        ctx->_overlap = tmp;

        int_int_map_reset(&ctx->_overlap);
        // st_clear(_overlap);
      }
    }
  }

  if(sub_length == 0) {
    size_t common_len = MIN(len_old, len_new);
    if(common_len > 0) {
      output_change_set(ctx, CHANGE_TYPE_MOD,
                        start_old, common_len,
                        start_new, common_len);

      // rb_ary_push(rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_MOD, rb_input_old, rb_input_new,
      //                                                tokens_old, start_old, common_len,
      //                                                tokens_new, start_new, common_len));
      start_old += common_len;
      start_new += common_len;
      len_old -= common_len;
      len_new -= common_len;
    }

    if(len_old > 0) {
      // rb_ary_push(rb_out_ary, rb_change_set_new(CHANGE_TYPE_DEL, rb_input_old, tokens_old, start_old, len_old));
      output_change_set(ctx, CHANGE_TYPE_DEL,
                        start_old, len_old,
                        0, 0);
    }

    if(len_new > 0) {
      // rb_ary_push(rb_out_ary, rb_change_set_new(CHANGE_TYPE_ADD, rb_input_new, tokens_new, start_new, len_new));
      output_change_set(ctx, CHANGE_TYPE_ADD,
                        0, 0,
                        start_new, len_new);
                        
    }
  } else {
    // st_clear(_overlap);
    // st_clear(overlap);
    int_int_map_reset(&ctx->_overlap);
    int_int_map_reset(&ctx->overlap);

    st_clear(ctx->old_index_map);
    ctx->index_list.len = 0;

    assert(sub_length <= len_old);
    assert(sub_length <= len_new);
    assert(sub_start_old >= start_old);
    assert(sub_start_new >= start_new);

    token_diff(ctx, start_old, sub_start_old - start_old,
              start_new, sub_start_new - start_new);

    if(ctx->output_eq) {
      output_change_set(ctx, CHANGE_TYPE_EQL,
                        sub_start_old, sub_length,
                        sub_start_new, sub_length);
      // rb_ary_push(rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_EQL, rb_input_old, rb_input_new,
      //                                                tokens_old, sub_start_old, sub_length,
      //                                                tokens_new, sub_start_new, sub_length));
    }

    // st_clear(_overlap);
    // st_clear(overlap);
    int_int_map_reset(&ctx->_overlap);
    int_int_map_reset(&ctx->overlap);
    st_clear(ctx->old_index_map);
    ctx->index_list.len = 0;

    token_diff(ctx, 
              sub_start_old + sub_length, (start_old + len_old) - (sub_start_old + sub_length),
              sub_start_new + sub_length, (start_new + len_new) - (sub_start_new + sub_length));
  }
}

static void
rb_change_set_get(ChangeSet *change_set, long index, VALUE *rb_old_token, VALUE *rb_new_token) {
  *rb_old_token = Qnil;
  *rb_new_token = Qnil;

  switch(change_set->change_type) {
    case CHANGE_TYPE_EQL:
    case CHANGE_TYPE_MOD:
      *rb_old_token = rb_new_token_from_ptr(&change_set->old_tokens[index]);
      *rb_new_token = rb_new_token_from_ptr(&change_set->new_tokens[index]);
      break;
    case CHANGE_TYPE_DEL:
      *rb_old_token = rb_new_token_from_ptr(&change_set->old_tokens[index]);
      break;
    case CHANGE_TYPE_ADD:
      *rb_new_token = rb_new_token_from_ptr(&change_set->new_tokens[index]);
      break;
  }
}

static uint32_t change_set_len(ChangeSet *change_set) {
  return MAX(change_set->old_len, change_set->new_len);
}


static VALUE
rb_change_set_aref(VALUE self, VALUE rb_index) {
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  long index = FIX2LONG(rb_index);
  uint32_t len = change_set_len(change_set);

  if(index < 0) {
    index = len + index;
  }
  if(index < 0 || index >= len) {
    rb_raise(rb_eIndexError, "index %ld outside bounds: %d...%u", index, 0,  (unsigned) len);
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

  uint32_t len = change_set_len(change_set);
  return LONG2FIX((long) len);
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

static VALUE
rb_ts_diff_diff_s(VALUE self, VALUE rb_old, VALUE rb_new,
                  VALUE rb_output_eq, VALUE rb_ignore_whitespace, VALUE rb_ignore_comments) {

  // FIXME: check node
  // Check_Type(rb_old, T_STRING);
  // Check_Type(rb_new, T_STRING);

  DiffContext ctx;

  ctx.output_eq = RB_TEST(rb_output_eq);
  ctx.split_lines = false; //RB_TEST(rb_split_lines);
  bool ignore_whitespace = RB_TEST(rb_ignore_whitespace);
  bool ignore_comments = RB_TEST(rb_ignore_comments);

  uint32_t input_old_start;
  uint32_t input_new_start;
  uint32_t input_old_len;
  uint32_t input_new_len;

  ctx.input_old = rb_node_input_(rb_old, &input_old_start, &input_old_len);
  ctx.input_new = rb_node_input_(rb_new, &input_new_start, &input_new_len);
  ctx.rb_out_ary = rb_ary_new();

  if(input_old_len == input_new_len && !memcmp(ctx.input_old + input_old_start, ctx.input_new + input_new_start, input_new_len)) {
    return ctx.rb_out_ary;
  }

  ctx.tokens_old = rb_node_tokenize_(rb_old, ignore_whitespace, ignore_comments);
  ctx.tokens_new = rb_node_tokenize_(rb_new, ignore_whitespace, ignore_comments);

  ssize_t tokens_old_len = (ssize_t) ctx.tokens_old.len;
  ssize_t tokens_new_len = (ssize_t) ctx.tokens_new.len;

  ctx.table_entries_old = RB_ALLOC_N(IndexKey, tokens_old_len);

  ctx.index_list.capa = ctx.tokens_old.len;
  ctx.index_list.len = 0;
  ctx.index_list.entries = RB_ALLOC_N(st_data_t, ctx.index_list.capa);

  // for(size_t i = 0; i < tokens_new_len; i++) {
  //   Token *token = &tokens_new[i];
  //   fprintf(stderr, "TOKEN OLD: %.*s\n", token->end_byte - token->start_byte, input_new + token->start_byte);
  //   if(token->before_newline) {
  //     fprintf(stderr, "BEFORE NEWLINE\n");
  //   }
  // }


  int_int_map_init(&ctx.overlap, ctx.tokens_old.len);
  int_int_map_init(&ctx._overlap, ctx.tokens_old.len);

  ctx.old_index_map = st_init_table(&type_index_key_hash);

  ssize_t prefix_len = 0;
  ssize_t tokens_min_len = MIN(ctx.tokens_old.len, ctx.tokens_new.len);
  for(ssize_t i = 0; i < tokens_min_len; i++) {
    Token *old_token = &ctx.tokens_old.data[i];
    Token *new_token = &ctx.tokens_new.data[i];

    assert(old_token->end_byte <= input_old_start + input_old_len);
    assert(new_token->end_byte <= input_new_start + input_new_len);

    if(!token_eql(old_token, ctx.input_old, new_token, ctx.input_new)) break;


    //if(old_token->before_newline && new_token->before_newline) {
      prefix_len = MIN(tokens_min_len, i + 1);
    //}
  }

  if(prefix_len == tokens_old_len && prefix_len == tokens_new_len) {
    goto done;
  }


  ssize_t suffix_len = 0;
  for(ssize_t i = 0; tokens_old_len - i > prefix_len && tokens_new_len - i > prefix_len; i++) {
    // assert(tokens_old[i - 1].end_byte <= input_old_len);
    // assert(tokens_new[i - 1].end_byte <= input_new_len);
    Token *old_token = &ctx.tokens_old.data[tokens_old_len - i - 1];
    Token *new_token = &ctx.tokens_new.data[tokens_new_len - i - 1];

    //if(old_token->before_newline && new_token->before_newline) {
      suffix_len = i;
    //}

    if(!token_eql(old_token, ctx.input_old, new_token, ctx.input_new)) break;

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

  token_diff(&ctx,
            prefix_len, ctx.tokens_old.len - suffix_len - prefix_len,
            prefix_len, ctx.tokens_new.len - suffix_len - prefix_len);

  // token_diff(tokens_old, tokens_new, &index_list, table_entries_old, overlap, _overlap, old_index_map,
  //           rb_old, rb_new, 0, tokens_old_len, 0, tokens_new_len, rb_out_ary, output_eq);

done:
  // st_free_table(overlap);
  // st_free_table(_overlap);
  int_int_map_destroy(&ctx.overlap);
  int_int_map_destroy(&ctx._overlap);
  st_free_table(ctx.old_index_map);
  xfree(ctx.table_entries_old);
  xfree(ctx.index_list.entries);
  xfree(ctx.tokens_old.data);
  xfree(ctx.tokens_new.data);

  return ctx.rb_out_ary;
}

static VALUE
change_set_enum_length(VALUE rb_change_set, VALUE args, VALUE eobj)
{
  ChangeSet *change_set;
  TypedData_Get_Struct(rb_change_set, ChangeSet, &change_set_type, change_set);
  uint32_t len = change_set_len(change_set);
  return UINT2NUM(len);
}

static VALUE
rb_change_set_each(VALUE self)
{
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);
  RETURN_SIZED_ENUMERATOR(self, 0, 0, change_set_enum_length);

  uint32_t len = change_set_len(change_set);
  for(uint32_t i = 0; i < len; i++) {
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

  uint32_t len = change_set_len(change_set);
  VALUE rb_ary = rb_ary_new_capa(len);
  for(uint32_t i = 0; i < len; i++) {
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

  uint32_t len = change_set_len(change_set);
  VALUE rb_ary = rb_ary_new_capa(len);
  for(uint32_t i = 0; i < len; i++) {
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

  VALUE rb_mTreeSitter = rb_define_module("TreeSitter");
  rb_mTSDiff = rb_define_module_under(rb_mTreeSitter, "Diff");
  rb_eTsDiffError = rb_define_class_under(rb_mTSDiff, "Error", rb_eStandardError);

  rb_define_singleton_method(rb_mTSDiff, "__diff__", rb_ts_diff_diff_s, 5);

  rb_cChangeSet = rb_define_class_under(rb_mTSDiff, "ChangeSet", rb_cObject);

  rb_define_method(rb_cChangeSet, "[]", rb_change_set_aref, 1);
  rb_define_method(rb_cChangeSet, "size", rb_change_set_size, 0);
  rb_define_method(rb_cChangeSet, "type", rb_change_set_type, 0);
  rb_define_method(rb_cChangeSet, "old", rb_change_set_old, 0);
  rb_define_method(rb_cChangeSet, "new", rb_change_set_new_m, 0);
  rb_define_method(rb_cChangeSet, "each", rb_change_set_each, 0);
  rb_include_module(rb_cChangeSet, rb_mEnumerable);

  // rb_define_method(rb_cToken, "==", rb_token_eql, 1);
  // rb_define_method(rb_cToken, "eql?", rb_token_eql, 1);


}