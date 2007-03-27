#ifndef _VERIFY_METADATA_H_
#define _VERIFY_METADATA_H_

#include "../common/gc_common.h"
#include "../utils/vector_block.h"
#include "../utils/sync_pool.h"

#define METADATA_SEGMENT_NUM 128
typedef volatile unsigned int SpinLock;
typedef Vector_Block* Vector_Block_Ptr;

typedef struct Heap_Verifier_Metadata{
  void* segments[METADATA_SEGMENT_NUM];
  unsigned int num_alloc_segs;
  SpinLock alloc_lock;
  
  Pool* free_set_pool;
  Pool* free_task_pool;
  
  Pool* root_set_pool;
  Pool* mark_task_pool;
  
  Pool* free_objects_pool;
  
  Pool* objects_pool_before_gc;
  Pool* objects_pool_after_gc; 
  
  Pool* resurrect_objects_pool_before_gc;
  Pool* resurrect_objects_pool_after_gc;
  
  Pool* new_objects_pool;
} Heap_Verifier_Metadata;

extern Heap_Verifier_Metadata* verifier_metadata;

struct Heap_Verifier;
void gc_verifier_metadata_initialize(Heap_Verifier* heap_verifier);
void gc_verifier_metadata_destruct(Heap_Verifier* heap_verifier);
Vector_Block* gc_verifier_metadata_extend(Pool* pool, Boolean is_set_pool);

void verifier_clear_pool(Pool* working_pool, Pool* free_pool, Boolean is_vector_stack);

inline Vector_Block* verifier_free_set_pool_get_entry(Pool* free_pool)
{
  assert(free_pool);
  Vector_Block* block = pool_get_entry(free_pool);
  
  while(!block)
    block = gc_verifier_metadata_extend(free_pool, TRUE);
  
  assert(vector_block_is_empty(block));
  return block; 
}

inline Vector_Block* verifier_free_task_pool_get_entry(Pool* free_pool)
{
  assert(free_pool);
  Vector_Block* block = pool_get_entry(free_pool);
  
  while(!block)
    block = gc_verifier_metadata_extend(free_pool, FALSE);
  
  assert(vector_stack_is_empty(block));
  return block; 
}



inline void verifier_tracestack_push(void* p_task, Vector_Block_Ptr& trace_task)
{
  vector_stack_push(trace_task, (POINTER_SIZE_INT)p_task);
  
  if( !vector_stack_is_full(trace_task)) return;
    
  pool_put_entry(verifier_metadata->mark_task_pool, trace_task);
  trace_task = verifier_free_task_pool_get_entry(verifier_metadata->free_task_pool);  
  assert(trace_task);
}

inline void verifier_rootset_push(void* p_task, Vector_Block_Ptr& root_set)
{
  vector_block_add_entry(root_set, (POINTER_SIZE_INT)p_task);
  
  if( !vector_block_is_full(root_set)) return;
    
  pool_put_entry(verifier_metadata->root_set_pool, root_set);
  root_set = verifier_free_set_pool_get_entry(verifier_metadata->free_set_pool);  
  assert(root_set);
}

inline void verifier_set_push(void* p_data, Vector_Block_Ptr& set_block, Pool* pool)
{
  vector_block_add_entry(set_block, (POINTER_SIZE_INT)p_data);
  
  if( !vector_block_is_full(set_block) ) return;
  
  pool_put_entry(pool, set_block);
  set_block = verifier_free_set_pool_get_entry(verifier_metadata->free_objects_pool);
  assert(set_block);
}

#endif //_VERIFY_METADATA_H_