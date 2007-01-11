/*
 *  Copyright 2005-2006 The Apache Software Foundation or its licensors, as applicable.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/**
 * @author Ji Qi, 2006/10/05
 */

#ifndef _LSPACE_H_
#define _LSPACE_H_

#include "../common/gc_common.h"
#include "../thread/gc_thread.h"
#include "free_area_pool.h"

#define GC_MIN_LOS_SIZE ( 4 * 1024 * 1024)


typedef struct Lspace{
  /* <-- first couple of fields are overloadded as Space */
  void* heap_start;
  void* heap_end;
  unsigned int reserved_heap_size;
  unsigned int committed_heap_size;
  unsigned int num_collections;
  int64 time_collections;
  float survive_ratio;
  GC* gc;
  Boolean move_object;
  /* END of Space --> */

//  void* alloc_free;
  Free_Area_Pool* free_pool;
  
}Lspace;

void lspace_initialize(GC* gc, void* reserved_base, unsigned int lspace_size);
void lspace_destruct(Lspace* lspace);
Managed_Object_Handle lspace_alloc(unsigned int size, Allocator* allocator);
void lspace_sweep(Lspace* lspace);
void lspace_reset_after_collection(Lspace* lspace);
void lspace_collection(Lspace* lspace);

inline unsigned int lspace_free_memory_size(Lspace* lspace){ /* FIXME:: */ return 0; }
inline unsigned int lspace_committed_size(Lspace* lspace){ return lspace->committed_heap_size; }

inline Partial_Reveal_Object* lspace_get_next_marked_object( Lspace* lspace, unsigned int* iterate_index)
{
    unsigned int next_area_start = (unsigned int)lspace->heap_start + (*iterate_index) * KB;
    BOOLEAN reach_heap_end = 0;

    while(!reach_heap_end){
        //FIXME: This while shoudl be if, try it!
        while(!*((unsigned int *)next_area_start)){
                next_area_start += ((Free_Area*)next_area_start)->size;
        }
        if(next_area_start < (unsigned int)lspace->heap_end){
            //If there is a living object at this addr, return it, and update iterate_index
            if(obj_is_marked_in_vt((Partial_Reveal_Object*)next_area_start)){
                unsigned int obj_size = ALIGN_UP_TO_KILO(vm_object_size((Partial_Reveal_Object*)next_area_start));
                *iterate_index = (next_area_start + obj_size - (unsigned int)lspace->heap_start) >> BIT_SHIFT_TO_KILO;
                return (Partial_Reveal_Object*)next_area_start;
            //If this is a dead object, go on to find  a living one.
            }else{
                unsigned int obj_size = ALIGN_UP_TO_KILO(vm_object_size((Partial_Reveal_Object*)next_area_start));
                next_area_start += obj_size;
            }
        }else{
            reach_heap_end = 1;
        } 
    }
    return NULL;

}

inline Partial_Reveal_Object* lspace_get_first_marked_object(Lspace* lspace, unsigned int* mark_bit_idx)
{
    return lspace_get_next_marked_object(lspace, mark_bit_idx);
}

void lspace_fix_after_copy_nursery(Collector* collector, Lspace* lspace);

void lspace_fix_repointed_refs(Collector* collector, Lspace* lspace);

#endif /*_LSPACE_H_ */
