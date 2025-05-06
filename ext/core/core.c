#include "ruby.h"
#include "ruby/internal/special_consts.h"
#include "ruby/internal/value_type.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>

/*
Implementation based on this blog post:
https://blog.jcoglan.com/2017/03/22/myers-diff-in-linear-space-theory/
*/

VALUE rb_mTSDiff;
VALUE rb_cChangeSet;
VALUE rb_eTsDiffError;

static ID id_eql;
static ID id_add;
static ID id_del;
static ID id_sub;

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
  uint16_t node_symbol;
  bool implicit;
  bool before_newline;
} Token;

typedef struct TokenArray {
  Token *data;
  size_t len;
  size_t capa;
} TokenArray;

typedef struct TmpTokenArray {
  Token *data;
  size_t len;
  size_t capa;
  bool non_eq;
} TmpTokenArray;

typedef struct {} Tree;

TokenArray rb_node_tokenize_(VALUE self, VALUE rb_ignore_whitespace, VALUE rb_ignore_comments);
const char *rb_node_input_(VALUE self, uint32_t *start, uint32_t *len);
VALUE rb_new_token_from_ptr(Token *orig_token);
void tree_sitter_token_mark(Token *token);
Tree *rb_tree_unwrap(VALUE self);
// typedef struct {
//   uint32_t capa;
//   uint32_t len;
//   st_data_t *entries;
// } IndexValue;

// typedef struct {
//   uint32_t start_byte;
//   uint32_t end_byte;
//   const char *input;
// } IndexKey;

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
  CHANGE_TYPE_SUB,
} ChangeType;

// typedef struct {
//   uint32_t len;
// } Match;

// typedef struct {
//   Match *data;
//   uint32_t min_index;
//   uint32_t max_index;
// } MatchMap;

// typedef struct {
//   uint32_t sub_start_old;
//   uint32_t sub_start_new;
//   uint32_t sub_len;
// } BetterMatch;

// #define BETTER_MATCH_ARRAY_INLINE_CAPA 6

// typedef struct {
//   BetterMatch *data;
//   BetterMatch inline_data[BETTER_MATCH_ARRAY_INLINE_CAPA];
//   uint32_t len;
//   uint32_t capa;
//   uint32_t max;
// } BetterMatchArray;

typedef struct Path {
  int64_t x;
  int64_t y;
  uint32_t next;
} Path;

typedef uint32_t PathIdx;

typedef struct {
  Path *data;
  uint32_t len;
  uint32_t capa;
} PathArray;

// typedef struct {
//   uint32_t *data;
//   uint32_t rows;
//   uint32_t cols;
// } Matrix;

typedef enum {
  CALLBACK_START,
  CALLBACK_FINISH,
  CALLBACK_EQ,
  CALLBACK_INS,
  CALLBACK_DEL,
} CallbackType;

struct DiffContext;

typedef void (*Callback)(struct DiffContext *ctx, CallbackType type, Token *token_old, Token *token_new);

typedef struct DiffContext {
  TokenArray tokens_old;
  TokenArray tokens_new;
  Token *tokens_old_;
  Token *tokens_new_;
  // MatchMap match_map;
  // MatchMap next_match_map;
  PathArray path_array;
  // IndexValue index_list;
  // IndexKey *table_entries_old;
  // st_table *old_index_map;
  const char *input_old;
  const char *input_new;
  VALUE rb_old;
  VALUE rb_new;
  bool output_eq;
  bool output_replace;
  bool split_lines;
  Callback cb;
  TmpTokenArray tmp_tokens_old;
  TmpTokenArray tmp_tokens_new;
  VALUE rb_out_ary;
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

static void
add_tmp_token(TmpTokenArray *tokens, Token token, CallbackType type) {
  if(tokens->len >= tokens->capa) {
    size_t new_capa = 2 * tokens->capa;
    RB_REALLOC_N(tokens->data, Token, new_capa);
    tokens->capa = new_capa;
  }

  tokens->data[tokens->len] = token;
  tokens->len++;
  tokens->non_eq = (type != CALLBACK_EQ);
}

static void
tmp_token_array_init(TmpTokenArray *token_array, uint32_t capa) {
  token_array->data = RB_ZALLOC_N(Token, capa);
  token_array->capa = capa;
  token_array->len = 0;
  token_array->non_eq = false;
}

static void
tmp_token_array_destroy(TmpTokenArray *token_array) {
  xfree(token_array->data);
}

static void
tmp_token_array_reset(TmpTokenArray *token_array) {
  token_array->len = 0;
  token_array->non_eq = false;
}

// static st_index_t
// index_key_hash(st_data_t arg) {
//   IndexKey *table_entry = (IndexKey *) arg;

//   uint32_t start_byte = table_entry->start_byte;
//   uint32_t end_byte = table_entry->end_byte;
//   const char *str = table_entry->input + start_byte;
//   size_t len = end_byte - start_byte;

//   //return st_hash(str, len, FNV1_32A_INIT);
//   return rb_memhash(str, len); // ^ len;
// }

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

// static int
// index_key_cmp(st_data_t x, st_data_t y) {
//   IndexKey *table_entry_x = (IndexKey *) x;
//   IndexKey *table_entry_y = (IndexKey *) y;

//   uint32_t start_byte_x = table_entry_x->start_byte;
//   uint32_t end_byte_x = table_entry_x->end_byte;

//   uint32_t start_byte_y = table_entry_y->start_byte;
//   uint32_t end_byte_y = table_entry_y->end_byte;

//   uint32_t len_x = end_byte_x - start_byte_x;
//   uint32_t len_y = end_byte_y - start_byte_y;

//   if(len_x != len_y) {
//     return 1;
//   } else {
//     const char *str_x = table_entry_x->input + start_byte_x;
//     const char *str_y = table_entry_y->input + start_byte_y;
//     return memcmp(str_x, str_y, len_x);
//   }
// }

// static const struct st_hash_type type_index_key_hash = {
//     index_key_cmp,
//     index_key_hash,
// };

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


// static uint32_t
// index_list_append(IndexValue *index_list, st_data_t pair) {
//   if(!(index_list->len < index_list->capa)) {
//     uint32_t new_capa = 2 * index_list->capa;
//     RB_REALLOC_N(index_list->entries, st_data_t, new_capa);
//     index_list->capa = new_capa;
//   }
//   uint32_t index = index_list->len;
//   index_list->entries[index] = pair;
//   index_list->len++;
//   return index;
// }

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

// typedef struct {
//   IndexValue *index_list;
//   uint32_t value;
// } UpdateArg;

// _Static_assert(sizeof(st_data_t) >= 2 * sizeof(uint32_t));

// #define MAKE_PAIR64(f, s) ((((st_data_t)(s)) << 32) | ((st_data_t)(f)))
// #define PAIR64_FIRST(p) ((p) & 0xFFFFFFFF)
// #define PAIR64_SECOND(p) ((p) >> 32)

// static int
// update_callback(st_data_t *key, st_data_t *value, st_data_t arg, int existing) {
//   UpdateArg *update_arg = (UpdateArg *) arg;
//   IndexValue *index_list = update_arg->index_list;
//   uint32_t insert_value = update_arg->value;

//   if(!existing) {
//     *value = MAKE_PAIR64(insert_value, UINT32_MAX);
//   } else {
//     uint32_t inserted_index = index_list_append(index_list, *value);
//     *value = MAKE_PAIR64(insert_value, inserted_index);
//   }

//   return ST_CONTINUE;
// }

// static inline uint32_t
// matrix_get(Matrix *matrix, uint32_t row, uint32_t col) {
//   return matrix->data[row * matrix->cols + col];
// }

// static void uint32_t
// matrix_set(Matrix *matrix, uint32_t row, uint32_t col, uint32_t val) {
//   matrix->data[row * matrix->cols + col] = val;
// }

// static void
// matrix_init(Matrix *matrix, uint32_t rows, uint32_t cols) {
//   matrix->rows = rows;
//   matrix->cols = cols;
//   matrix->data = RB_ALLOC_N(uint32_t, rows * cols);
// }

// static void
// matrix_destroy(Matrix *matrix) {
//   xfree(matrix->data);
// }

// static void
// match_map_init(MatchMap *map, uint32_t len) {
//   map->data = RB_ZALLOC_N(Match, len);
//   map->min_index = len;
//   map->max_index = 0;
// }

// static void
// match_map_destroy(MatchMap *map) {
//   xfree(map->data);
// }

// static void
// match_map_insert(MatchMap *map, uint32_t index, uint32_t len) {
//   Match match = {
//     .len = len
//   };
//   map->data[index] = match;
//   map->max_index = MAX(map->max_index, index);
//   map->min_index = MIN(map->min_index, index);
// }

// static void
// match_map_reset(MatchMap *map) {
//   if(map->min_index <= map->max_index) {
//      memset(map->data + map->min_index, 0, (map->max_index - map->min_index + 1) * sizeof(Match));
//   }
//   // memset(map->data, 0, map->max_index * sizeof(uint32_t));
// }

// static void
// better_match_array_init(BetterMatchArray *better_match_array) {
//     better_match_array->data = &better_match_array->inline_data[0];
//     better_match_array->capa = BETTER_MATCH_ARRAY_INLINE_CAPA;
//     better_match_array->len = 0;
//     better_match_array->max = 0;

// }

// static uint32_t
// better_match_array_insert(BetterMatchArray *better_match_array, BetterMatch match) {

//   if(better_match_array->max < match.sub_len) {
//     better_match_array->data[0] = match;
//     better_match_array->len = 1;
//     better_match_array->max = match.sub_len;
//     return 0;
//   } else {
//     if(!(better_match_array->len < better_match_array->capa)) {
//       uint32_t new_capa = 2 * better_match_array->capa;
//       if(better_match_array->data == &better_match_array->inline_data[0]) {
//         better_match_array->data = RB_ALLOC_N(BetterMatch, new_capa);
//         memcpy(better_match_array->data, &better_match_array->inline_data[0], sizeof(better_match_array->inline_data));
//       } else {
//         RB_REALLOC_N(better_match_array->data, BetterMatch, new_capa);
//       }
//       better_match_array->capa = new_capa;
//     }

//     uint32_t index = better_match_array->len;
//     better_match_array->data[index] = match;
//     better_match_array->len++;

//     return index;
//   }
// }

// static uint32_t
// better_match_array_reset(BetterMatchArray *better_match_array) {
//   memset(better_match_array->data, 0, sizeof(BetterMatch) * better_match_array->len);
// }

// static void
// better_match_array_destroy(BetterMatchArray *better_match_array) {
//   if(better_match_array->data != &better_match_array->inline_data[0]) {
//     xfree(better_match_array->data);
//   }
// }

static void
path_array_init(PathArray *path_array, uint32_t capa) {
  path_array->data = RB_ZALLOC_N(Path, capa);
  path_array->capa = capa;
  // 0 is the empty list
  path_array->len = 1;
}

static void
path_array_destroy(PathArray *path_array) {
  xfree(path_array->data);
}

static uint32_t
path_array_push(PathArray *path_array, Path **path) {
  if(!(path_array->len < path_array->capa)) {
    uint32_t new_capa = 2 * path_array->capa;
    RB_REALLOC_N(path_array->data, Path, new_capa);
    path_array->capa = new_capa;
  }
  uint32_t index = path_array->len;
  path_array->data[index] = (Path) {0, };
  *path = &path_array->data[index];
  path_array->len++;
  return index;
}

static inline Path *
path_array_get(PathArray *path_array, uint32_t idx) {
  return &path_array->data[idx];
}

static VALUE
rb_change_set_new_full(ChangeType change_type, VALUE rb_old, VALUE rb_new,
                       Token *old_tokens, size_t old_start, size_t old_len,
                       Token *new_tokens, size_t new_start, size_t new_len)
{
  ChangeSet *change_set = RB_ALLOC(ChangeSet);
  change_set->change_type = change_type;
  change_set->rb_old = rb_old;
  change_set->rb_new = rb_new;
  change_set->old_len = old_len;
  change_set->new_len = new_len;
  change_set->old_tokens = NULL;
  change_set->new_tokens = NULL;

  // assert(old_len == 0 || new_len == 0 || old_len == new_len);

  if(old_len > 0) {
    change_set->old_tokens = RB_ALLOC_N(Token, old_len);
    memcpy(change_set->old_tokens, old_tokens + old_start, old_len * sizeof(Token));
  }

  if(new_len > 0) {
    change_set->new_tokens = RB_ALLOC_N(Token, new_len);
    memcpy(change_set->new_tokens, new_tokens + new_start, new_len * sizeof(Token));
  }
  // fprintf(stderr, "TOKEN SET %d/%d  %d/%d\n", old_start, old_len, new_start, new_len);

  
  return TypedData_Wrap_Struct(rb_cChangeSet, &change_set_type, change_set);
}

// static VALUE
// rb_change_set_new(ChangeType change_type, VALUE rb_input,
//                   TokenArray *tokens, size_t start, size_t len)
// {
//   switch(change_type) {
//     case CHANGE_TYPE_DEL:
//       return rb_change_set_new_full(change_type, rb_input, Qnil,
//                         tokens, start, len,
//                         NULL, 0, 0);
//     case CHANGE_TYPE_ADD:
//       return rb_change_set_new_full(change_type, Qnil, rb_input,
//                         NULL, 0, 0,
//                         tokens, start, len);
//     default:
//       return Qnil;                        
//   }
// }

static void
output_change_set(DiffContext *ctx) {
  if(ctx->tmp_tokens_new.len == 0) {
    if(ctx->tmp_tokens_old.len == 0) return;
    rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_DEL, ctx->rb_old, Qnil,
                                                        ctx->tmp_tokens_old.data, 0, ctx->tmp_tokens_old.len,
                                                        NULL, 0, 0));

  } else if(ctx->tmp_tokens_old.len == 0) {
    if(ctx->tmp_tokens_new.len == 0) return;
    rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_ADD, Qnil, ctx->rb_new,
                                                        NULL, 0, 0,
                                                        ctx->tmp_tokens_new.data, 0, ctx->tmp_tokens_new.len));
  } else {
    if(!ctx->tmp_tokens_old.non_eq && !ctx->tmp_tokens_new.non_eq) {
      assert(ctx->tmp_tokens_old.len == ctx->tmp_tokens_new.len);
      rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_EQL, ctx->rb_old, ctx->rb_new,
                                                          ctx->tmp_tokens_old.data, 0, ctx->tmp_tokens_old.len,
                                                          ctx->tmp_tokens_new.data, 0, ctx->tmp_tokens_new.len));
    } else {
      if(ctx->output_replace) {
        rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_SUB, ctx->rb_old, ctx->rb_new,
                                                            ctx->tmp_tokens_old.data, 0, ctx->tmp_tokens_old.len,
                                                            ctx->tmp_tokens_new.data, 0, ctx->tmp_tokens_new.len));
      } else {
        rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_DEL, ctx->rb_old, Qnil,
                                                        ctx->tmp_tokens_old.data, 0, ctx->tmp_tokens_old.len,
                                                        NULL, 0, 0));
        rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_ADD, Qnil, ctx->rb_new,
                                                        NULL, 0, 0,
                                                        ctx->tmp_tokens_new.data, 0, ctx->tmp_tokens_new.len));
      }

      /* FIXME: we have a choice how to align old and new here
         at this point it stupidliy aligns at the beginning */
      // uint32_t common_len = MIN(ctx->tmp_tokens_new.len, ctx->tmp_tokens_old.len);
      // rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_MOD, ctx->rb_old, ctx->rb_new,
      //                                                     ctx->tmp_tokens_old.data, 0, common_len,
      //                                                     ctx->tmp_tokens_new.data, 0, common_len));
      // if(common_len < ctx->tmp_tokens_old.len) {
      //   rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_DEL, ctx->rb_old, Qnil,
      //                                                       ctx->tmp_tokens_old.data, common_len, ctx->tmp_tokens_old.len - common_len,
      //                                                       NULL, 0, 0));
      // } else if(common_len < ctx->tmp_tokens_new.len) {
      //   rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_ADD, Qnil, ctx->rb_new,
      //                                                       NULL, 0, 0,
      //                                                       ctx->tmp_tokens_new.data, common_len, ctx->tmp_tokens_new.len - common_len));
      // }
    }
  }


  // //FIXME: splitting modification is tricky...                    
  // if(change_type == CHANGE_TYPE_MOD || change_type == CHANGE_TYPE_EQL) {
  // } else {
  //   size_t start, len;
  //   TokenArray *tokens;
  //   VALUE rb_input;
  //   switch(change_type) {
  //     case CHANGE_TYPE_ADD: {
  //       start = start_new;
  //       len = len_new;
  //       tokens = &ctx->tokens_new;
  //       rb_input = ctx->rb_new;
  //       break;
  //     }
  //     case CHANGE_TYPE_DEL: {
  //       start = start_old;
  //       len = len_old;
  //       tokens = &ctx->tokens_old;
  //       rb_input = ctx->rb_old;
  //       break;
  //     }
  //     default: {
  //       rb_raise(rb_eRuntimeError, "unexpected change type");
  //     }
  //   }

  //   size_t end = start + len;
  //   size_t next_start = start;

  //   // for(size_t i = start; i < end; i++) {
  //   //   Token *token = &tokens->data[i];
  //   //   if(token->before_newline) {
  //   //     rb_ary_push(rb_out_ary, rb_change_set_new(change_type, rb_input, tokens, next_start, i - next_start + 1));
  //   //     next_start = i + 1;
  //   //   }
  //   // }

  //   if(next_start < end) {
  //     rb_ary_push(ctx->rb_out_ary, rb_change_set_new(change_type, rb_input, tokens, next_start, end - next_start));
  //   }
  // }
}


// static void
// change_set_from_tree(DiffContext *ctx, uint32_t root_idx, VALUE rb_ary) {
//   // empty node
//   if(root_idx == 0) {
//     return;
//   }

//   ChangeSetTree *tree = &ctx->change_set_trees.data[root_idx];

//   if(tree->change_type == CHANGE_TYPE_MOD || tree->change_type == CHANGE_TYPE_EQL) {
//     if(tree->change_type == CHANGE_TYPE_MOD || (tree->change_type ==CHANGE_TYPE_EQL && ctx->output_eq)) {
//       rb_ary_push(rb_ary, rb_change_set_new_full(tree->change_type, ctx->rb_old, ctx->rb_new,
//                                                   &ctx->tokens_old, tree->start_old, tree->len_old,
//                                                   &ctx->tokens_new, tree->start_new, tree->len_new));
//     }
//   } else {
//     size_t start, len;
//     TokenArray *tokens;
//     VALUE rb_input;
//     switch(tree->change_type) {
//       case CHANGE_TYPE_ADD: {
//         start = tree->start_new;
//         len = tree->len_new;
//         tokens = &ctx->tokens_new;
//         rb_input = ctx->rb_new;
//         break;
//       }
//       case CHANGE_TYPE_DEL: {
//         start = tree->start_old;
//         len = tree->len_old;
//         tokens = &ctx->tokens_old;
//         rb_input = ctx->rb_old;
//         break;
//       }
//       default: {
//         rb_raise(rb_eRuntimeError, "unexpected change type %d", tree->change_type);
//       }
//     }
//     size_t end = start + len;
//     rb_ary_push(rb_ary, rb_change_set_new(tree->change_type, rb_input, tokens, start, end - start));
//   }

//   change_set_from_tree(ctx, tree->children[0], rb_ary);
//   change_set_from_tree(ctx, tree->children[1], rb_ary);
// }

// static void
// output_change_set(DiffContext *ctx, ChangeType change_type, size_t start_old, size_t len_old, size_t start_new, size_t len_new) {

//   //FIXME: splitting modification is tricky...                    
//   if(change_type == CHANGE_TYPE_MOD || change_type == CHANGE_TYPE_EQL || !ctx->split_lines) {
//     rb_ary_push(ctx->rb_out_ary, rb_change_set_new_full(change_type, ctx->rb_old, ctx->rb_new,
//                                                         &ctx->tokens_old, start_old, len_old,
//                                                         &ctx->tokens_new, start_new, len_new));
//   } else {
//     size_t start, len;
//     TokenArray *tokens;
//     VALUE rb_input;
//     switch(change_type) {
//       case CHANGE_TYPE_ADD: {
//         start = start_new;
//         len = len_new;
//         tokens = &ctx->tokens_new;
//         rb_input = ctx->rb_new;
//         break;
//       }
//       case CHANGE_TYPE_DEL: {
//         start = start_old;
//         len = len_old;
//         tokens = &ctx->tokens_old;
//         rb_input = ctx->rb_old;
//         break;
//       }
//       default: {
//         rb_raise(rb_eRuntimeError, "unexpected change type");
//       }
//     }

//     size_t end = start + len;
//     size_t next_start = start;

//     // for(size_t i = start; i < end; i++) {
//     //   Token *token = &tokens->data[i];
//     //   if(token->before_newline) {
//     //     rb_ary_push(rb_out_ary, rb_change_set_new(change_type, rb_input, tokens, next_start, i - next_start + 1));
//     //     next_start = i + 1;
//     //   }
//     // }

//     if(next_start < end) {
//       rb_ary_push(ctx->rb_out_ary, rb_change_set_new(change_type, rb_input, tokens, next_start, end - next_start));
//     }
//   }
// }


// static void
// index_old(DiffContext *ctx, uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new) {
//   st_clear(ctx->old_index_map);
//   ctx->index_list.len = 0;

//   for(size_t iold = start_old; iold < start_old + len_old; iold++) {
//     Token *token = &ctx->tokens_old.data[iold];

//     IndexKey *key = &ctx->table_entries_old[iold];
//     key->start_byte = token->start_byte;
//     key->end_byte = token->end_byte;
//     key->input = ctx->input_old;

//     UpdateArg arg = {
//       .index_list = &ctx->index_list,
//       .value = iold,
//     };

//     st_update(ctx->old_index_map, (st_data_t) key, update_callback, (st_data_t) &arg);
//   }
// }

typedef struct {
  int64_t left;
  int64_t right;
  int64_t top;
  int64_t bottom;
} Box;

typedef struct {
  int64_t x1;
  int64_t y1;
  int64_t x2;
  int64_t y2;
} Snake;


#define BOX_WIDTH(b) (b->right - b->left)
#define BOX_HEIGHT(b) (b->bottom - b->top)
#define BOX_SIZE(b) (BOX_WIDTH(b) + BOX_HEIGHT(b))
#define BOX_DELTA(b) (BOX_WIDTH(b) - BOX_HEIGHT(b))

#define VGET(v, i) (v[(i) < 0 ? ((i) + (vlen)) : (i)])
#define VSET(v, i, val) (v[(i) < 0 ? ((i) + (vlen)) : (i)] = val)

static bool
forward(DiffContext *ctx, Box *box, int64_t *vf, int64_t *vb, int64_t d, int64_t vlen, Snake *snake) {
  for(int64_t k = d; k >= -d; k -= 2) {
    int64_t c = k - BOX_DELTA(box);
    int64_t px, x, y, py;

    if(k == -d || (k != d && VGET(vf, k - 1) < VGET(vf, k + 1))) {
      px = x = VGET(vf, k + 1);
    }
    else {
      px = VGET(vf, k - 1);
      x  = px + 1;
    }

    y = box->top + (x - box->left) - k;
    py = (d == 0 || x != px) ? y : y - 1;

    while(x < box->right && y < box->bottom && token_eql(&ctx->tokens_old_[x], ctx->input_old, &ctx->tokens_new_[y], ctx->input_new)) {
      x++;
      y++;
    }

    VSET(vf, k, x);

    if(BOX_DELTA(box) % 2 != 0 && (c >= -(d - 1) && c <= d - 1) && y >= VGET(vb, c)) {
      *snake = (Snake){px, py, x, y};
      return true;
    }
  }
  return false;
}


static bool
backward(DiffContext *ctx, Box *box, int64_t *vf, int64_t *vb, int64_t d, int64_t vlen, Snake *snake) {
  for(int64_t c = d; c >= -d; c -= 2) {
    int64_t k = c + BOX_DELTA(box);
    int64_t px, x, y, py;

    if(c == -d || (c != d && VGET(vb, c - 1) > VGET(vb, c + 1))) {
      py = y = VGET(vb, c + 1);
    }
    else {
      py = VGET(vb, c - 1);
      y  = py - 1;
    }

    x = box->left + (y - box->top) + k;
    px = (d == 0 || y != py) ? x : x + 1;

    while(x > box->left && y > box->top && token_eql(&ctx->tokens_old_[x - 1], ctx->input_old, &ctx->tokens_new_[y - 1], ctx->input_new)) {
      x--;
      y--;
    }

    VSET(vb, c, y);

    if(BOX_DELTA(box) % 2 == 0 && (k >= -d && k <= d) && x <= VGET(vf, k)) {
      *snake = (Snake){x, y, px, py};
      return true;
    }
  }
  return false;
}

static bool
midpoint(DiffContext *ctx, Box *box, Snake *snake) {
  if(BOX_SIZE(box) == 0) return false;

  int64_t max = (BOX_SIZE(box) + 1) / 2;
  int64_t vlen = 2 * max + 1;
  int64_t *vf_vb = RB_ZALLOC_N(int64_t, 2 * vlen);
  int64_t *vf = vf_vb + 0;
  int64_t *vb = vf_vb + vlen;
  bool retval = false;

  vf[1] = box->left;
  vb[1] = box->bottom;

  for(int64_t d = 0; d <= max; d++) {
    if(forward(ctx, box, vf, vb, d, vlen, snake)) {
      retval = true;
      goto done;
    }
    if(backward(ctx, box, vf, vb, d, vlen, snake)) {
      retval = true;
      goto done;
    }
  }

done:
  xfree(vf_vb);
  return retval;
}


static PathIdx
find_path(DiffContext *ctx, int64_t left, int64_t top, int64_t right, int64_t bottom) {
  Box box = {
    .left = left,
    .top = top,
    .right = right,
    .bottom = bottom
  };
  Snake snake;
  if(!midpoint(ctx, &box, &snake)) {
    return 0;
  } 

  int64_t start_x = snake.x1, start_y = snake.y1, finish_x = snake.x2, finish_y = snake.y2;

  assert(!(start_x == right && start_y == bottom));

  PathIdx head_idx = find_path(ctx, box.left, box.top, start_x, start_y);
  PathIdx tail_idx = find_path(ctx, finish_x, finish_y, box.right, box.bottom);

  if(head_idx == 0) {
    Path *head;
    head_idx = path_array_push(&ctx->path_array, &head);
    head->x = start_x;
    head->y = start_y;
  }

  if(tail_idx == 0) {
    Path *tail;
    tail_idx = path_array_push(&ctx->path_array, &tail);
    tail->x = finish_x;
    tail->y = finish_y;
  }

  PathIdx iter_idx = head_idx;
  while(true) {
    Path *iter = path_array_get(&ctx->path_array, iter_idx);
    if(!iter->next) {
      iter->next = tail_idx;
      break;
    } else {
      iter_idx = iter->next;
    }
  }

  return head_idx;
}

static void
call_cb(DiffContext *ctx, int64_t x1, int64_t y1, int64_t x2, int64_t y2) {
  CallbackType type;
  Token *token_old = NULL, *token_new = NULL;
  if(x1 == x2) {
    type = CALLBACK_INS;
    token_new = &ctx->tokens_new_[y1];
  } else if(y1 == y2) {
    type = CALLBACK_DEL;
    token_old = &ctx->tokens_old_[x1];
  } else {
    type = CALLBACK_EQ;
    token_old = &ctx->tokens_old_[x1];
    token_new = &ctx->tokens_new_[y1];
  }

  ctx->cb(ctx, type, token_old, token_new);
}

static void
walk_diagonal(DiffContext *ctx, int64_t x1, int64_t y1, int64_t x2, int64_t y2, int64_t *out_x1, int64_t *out_y1) {
  while(x1 < x2 && y1 < y2 && token_eql(&ctx->tokens_old_[x1], ctx->input_old, &ctx->tokens_new_[y1], ctx->input_new)) {
    call_cb(ctx, x1, y1, x1 + 1, y1 + 1);
    x1++;
    y1++;
  }
  *out_x1 = x1;
  *out_y1 = y1;
}

// static void
// free_path(Path *path) {
//   if(path->next != NULL) {
//     free_path(path->next);
//   }
//   xfree(path);
// }

static void 
walk_snakes(DiffContext *ctx, uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new) {
  PathIdx path_idx = find_path(ctx, start_old, start_new, len_old, len_new);
  if(path_idx == 0) return;

  int64_t x1, y1, x2, y2;
  PathIdx iter_idx = path_idx;
  Path *iter;

  iter = path_array_get(&ctx->path_array, iter_idx);
  x1 = iter->x;
  y1 = iter->y;
  iter_idx = iter->next;
  iter = path_array_get(&ctx->path_array, iter_idx);
  x2 = iter->x;
  y2 = iter->y;
  iter_idx = iter->next;

  while(true) {
    // fprintf(stderr, "%d %d %d %d\n", x1, y1, x2, y2);

    walk_diagonal(ctx, x1, y1, x2, y2, &x1, &y1);
    int64_t d = (x2 - x1) - (y2 - y1);
    if(d < 0) {
      call_cb(ctx, x1, y1, x1, y1 + 1);
      y1++;
    } else if(d > 0) {
      call_cb(ctx, x1, y1, x1 + 1, y1);
      x1++;
    }
    walk_diagonal(ctx, x1, y1, x2, y2, &x1, &y1);

    if(iter_idx == 0) {
      break;
    } else {
      x1 = x2;
      y1 = y2;
      iter = path_array_get(&ctx->path_array, iter_idx);
      x2 = iter->x;
      y2 = iter->y;
      iter_idx = iter->next;
    }
  } 

  // free_path(path);
}

static void 
collect_change_sets(DiffContext *ctx, CallbackType type, Token *token_old, Token *token_new) {
  switch(type) {
    case CALLBACK_START:
      tmp_token_array_reset(&ctx->tmp_tokens_new);
      tmp_token_array_reset(&ctx->tmp_tokens_old);
      break;
    case CALLBACK_FINISH:
      // fprintf(stderr, "FINISH\n");
      output_change_set(ctx);
      break;
    case CALLBACK_DEL:
      // fprintf(stderr, "token_old DEL (%d): %d-%d %.*s\n", token_old->implicit, token_old->start_byte, token_old->end_byte, token_old->end_byte - token_old->start_byte, ctx->input_old + token_old->start_byte);
      add_tmp_token(&ctx->tmp_tokens_old, *token_old, type);
      break;
    case CALLBACK_EQ:
      output_change_set(ctx);
      // fprintf(stderr, "token EQ (%d): %.*s\n", token_old->implicit, token_old->end_byte - token_old->start_byte, ctx->input_old + token_old->start_byte);
      // fprintf(stderr, "token EQ (%d): %.*s\n", token_new->implicit, token_new->end_byte - token_new->start_byte, ctx->input_new + token_new->start_byte);
      // fprintf(stderr, "RESET\n");
      tmp_token_array_reset(&ctx->tmp_tokens_new);
      tmp_token_array_reset(&ctx->tmp_tokens_old);
      if(ctx->output_eq) {
        add_tmp_token(&ctx->tmp_tokens_old, *token_old, type);
        add_tmp_token(&ctx->tmp_tokens_new, *token_new, type);
      }
      break;
    case CALLBACK_INS:
      // fprintf(stderr, "token_new INS (%d): %.*s\n", token_new->implicit, token_new->end_byte - token_new->start_byte, ctx->input_new + token_new->start_byte);
      add_tmp_token(&ctx->tmp_tokens_new, *token_new, type);
      break;
  }

}

static void
token_diff2(DiffContext *ctx, uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new) {
  ctx->cb = collect_change_sets;
  ctx->cb(ctx, CALLBACK_START, NULL, NULL);
  walk_snakes(ctx, start_old, len_old, start_new, len_new);
  ctx->cb(ctx, CALLBACK_FINISH, NULL, NULL);
}


// // Loosely based on https://github.com/paulgb/simplediff
// static uint32_t
// token_diff(DiffContext *ctx, uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new) {

//   if(len_old == 0 && len_new == 0) return 0;

//   // uint32_t sub_start_old = start_old;
//   // uint32_t sub_start_new = start_new;
//   uint32_t best_sub_length = 0;
//   BetterMatchArray better_matches;
//   better_match_array_init(&better_matches);
//   // for(size_t i = 0; i < ctx->tokens_new.len; i++) {
//   //   Token *token = &ctx->tokens_new.data[i];
//   //   fprintf(stderr, "TOKEN NEW (%d): %.*s\n", token->implicit, token->end_byte - token->start_byte, ctx->input_new + token->start_byte);
//   //   if(token->before_newline) {
//   //     fprintf(stderr, "BEFORE NEWLINE\n");
//   //   }
//   // }

//   // for(size_t i = 0; i < ctx->tokens_old.len; i++) {
//   //   Token *token = &ctx->tokens_old.data[i];
//   //   fprintf(stderr, "TOKEN OLD (%d): %.*s\n", token->implicit, token->end_byte - token->start_byte, ctx->input_old + token->start_byte);
//   //   if(token->before_newline) {
//   //     fprintf(stderr, "BEFORE NEWLINE\n");
//   //   }
//   // }

//   if(len_old > 0) {
//     // st_clear(next_match_map);
//     // st_clear(match_map);
//     match_map_reset(&ctx->next_match_map);
//     match_map_reset(&ctx->match_map);

//     index_old(ctx, start_old, len_old, start_new, len_new);


//     for(size_t inew = start_new; inew < start_new + len_new; inew++) {
//       Token *token = &ctx->tokens_new.data[inew];

//     //  fprintf(stderr, "TOKEN NEW: %.*s %d\n", token->end_byte - token->start_byte, input_new + token->start_byte, inew);
//     //   if(token->before_newline) {
//     //      fprintf(stderr, "HAVE FOUND NEWLINE TOKEN\n");
//     //     //  abort();
//     //   }

//       // assert(next_match_map->num_entries == 0);

//       IndexKey key = {
//         .start_byte = token->start_byte,
//         .end_byte = token->end_byte,
//         .input = ctx->input_new
//       };

//       st_data_t pair;
//       // uint32_t max_new_sub_len = 0;

//       // NOTE: at this point, next_match_map but be fresh, i.e., in a post-reset() state

//       if(st_lookup(ctx->old_index_map, (st_data_t)&key, &pair)) {
//         while(true) {
//           uint32_t value = PAIR64_FIRST(pair);
//           uint32_t next_index = PAIR64_SECOND(pair);

//           uint32_t iold = value;

//           uint32_t prev_sub_len = 0;

//           if(iold > start_old) {
//             prev_sub_len = ctx->match_map.data[iold - 1].len;
//             // st_lookup(match_map, (st_data_t) (iold - 1), (st_data_t *) &prev_sub_len);
//           }

//           /*if(!(prev_sub_len == 0 && token->dont_start))*/ {
//             uint32_t new_sub_len = prev_sub_len + 1;
//             uint32_t new_sub_start_old = iold - new_sub_len + 1;
//             uint32_t new_sub_start_new = inew - new_sub_len + 1;

//             // bool new_sub_ends_at_newline = false;
//             // bool cur_sub_ends_at_newline = false;

//             // if(sub_length > 0) {
//             //   int64_t cur_sub_last_token_index = sub_start_new + sub_length - 1;
//             //   int64_t new_sub_last_token_index = new_sub_start_new + new_sub_len - 1;

//             //   new_sub_ends_at_newline = new_sub_last_token_index > 0 && tokens_new->data[new_sub_last_token_index].before_newline;
//             //   cur_sub_ends_at_newline = cur_sub_last_token_index > 0 && tokens_new->data[cur_sub_last_token_index].before_newline;
//             // }


//             if(new_sub_len >= best_sub_length) { // || (new_sub_len == sub_length && !cur_sub_ends_at_newline && new_sub_ends_at_newline)) {

//                 // if(new_sub_len == sub_length && (new_sub_ends_at_newline || !cur_sub_ends_at_newline)) {
//                 //   fprintf(stderr, "HAVE FOUND NEWLINE TOKEN\n");
//                 //   fprintf(stderr, "preferring (%d, %d) over (%d, %d)\n", new_sub_start_old, new_sub_len, sub_start_old, sub_length);
//                 // }

//                 // we need to find the *best* matches
//                 // but we do not know until fully through the loop
//                 // so either we go through twice, or we store all 'better' matches
//                 // and eventually only consider the best
//                 BetterMatch better_match = {
//                   .sub_len = new_sub_len,
//                   .sub_start_old = new_sub_start_old,
//                   .sub_start_new = new_sub_start_new,
//                 };
//                 best_sub_length = new_sub_len;
//                 // sub_start_old = new_sub_start_old;
//                 // sub_start_new = new_sub_start_new;

//                 better_match_array_insert(&better_matches, better_match);

//                 // fprintf(stderr, "%d => %d\n", new_sub_start_old, iold - sub_length + 1);
//                 // fprintf(stderr, "%d => %d\n", new_sub_start_new, inew - sub_length + 1);
//                 // sub_start_old = iold - sub_length + 1;
//                 // sub_start_new = inew - sub_length + 1;
//             }
//             // assert(best_sub_length <= len_old);
//             // assert(best_sub_length <= len_new);
//             // assert(sub_start_old >= start_old);
//             // assert(sub_start_new >= start_new);
//             // st_insert(next_match_map, (st_data_t) iold, new_sub_len);
//             match_map_insert(&ctx->next_match_map, iold, new_sub_len);
//           }

//           if(next_index == UINT32_MAX) {
//             break;
//           } else {
//             pair = ctx->index_list.entries[next_index];
//           }
//         }
//       }

//       {
//         MatchMap tmp = ctx->match_map;
//         ctx->match_map = ctx->next_match_map;
//         ctx->next_match_map = tmp;

//         match_map_reset(&ctx->next_match_map);
//         // st_clear(next_match_map);
//       }
//     }
//   }

//   uint32_t ret_tree_idx = UINT32_MAX;

//   if(best_sub_length == 0) {
//     uint32_t tree_idxs[3] = {0,};
//     uint64_t cost = 0;

//     size_t common_len = MIN(len_old, len_new);
//     if(common_len > 0) {
//       tree_idxs[0] = change_set_tree_array_push(&ctx->change_set_trees);
//       ChangeSetTree *tree = change_set_tree_array_get(&ctx->change_set_trees, tree_idxs[0]);
//       tree->change_type = CHANGE_TYPE_MOD;
//       tree->cost = 2 * common_len;
//       tree->start_old = start_old;
//       tree->len_old = common_len;
//       tree->start_new = start_new;
//       tree->len_new = common_len;
//       cost += tree->cost;

//       // output_change_set(ctx, CHANGE_TYPE_MOD,
//       //                   start_old, common_len,
//       //                   start_new, common_len);

//       // rb_ary_push(rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_MOD, rb_input_old, rb_input_new,
//       //                                                tokens_old, start_old, common_len,
//       //                                                tokens_new, start_new, common_len));
//       start_old += common_len;
//       start_new += common_len;
//       len_old -= common_len;
//       len_new -= common_len;
//     }


//     if(len_old > 0) {
//       tree_idxs[1] = change_set_tree_array_push(&ctx->change_set_trees);
//       ChangeSetTree *tree = change_set_tree_array_get(&ctx->change_set_trees, tree_idxs[1]);
//       tree->change_type = CHANGE_TYPE_DEL;
//       tree->cost = len_old;
//       tree->start_old = start_old;
//       tree->len_old = len_old;
//       tree->start_new = 0;
//       tree->len_new = 0;
//       cost += tree->cost;


//       // rb_ary_push(rb_out_ary, rb_change_set_new(CHANGE_TYPE_DEL, rb_input_old, tokens_old, start_old, len_old));
//       // output_change_set(ctx, CHANGE_TYPE_DEL,
//       //                   start_old, len_old,
//       //                   0, 0);
//     }

//     if(len_new > 0) {
//       tree_idxs[2] = change_set_tree_array_push(&ctx->change_set_trees);
//       ChangeSetTree *tree = change_set_tree_array_get(&ctx->change_set_trees, tree_idxs[2]);
//       tree->change_type = CHANGE_TYPE_ADD;
//       tree->cost = len_new;
//       tree->start_old = 0;
//       tree->len_old = 0;
//       tree->start_new = start_new;
//       tree->len_new = len_new;
//       cost += tree->cost;
//       // rb_ary_push(rb_out_ary, rb_change_set_new(CHANGE_TYPE_ADD, rb_input_new, tokens_new, start_new, len_new));
//       // output_change_set(ctx, CHANGE_TYPE_ADD,
//       //                   0, 0,
//       //                   start_new, len_new);
//     }

//     if(tree_idxs[0] != 0) {
//       ret_tree_idx = tree_idxs[0];
//       ChangeSetTree *tree = change_set_tree_array_get(&ctx->change_set_trees, ret_tree_idx);
//       tree->children[0] = tree_idxs[1];
//       tree->children[1] = tree_idxs[2];
//       tree->cost = cost;
//     } else if(tree_idxs[1] != 0) {
//       ret_tree_idx = tree_idxs[1];
//       ChangeSetTree *tree = change_set_tree_array_get(&ctx->change_set_trees, ret_tree_idx);
//       tree->children[0] = tree_idxs[2];
//       tree->cost = cost;
//     } else {
//       ret_tree_idx = tree_idxs[2];
//       ChangeSetTree *tree = change_set_tree_array_get(&ctx->change_set_trees, ret_tree_idx);
//       tree->cost = cost;
//     }
//     // fprintf(stderr, "BASE COST: %lld\n", cost);
//   } else {

//     assert(best_sub_length <= len_old);
//     assert(best_sub_length <= len_new);

//     uint64_t best_cost = UINT64_MAX;
//     // fprintf(stderr, "----------------- %d %d\n", better_matches.len, best_sub_length);
//     for(uint32_t i = 0; i < better_matches.len; i++) {
//       BetterMatch *better_match = &better_matches.data[i];
//       uint32_t sub_length = better_match->sub_len;
//       bool is_best = sub_length == best_sub_length;
//       // fprintf(stderr, "%d MATCH WITH %d (%d) (%d)\n", i, better_match->sub_len, is_best, best_sub_length);
//       if(is_best) {

//         uint32_t sub_start_old = better_match->sub_start_old;
//         uint32_t sub_start_new = better_match->sub_start_new;

//         uint32_t left_start_old = start_old;
//         uint32_t left_len_old = sub_start_old - start_old;
//         uint32_t left_start_new = start_new;
//         uint32_t left_len_new = sub_start_new - start_new;

//         uint32_t right_start_old = sub_start_old + sub_length;
//         uint32_t right_len_old = (start_old + len_old) - (sub_start_old + sub_length);
//         uint32_t right_start_new = sub_start_new + sub_length;
//         uint32_t right_len_new = (start_new + len_new) - (sub_start_new + sub_length);

//         assert(left_start_old >= start_old);
//         assert(left_start_old + left_len_old < right_start_old);
//         assert(right_len_old <= len_old);

//         assert(left_start_new >= start_new);
//         assert(left_start_new + left_len_new < right_start_new);
//         assert(right_len_new <= len_new);

//         assert(right_len_new + left_len_new < len_new || right_len_old + right_len_old < len_old);

//         fprintf(stderr, "%ld\n", right_len_new + left_len_new + right_len_old + right_len_old);

//         fprintf(stderr, "N%u_%u__%u_%u -> N%u_%u__%u_%u;\n", start_old, len_old, start_new, len_new,  
//                                                              left_start_old, left_len_old, left_start_new, left_len_new);

//         // raise(SIGTRAP);

//         uint32_t left_idx = token_diff(ctx, left_start_old, left_len_old, left_start_new, left_len_new);

//         if(ctx->output_eq) {


//           // output_change_set(ctx, CHANGE_TYPE_EQL,
//           //                   sub_start_old, sub_length,
//           //                   sub_start_new, sub_length);

//           // rb_ary_push(rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_EQL, rb_input_old, rb_input_new,
//           //                                                tokens_old, sub_start_old, sub_length,
//           //                                                tokens_new, sub_start_new, sub_length));
//         }


//         fprintf(stderr, "N%u_%u__%u_%u -> N%u_%u__%u_%u;\n", start_old, len_old, start_new, len_new,
//                                                              right_start_old, right_len_old, right_start_new, right_len_new);

//         uint32_t right_idx = token_diff(ctx, right_start_old, right_len_old, right_start_new, right_len_new);

//         uint64_t cost = ctx->change_set_trees.data[left_idx].cost + ctx->change_set_trees.data[right_idx].cost;

//         if(cost < best_cost) {
//           ret_tree_idx = change_set_tree_array_push(&ctx->change_set_trees);
//           ChangeSetTree *tree = change_set_tree_array_get(&ctx->change_set_trees, ret_tree_idx);

//           tree->change_type = CHANGE_TYPE_EQL;
//           tree->cost = cost;
//           tree->start_old = sub_start_old;
//           tree->len_old = best_sub_length;
//           tree->start_new = sub_start_new;
//           tree->len_new = best_sub_length;
//           tree->children[0] = left_idx;
//           tree->children[1] = right_idx;

//           best_cost = cost;
//           // fprintf(stderr, "BEST COST: %lld\n", best_cost);
//         }
//       }
//     }
//     better_match_array_destroy(&better_matches);
//   }

//   assert(ret_tree_idx != 0);
//   return ret_tree_idx;
// }

static void
rb_change_set_get(ChangeSet *change_set, long index, VALUE *rb_old_token, VALUE *rb_new_token) {
  *rb_old_token = Qnil;
  *rb_new_token = Qnil;

  switch(change_set->change_type) {
    case CHANGE_TYPE_EQL:
    case CHANGE_TYPE_SUB:
      *rb_old_token = index < change_set->old_len ? rb_new_token_from_ptr(&change_set->old_tokens[index]) : Qnil;
      *rb_new_token = index < change_set->new_len ? rb_new_token_from_ptr(&change_set->new_tokens[index]) : Qnil;
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
rb_change_set_change_count(VALUE self) {
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  long len = change_set->new_len + change_set->old_len;
  return LONG2FIX(len);
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
      type_id = id_eql;
      break;
    case CHANGE_TYPE_SUB:
      type_id = id_sub;
      break;
    default:
      return Qnil;
  }

  return ID2SYM(type_id);
}

static VALUE
rb_ts_diff_diff_s(VALUE self, VALUE rb_old, VALUE rb_new,
                  VALUE rb_output_eq, VALUE rb_output_replace, VALUE rb_ignore_whitespace, VALUE rb_ignore_comments) {

  // FIXME: check node
  // Check_Type(rb_old, T_STRING);
  // Check_Type(rb_new, T_STRING);

  DiffContext ctx;

  ctx.output_eq = RB_TEST(rb_output_eq);
  ctx.output_replace = RB_TEST(rb_output_replace);
  ctx.split_lines = false; //RB_TEST(rb_split_lines);
  bool ignore_whitespace = RB_TEST(rb_ignore_whitespace);
  bool ignore_comments = RB_TEST(rb_ignore_comments);
  VALUE rb_out_ary;

  uint32_t input_old_start;
  uint32_t input_new_start;
  uint32_t input_old_len;
  uint32_t input_new_len;

  ctx.input_old = rb_node_input_(rb_old, &input_old_start, &input_old_len);
  ctx.input_new = rb_node_input_(rb_new, &input_new_start, &input_new_len);
  ctx.rb_new = rb_new;
  ctx.rb_old = rb_old;

  rb_out_ary = rb_ary_new();

  if(input_old_len == input_new_len && !memcmp(ctx.input_old + input_old_start, ctx.input_new + input_new_start, input_new_len)) {
    return rb_out_ary;
  }

  ctx.rb_out_ary = rb_out_ary;
  ctx.tokens_old = rb_node_tokenize_(rb_old, ignore_whitespace, ignore_comments);
  ctx.tokens_new = rb_node_tokenize_(rb_new, ignore_whitespace, ignore_comments);

  RB_GC_GUARD(rb_old);
  RB_GC_GUARD(rb_new);

  ssize_t tokens_old_len = (ssize_t) ctx.tokens_old.len;
  ssize_t tokens_new_len = (ssize_t) ctx.tokens_new.len;

  // ctx.table_entries_old = RB_ALLOC_N(IndexKey, tokens_old_len);

  // ctx.index_list.capa = ctx.tokens_old.len;
  // ctx.index_list.len = 0;
  // ctx.index_list.entries = RB_ALLOC_N(st_data_t, ctx.index_list.capa);

  // for(size_t i = 0; i < tokens_new_len; i++) {
  //   Token *token = &tokens_new[i];
  //   fprintf(stderr, "TOKEN OLD: %.*s\n", token->end_byte - token->start_byte, input_new + token->start_byte);
  //   if(token->before_newline) {
  //     fprintf(stderr, "BEFORE NEWLINE\n");
  //   }
  // }


  // match_map_init(&ctx.match_map, ctx.tokens_old.len);
  // match_map_init(&ctx.next_match_map, ctx.tokens_old.len);
  // change_set_tree_array_init(&ctx.change_set_trees, 512);
  // ctx.old_index_map = st_init_table(&type_index_key_hash);

  path_array_init(&ctx.path_array, 512);
  tmp_token_array_init(&ctx.tmp_tokens_new, 128);
  tmp_token_array_init(&ctx.tmp_tokens_old, 128);

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

  //  token_diff2(&ctx,
  //                                prefix_len, ctx.tokens_old.len - suffix_len - prefix_len,
  //                                prefix_len, ctx.tokens_new.len - suffix_len - prefix_len);

  if(ctx.output_eq && prefix_len > 0) {
      rb_ary_push(ctx.rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_EQL, ctx.rb_old, ctx.rb_new,
                                                          ctx.tokens_old.data, 0, prefix_len,
                                                          ctx.tokens_new.data, 0, prefix_len));
  }

  ctx.tokens_new_ = ctx.tokens_new.data + prefix_len;
  ctx.tokens_old_ = ctx.tokens_old.data + prefix_len;
  token_diff2(&ctx, 0, ctx.tokens_old.len - suffix_len - prefix_len ,
                    0, ctx.tokens_new.len - suffix_len - prefix_len);


  if(ctx.output_eq && suffix_len > 0) {
    rb_ary_push(ctx.rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_EQL, ctx.rb_old, ctx.rb_new,
                                                        ctx.tokens_old.data, ctx.tokens_old.len - suffix_len, suffix_len,
                                                        ctx.tokens_new.data, ctx.tokens_new.len - suffix_len, suffix_len));
  }

  //  ctx.tokens_new_ = ctx.tokens_new.data;
  //  ctx.tokens_old_ = ctx.tokens_old.data;
  //  token_diff2(&ctx, 0, ctx.tokens_old.len,
  //                 0, ctx.tokens_new.len);

  // uint32_t tree_idx = token_diff(&ctx,
  //                                prefix_len, ctx.tokens_old.len - suffix_len - prefix_len,
  //                                prefix_len, ctx.tokens_new.len - suffix_len - prefix_len);



  // token_diff(tokens_old, tokens_new, &index_list, table_entries_old, match_map, _overlap, old_index_map,
  //           rb_old, rb_new, 0, tokens_old_len, 0, tokens_new_len, rb_out_ary, output_eq);

  // change_set_from_tree(&ctx, tree_idx, rb_out_ary);


done:
  // st_free_table(match_map);
  // st_free_table(next_match_map);
  // match_map_destroy(&ctx.match_map);
  // match_map_destroy(&ctx.next_match_map);
  path_array_destroy(&ctx.path_array);
  tmp_token_array_destroy(&ctx.tmp_tokens_new);
  tmp_token_array_destroy(&ctx.tmp_tokens_old);
  xfree(ctx.tokens_old.data);
  xfree(ctx.tokens_new.data);

  return rb_out_ary;
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

typedef enum {
  PQ_ACTION_NONE,
  PQ_ACTION_INSERT,
  PQ_ACTION_DELETE,
} PQAction;


void
rb_node_pq_profile_(TSNode node, Tree *tree, PQAction action, VALUE rb_p, VALUE rb_q, VALUE rb_include_root_ancestors, VALUE rb_raw, VALUE rb_pairs, VALUE rb_only_named, VALUE rb_max_depth, VALUE rb_profile);

static void
tokens_to_pq_profile(Token *tokens, size_t tokens_len, PQAction action, VALUE rb_p, VALUE rb_q, VALUE rb_include_root_ancestors, VALUE rb_raw, VALUE rb_pairs, VALUE rb_only_named, VALUE rb_max_depth, VALUE rb_profile) {
  for(size_t i = 0; i < tokens_len; i++) {
    Token *token = &tokens[i];
    Tree *tree = rb_tree_unwrap(token->rb_tree);
    rb_node_pq_profile_(token->ts_node, tree, action, rb_p, rb_q, rb_include_root_ancestors, rb_raw, rb_pairs, rb_only_named, rb_max_depth, rb_profile); 
  }
}

static VALUE
rb_change_set_pq_profile(VALUE self, VALUE rb_p, VALUE rb_q, VALUE rb_profile, VALUE rb_include_root_ancestors, VALUE rb_raw, VALUE rb_pairs, VALUE rb_named_only, VALUE rb_max_depth)
{
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  if(RB_NIL_P(rb_profile)) {
    rb_profile = rb_ary_new_capa(64);
  } else {
    Check_Type(rb_profile, RUBY_T_ARRAY);
  }
  tokens_to_pq_profile(change_set->old_tokens, change_set->old_len, PQ_ACTION_DELETE, rb_p, rb_q, rb_include_root_ancestors, rb_raw, rb_pairs, rb_named_only, rb_max_depth, rb_profile);
  tokens_to_pq_profile(change_set->new_tokens, change_set->new_len, PQ_ACTION_INSERT, rb_p, rb_q, rb_include_root_ancestors, rb_raw, rb_pairs, rb_named_only, rb_max_depth, rb_profile);

  return rb_profile;
}


static VALUE
rb_change_set_old(VALUE self)
{
  ChangeSet *change_set;
  TypedData_Get_Struct(self, ChangeSet, &change_set_type, change_set);

  // uint32_t len = change_set_len(change_set);
  uint32_t len = change_set->old_len;
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

  // uint32_t len = change_set_len(change_set);
  uint32_t len = change_set->new_len;
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
Init_core(void)
{
  id_add = rb_intern("+");
  id_del = rb_intern("-");
  id_eql = rb_intern("=");
  id_sub = rb_intern("!");

  VALUE rb_mTreeSitter = rb_define_module("TreeSitter");
  rb_mTSDiff = rb_define_module_under(rb_mTreeSitter, "Diff");
  rb_eTsDiffError = rb_define_class_under(rb_mTSDiff, "Error", rb_eStandardError);

  rb_define_singleton_method(rb_mTSDiff, "__diff__", rb_ts_diff_diff_s, 6);

  rb_cChangeSet = rb_define_class_under(rb_mTSDiff, "ChangeSet", rb_cObject);
  rb_undef_alloc_func(rb_cChangeSet);

  rb_define_method(rb_cChangeSet, "[]", rb_change_set_aref, 1);
  rb_define_method(rb_cChangeSet, "size", rb_change_set_size, 0);
  rb_define_method(rb_cChangeSet, "change_count", rb_change_set_change_count, 0);
  rb_define_method(rb_cChangeSet, "type", rb_change_set_type, 0);
  rb_define_method(rb_cChangeSet, "old", rb_change_set_old, 0);
  rb_define_method(rb_cChangeSet, "new", rb_change_set_new_m, 0);
  rb_define_method(rb_cChangeSet, "each", rb_change_set_each, 0);
  rb_define_method(rb_cChangeSet, "__pq_profile__", rb_change_set_pq_profile, 8);
  rb_include_module(rb_cChangeSet, rb_mEnumerable);

  // rb_define_method(rb_cToken, "==", rb_token_eql, 1);
  // rb_define_method(rb_cToken, "eql?", rb_token_eql, 1);


}