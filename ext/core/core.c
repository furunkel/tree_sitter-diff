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
  uint32_t len;
} Match;

typedef struct {
  Match *data;
  uint32_t min_index;
  uint32_t max_index;
} MatchMap;

typedef struct {
  uint32_t sub_start_old;
  uint32_t sub_start_new;
  uint32_t sub_len;
} BetterMatch;

#define BETTER_MATCH_ARRAY_INLINE_CAPA 6

typedef struct {
  BetterMatch *data;
  BetterMatch inline_data[BETTER_MATCH_ARRAY_INLINE_CAPA];
  uint32_t len;
  uint32_t capa;
  uint32_t max;
} BetterMatchArray;

typedef struct {
  ChangeType change_type : 32;
  uint32_t start_old;
  uint32_t len_old;
  uint32_t start_new;
  uint32_t len_new;
  uint32_t children[2];
  uint64_t cost;
} ChangeSetTree;

typedef struct {
  ChangeSetTree *data;
  uint32_t len;
  uint32_t capa;
} ChangeSetTreeArray;

typedef struct {
  TokenArray tokens_old;
  TokenArray tokens_new;
  MatchMap match_map;
  MatchMap next_match_map;
  ChangeSetTreeArray change_set_trees;
  IndexValue index_list;
  IndexKey *table_entries_old;
  st_table *old_index_map;
  const char *input_old;
  const char *input_new;
  VALUE rb_old;
  VALUE rb_new;
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
match_map_init(MatchMap *map, uint32_t len) {
  map->data = RB_ZALLOC_N(Match, len);
  map->min_index = len;
  map->max_index = 0;
}

static void
match_map_destroy(MatchMap *map) {
  xfree(map->data);
}

static void
match_map_insert(MatchMap *map, uint32_t index, uint32_t len) {
  Match match = {
    .len = len
  };
  map->data[index] = match;
  map->max_index = MAX(map->max_index, index);
  map->min_index = MIN(map->min_index, index);
}

static void
match_map_reset(MatchMap *map) {
  if(map->min_index <= map->max_index) {
     memset(map->data + map->min_index, 0, (map->max_index - map->min_index + 1) * sizeof(Match));
  }
  // memset(map->data, 0, map->max_index * sizeof(uint32_t));
}

static void
better_match_array_init(BetterMatchArray *better_match_array) {
    better_match_array->data = &better_match_array->inline_data[0];
    better_match_array->capa = BETTER_MATCH_ARRAY_INLINE_CAPA;
    better_match_array->len = 0;
    better_match_array->max = 0;

}

static uint32_t
better_match_array_insert(BetterMatchArray *better_match_array, BetterMatch match) {

  if(better_match_array->max < match.sub_len) {
    better_match_array->data[0] = match;
    better_match_array->len = 1;
    better_match_array->max = match.sub_len;
    return 0;
  } else {
    if(!(better_match_array->len < better_match_array->capa)) {
      uint32_t new_capa = 2 * better_match_array->capa;
      if(better_match_array->data == &better_match_array->inline_data[0]) {
        better_match_array->data = RB_ALLOC_N(BetterMatch, new_capa);
        memcpy(better_match_array->data, &better_match_array->inline_data[0], sizeof(better_match_array->inline_data));
      } else {
        RB_REALLOC_N(better_match_array->data, BetterMatch, new_capa);
      }
      better_match_array->capa = new_capa;
    }

    uint32_t index = better_match_array->len;
    better_match_array->data[index] = match;
    better_match_array->len++;

    return index;
  }
}

// static uint32_t
// better_match_array_reset(BetterMatchArray *better_match_array) {
//   memset(better_match_array->data, 0, sizeof(BetterMatch) * better_match_array->len);
// }

static void
better_match_array_destroy(BetterMatchArray *better_match_array) {
  if(better_match_array->data != &better_match_array->inline_data[0]) {
    xfree(better_match_array->data);
  }
}

static void
change_set_tree_array_init(ChangeSetTreeArray *change_set_tree_array, uint32_t capa) {
  change_set_tree_array->data = RB_ZALLOC_N(ChangeSetTree, capa);
  change_set_tree_array->capa = capa;

  // we use the first element as a sentinel
  change_set_tree_array->len = 1;
}

static void
change_set_tree_array_destroy(ChangeSetTreeArray *change_set_tree_array) {
  xfree(change_set_tree_array->data);
}

static uint32_t
change_set_tree_array_push(ChangeSetTreeArray *change_set_tree_array, ChangeSetTree **tree) {
  if(!(change_set_tree_array->len < change_set_tree_array->capa)) {
    uint32_t new_capa = 2 * change_set_tree_array->capa;
    RB_REALLOC_N(change_set_tree_array->data, ChangeSetTree, new_capa);
    change_set_tree_array->capa = new_capa;
  }

  uint32_t index = change_set_tree_array->len;
  change_set_tree_array->data[index] = (ChangeSetTree){0,};
  *tree = &change_set_tree_array->data[index];
  change_set_tree_array->len++;
  return index;
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
change_set_from_tree(DiffContext *ctx, uint32_t root_idx, VALUE rb_ary) {
  // empty node
  if(root_idx == 0) {
    return;
  }

  ChangeSetTree *tree = &ctx->change_set_trees.data[root_idx];

  if(tree->change_type == CHANGE_TYPE_MOD || tree->change_type == CHANGE_TYPE_EQL) {
    if(tree->change_type == CHANGE_TYPE_MOD || (tree->change_type ==CHANGE_TYPE_EQL && ctx->output_eq)) {
      rb_ary_push(rb_ary, rb_change_set_new_full(tree->change_type, ctx->rb_old, ctx->rb_new,
                                                  &ctx->tokens_old, tree->start_old, tree->len_old,
                                                  &ctx->tokens_new, tree->start_new, tree->len_new));
    }
  } else {
    size_t start, len;
    TokenArray *tokens;
    VALUE rb_input;
    switch(tree->change_type) {
      case CHANGE_TYPE_ADD: {
        start = tree->start_new;
        len = tree->len_new;
        tokens = &ctx->tokens_new;
        rb_input = ctx->rb_new;
        break;
      }
      case CHANGE_TYPE_DEL: {
        start = tree->start_old;
        len = tree->len_old;
        tokens = &ctx->tokens_old;
        rb_input = ctx->rb_old;
        break;
      }
      default: {
        rb_raise(rb_eRuntimeError, "unexpected change type %d", tree->change_type);
      }
    }
    size_t end = start + len;
    rb_ary_push(rb_ary, rb_change_set_new(tree->change_type, rb_input, tokens, start, end - start));
  }

  change_set_from_tree(ctx, tree->children[0], rb_ary);
  change_set_from_tree(ctx, tree->children[1], rb_ary);
}

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


static void
index_old(DiffContext *ctx, uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new) {
  st_clear(ctx->old_index_map);
  ctx->index_list.len = 0;

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
static uint32_t
token_diff(DiffContext *ctx, uint32_t start_old, uint32_t len_old, uint32_t start_new, uint32_t len_new) {

  if(len_old == 0 && len_new == 0) return 0;

  // uint32_t sub_start_old = start_old;
  // uint32_t sub_start_new = start_new;
  uint32_t best_sub_length = 0;

  BetterMatchArray better_matches;
  better_match_array_init(&better_matches);

  // st_clear(next_match_map);
  // st_clear(match_map);
  match_map_reset(&ctx->next_match_map);
  match_map_reset(&ctx->match_map);

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

      // assert(next_match_map->num_entries == 0);

      IndexKey key = {
        .start_byte = token->start_byte,
        .end_byte = token->end_byte,
        .input = ctx->input_new
      };

      st_data_t pair;
      // uint32_t max_new_sub_len = 0;

      // NOTE: at this point, next_match_map but be fresh, i.e., in a post-reset() state

      if(st_lookup(ctx->old_index_map, (st_data_t)&key, &pair)) {
        while(true) {
          uint32_t value = PAIR64_FIRST(pair);
          uint32_t next_index = PAIR64_SECOND(pair);

          uint32_t iold = value;

          uint32_t prev_sub_len = 0;

          if(iold > start_old) {
            prev_sub_len = ctx->match_map.data[iold - 1].len;
            // st_lookup(match_map, (st_data_t) (iold - 1), (st_data_t *) &prev_sub_len);
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


            if(new_sub_len >= best_sub_length) { // || (new_sub_len == sub_length && !cur_sub_ends_at_newline && new_sub_ends_at_newline)) {

                // if(new_sub_len == sub_length && (new_sub_ends_at_newline || !cur_sub_ends_at_newline)) {
                //   fprintf(stderr, "HAVE FOUND NEWLINE TOKEN\n");
                //   fprintf(stderr, "preferring (%d, %d) over (%d, %d)\n", new_sub_start_old, new_sub_len, sub_start_old, sub_length);
                // }

                // we need to find the *best* matches
                // but we do not know until fully through the loop
                // so either we go through twice, or we store all 'better' matches
                // and eventually only consider the best
                BetterMatch better_match = {
                  .sub_len = new_sub_len,
                  .sub_start_old = new_sub_start_old,
                  .sub_start_new = new_sub_start_new,
                };
                best_sub_length = new_sub_len;
                // sub_start_old = new_sub_start_old;
                // sub_start_new = new_sub_start_new;

                better_match_array_insert(&better_matches, better_match);

                // fprintf(stderr, "%d => %d\n", new_sub_start_old, iold - sub_length + 1);
                // fprintf(stderr, "%d => %d\n", new_sub_start_new, inew - sub_length + 1);
                // sub_start_old = iold - sub_length + 1;
                // sub_start_new = inew - sub_length + 1;
            }
            // assert(best_sub_length <= len_old);
            // assert(best_sub_length <= len_new);
            // assert(sub_start_old >= start_old);
            // assert(sub_start_new >= start_new);
            // st_insert(next_match_map, (st_data_t) iold, new_sub_len);
            match_map_insert(&ctx->next_match_map, iold, new_sub_len);
          }

          if(next_index == UINT32_MAX) {
            break;
          } else {
            pair = ctx->index_list.entries[next_index];
          }
        }
      }

      {
        MatchMap tmp = ctx->match_map;
        ctx->match_map = ctx->next_match_map;
        ctx->next_match_map = tmp;

        match_map_reset(&ctx->next_match_map);
        // st_clear(next_match_map);
      }
    }
  }

  uint32_t ret_tree_idx = UINT32_MAX;
  ChangeSetTree *ret_tree = NULL;

  if(best_sub_length == 0) {
    ChangeSetTree *trees[3] = {0,};
    uint32_t tree_idxs[3] = {0,};
    uint64_t cost = 0;

    size_t common_len = MIN(len_old, len_new);
    if(common_len > 0) {
      tree_idxs[0] = change_set_tree_array_push(&ctx->change_set_trees, &trees[0]);
      trees[0]->change_type = CHANGE_TYPE_MOD;
      trees[0]->cost = 2 * common_len;
      trees[0]->start_old = start_old;
      trees[0]->len_old = common_len;
      trees[0]->start_new = start_new;
      trees[0]->len_new = common_len;
      cost += trees[0]->cost;

      // output_change_set(ctx, CHANGE_TYPE_MOD,
      //                   start_old, common_len,
      //                   start_new, common_len);

      // rb_ary_push(rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_MOD, rb_input_old, rb_input_new,
      //                                                tokens_old, start_old, common_len,
      //                                                tokens_new, start_new, common_len));
      start_old += common_len;
      start_new += common_len;
      len_old -= common_len;
      len_new -= common_len;
    }


    if(len_old > 0) {
      tree_idxs[1] = change_set_tree_array_push(&ctx->change_set_trees, &trees[1]);
      trees[1]->change_type = CHANGE_TYPE_DEL;
      trees[1]->cost = len_old;
      trees[1]->start_old = start_old;
      trees[1]->len_old = len_old;
      trees[1]->start_new = 0;
      trees[1]->len_new = 0;
      cost += trees[1]->cost;


      // rb_ary_push(rb_out_ary, rb_change_set_new(CHANGE_TYPE_DEL, rb_input_old, tokens_old, start_old, len_old));
      // output_change_set(ctx, CHANGE_TYPE_DEL,
      //                   start_old, len_old,
      //                   0, 0);
    }

    if(len_new > 0) {
      tree_idxs[2] = change_set_tree_array_push(&ctx->change_set_trees, &trees[2]);
      trees[2]->change_type = CHANGE_TYPE_ADD;
      trees[2]->cost = len_new;
      trees[2]->start_old = 0;
      trees[2]->len_old = 0;
      trees[2]->start_new = start_new;
      trees[2]->len_new = len_new;
      cost += trees[2]->cost;

      // rb_ary_push(rb_out_ary, rb_change_set_new(CHANGE_TYPE_ADD, rb_input_new, tokens_new, start_new, len_new));
      // output_change_set(ctx, CHANGE_TYPE_ADD,
      //                   0, 0,
      //                   start_new, len_new);
    }

    if(trees[0] != NULL) {
      trees[0]->children[0] = tree_idxs[1];
      trees[0]->children[1] = tree_idxs[2];
      ret_tree_idx = tree_idxs[0];
      ret_tree = trees[0];
    } else if(trees[1] != NULL) {
      trees[1]->children[0] = tree_idxs[2];
      ret_tree_idx = tree_idxs[1];
      ret_tree = trees[1];
    } else {
      ret_tree_idx = tree_idxs[2];
      ret_tree = trees[2];
    }
    fprintf(stderr, "BASE COST: %lld\n", cost);
    ret_tree->cost = cost;
  } else {

    assert(best_sub_length <= len_old);
    assert(best_sub_length <= len_new);

    uint64_t best_cost = UINT64_MAX;
    fprintf(stderr, "-----------------\n");
    for(uint32_t i = 0; i < better_matches.len; i++) {
      BetterMatch *better_match = &better_matches.data[i];
      uint32_t sub_length = better_match->sub_len;
      bool is_best = sub_length == best_sub_length;
      fprintf(stderr, "%d MATCH WITH %d (%d) (%d)\n", i, better_match->sub_len, is_best, best_sub_length);
      if(is_best) {

        uint32_t sub_start_old = better_match->sub_start_old;
        uint32_t sub_start_new = better_match->sub_start_new;

        assert(sub_start_old >= start_old);
        assert(sub_start_new >= start_new);


        uint32_t left_idx = token_diff(ctx, start_old, sub_start_old - start_old,
                                     start_new, sub_start_new - start_new);


        if(ctx->output_eq) {


          // output_change_set(ctx, CHANGE_TYPE_EQL,
          //                   sub_start_old, sub_length,
          //                   sub_start_new, sub_length);

          // rb_ary_push(rb_out_ary, rb_change_set_new_full(CHANGE_TYPE_EQL, rb_input_old, rb_input_new,
          //                                                tokens_old, sub_start_old, sub_length,
          //                                                tokens_new, sub_start_new, sub_length));
        }

        uint32_t right_idx = token_diff(ctx, 
                  sub_start_old + sub_length, (start_old + len_old) - (sub_start_old + sub_length),
                  sub_start_new + sub_length, (start_new + len_new) - (sub_start_new + sub_length));

        uint64_t cost = ctx->change_set_trees.data[left_idx].cost + ctx->change_set_trees.data[right_idx].cost;

        if(cost < best_cost) {
          ret_tree_idx = change_set_tree_array_push(&ctx->change_set_trees, &ret_tree);
          ret_tree->change_type = CHANGE_TYPE_EQL;
          ret_tree->cost = cost;
          ret_tree->start_old = sub_start_old;
          ret_tree->len_old = best_sub_length;
          ret_tree->start_new = sub_start_new;
          ret_tree->len_new = best_sub_length;
          ret_tree->children[0] = left_idx;
          ret_tree->children[1] = right_idx;

          best_cost = cost;
          fprintf(stderr, "BEST COST: %lld\n", best_cost);
        }
      }
    }
  }

  better_match_array_destroy(&better_matches);
  assert(ret_tree != NULL);
  assert(ret_tree_idx != 0);
  fprintf(stderr, "RET COST: %lld\n", ret_tree->cost);
  return ret_tree_idx;
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


  ctx.tokens_old = rb_node_tokenize_(rb_old, ignore_whitespace, ignore_comments);
  ctx.tokens_new = rb_node_tokenize_(rb_new, ignore_whitespace, ignore_comments);

  RB_GC_GUARD(rb_old);
  RB_GC_GUARD(rb_new);

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


  match_map_init(&ctx.match_map, ctx.tokens_old.len);
  match_map_init(&ctx.next_match_map, ctx.tokens_old.len);
  change_set_tree_array_init(&ctx.change_set_trees, 256);

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

  uint32_t tree_idx = token_diff(&ctx,
                                 prefix_len, ctx.tokens_old.len - suffix_len - prefix_len,
                                 prefix_len, ctx.tokens_new.len - suffix_len - prefix_len);



  // token_diff(tokens_old, tokens_new, &index_list, table_entries_old, match_map, _overlap, old_index_map,
  //           rb_old, rb_new, 0, tokens_old_len, 0, tokens_new_len, rb_out_ary, output_eq);

  change_set_from_tree(&ctx, tree_idx, rb_out_ary);


done:
  // st_free_table(match_map);
  // st_free_table(next_match_map);
  match_map_destroy(&ctx.match_map);
  match_map_destroy(&ctx.next_match_map);
  change_set_tree_array_destroy(&ctx.change_set_trees);
  st_free_table(ctx.old_index_map);
  xfree(ctx.table_entries_old);
  xfree(ctx.index_list.entries);
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