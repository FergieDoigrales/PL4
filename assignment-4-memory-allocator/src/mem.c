#define _DEFAULT_SOURCE

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "mem_internals.h"
#include "mem.h"
#include "util.h"

void debug_block(struct block_header* b, const char* fmt, ... );
void debug(const char* fmt, ... );

extern inline block_size size_from_capacity( block_capacity cap );
extern inline block_capacity capacity_from_size( block_size sz );

static bool            block_is_big_enough( size_t query, struct block_header* block ) { return block->capacity.bytes >= query; }
static size_t          pages_count   ( size_t mem )                      { return mem / getpagesize() + ((mem % getpagesize()) > 0); }
static size_t          round_pages   ( size_t mem )                      { return getpagesize() * pages_count( mem ) ; }

static void block_init( void* restrict addr, block_size block_sz, void* restrict next ) {
  *((struct block_header*)addr) = (struct block_header) {
    .next = next,
    .capacity = capacity_from_size(block_sz),
    .is_free = true
  };
}

static size_t region_actual_size( size_t query ) { return size_max( round_pages( query ), REGION_MIN_SIZE ); }

extern inline bool region_is_invalid( const struct region* r );



static void* map_pages(void const* addr, size_t length, int additional_flags) {
  return mmap( (void*) addr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | additional_flags , -1, 0 );
}

/*  аллоцировать регион памяти и инициализировать его блоком */
static struct region alloc_region  ( void const * addr, size_t query ) {
  size_t size = region_actual_size(size_from_capacity((block_capacity){.bytes = query}).bytes);
  void *region_addr = map_pages(addr, size, MAP_FIXED_NOREPLACE);
  if (region_addr == MAP_FAILED)
  {
    region_addr = map_pages(addr, size, 0);
    if (region_addr == MAP_FAILED) {
      return REGION_INVALID;
    }
  }
  block_init(region_addr, (block_size){.bytes = size}, NULL);
  return (struct region){
      .addr = region_addr, 
      .size = size, 
      .extends = (region_addr == addr)
      };
}

static void* block_after( struct block_header const* block )         ;

void* heap_init( size_t initial ) {
  const struct region region = alloc_region( HEAP_START, initial );
  if ( region_is_invalid(&region) ) return NULL;

  return region.addr;
}

/*  освободить всю память, выделенную под кучу */
void heap_term( ) {
    struct region *region = HEAP_START;
    struct block_header *block = (struct block_header *)region;
    while (region) {
        size_t region_size = 0;
        while (block->next && blocks_continuous(block, block->next)) {
            region_size += size_from_capacity(block->capacity).bytes;
            block = block->next;
        }
        region_size += size_from_capacity(block->capacity).bytes;
        block = block->next;
        unmap_pages(region, region_size);
        region = (struct region *)block;
    }
}

#define BLOCK_MIN_CAPACITY 24

/*  --- Разделение блоков (если найденный свободный блок слишком большой )--- */

static bool block_splittable( struct block_header* restrict block, size_t query) {
  return block-> is_free && query + offsetof( struct block_header, contents ) + BLOCK_MIN_CAPACITY <= block->capacity.bytes;
}

static bool split_if_too_big( struct block_header* block, size_t query ) {
  bool is_splittable = block_splittable(block, query);

  if (is_splittable) {
      void* new_block = block->contents + query;
      const block_size new_size = (block_size){.bytes = (block->capacity.bytes - query)};

      block_init(new_block, new_size, block->next);
      block->next = new_block;
      block->capacity.bytes = query;

      return true;
}


/*  --- Слияние соседних свободных блоков --- */

static void* block_after( struct block_header const* block )              {
  return  (void*) (block->contents + block->capacity.bytes);
}
static bool blocks_continuous (
                               struct block_header const* fst,
                               struct block_header const* snd ) {
  return (void*)snd == block_after(fst);
}

static bool mergeable(struct block_header const* restrict fst, struct block_header const* restrict snd) {
  return fst->is_free && snd->is_free && blocks_continuous( fst, snd ) ;
}

static bool try_merge_with_next( struct block_header* block ) {
    if (mergeable(block, block->next)) {
        struct block_header* new_next = block->next->next;
        block_size snd_size =  size_from_capacity(block->next->capacity);
        block_capacity new_capacity = { block->capacity.bytes + snd_size.bytes};
        block_size new_size = size_from_capacity(new_capacity);
        block_init(block, new_size, new_next);
        return 1;
    }
    return 0;
}


/*  --- ... ecли размера кучи хватает --- */

struct block_search_result {
  enum {BSR_FOUND_GOOD_BLOCK, BSR_REACHED_END_NOT_FOUND, BSR_CORRUPTED} type;
  struct block_header* block;
};


static struct block_search_result find_good_or_last  ( struct block_header* restrict block, size_t sz )    {
    if (!block) return (struct block_search_result) {BSR_CORRUPTED, NULL};
    struct block_header *last_block = NULL;
    while (block){
        while (try_merge_with_next(block));
        if (block->is_free && block_is_big_enough(sz, block))
            return (struct block_search_result) {BSR_FOUND_GOOD_BLOCK, block};
        last_block = block;
        block = block->next;
    }
    return (struct block_search_result) {BSR_REACHED_END_NOT_FOUND, last_block};
}

/*  Попробовать выделить память в куче начиная с блока `block` не пытаясь расширить кучу
 Можно переиспользовать как только кучу расширили. */
static struct block_search_result try_memalloc_existing ( size_t query, struct block_header* block ) {
    size_t adjusted_query = (query < BLOCK_MIN_CAPACITY) ? BLOCK_MIN_CAPACITY : query;

    struct block_search_result result = find_good_or_last(block, adjusted_query);

    if (result.type == BSR_FOUND_GOOD_BLOCK) {
        split_if_too_big(result.block, adjusted_query);
        result.block->is_free = false;
    }

    return result;
}



static struct block_header* grow_heap( struct block_header* restrict last, size_t query ) {
    void *new_region = block_after(last);
    struct region new = alloc_region(new_region, size_max(query, BLOCK_MIN_CAPACITY));
    if (region_is_invalid(&new)) {
        return NULL;
    }
    block_init(new.addr, (block_size) {new.size}, NULL);
    if (last == NULL) {
        return NULL;
    }
    last->next = (struct block_header *) new.addr;
    if (last->is_free && try_merge_with_next(last)) return last;
    else { return last->next; }
}

/*  Реализует основную логику malloc и возвращает заголовок выделенного блока */
static struct block_header* memalloc( size_t query, struct block_header* heap_start) {
    if (heap_start == NULL) {
        return NULL;
    }

    struct block_search_result res = try_memalloc_existing(query, heap_start);

    if (res.type != BSR_CORRUPTED) {
        if (res.type == BSR_FOUND_GOOD_BLOCK) {
            return res.block;
        }
        struct block_header* growed_block = grow_heap(res.block, query);
        if (growed_block == NULL) {
            return NULL;
        }
        res = try_memalloc_existing(query, growed_block);
        if (res.type == BSR_FOUND_GOOD_BLOCK) {
            return res.block;
        }
    }
    return NULL;
}

void* _malloc( size_t query ) {
  struct block_header* const addr = memalloc( query, (struct block_header*) HEAP_START );
  if (addr) return addr->contents;
  else return NULL;
}

static struct block_header* block_get_header(void* contents) {
  return (struct block_header*) (((uint8_t*)contents)-offsetof(struct block_header, contents));
}

void _free( void* mem ) {
  if (!mem) return ;
  struct block_header* header = block_get_header( mem );
  header->is_free = true;
  try_merge_with_next(header) {};
}
