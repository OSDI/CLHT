#include "clht_lb_res.h"
#include <assert.h>
#include <malloc.h>

static __thread ht_ts_t* clht_ts_thread = NULL;

/* 
 * initialize thread metadata for GC
 */
void
ht_gc_thread_init(clht_t* h, int id)
{
  ht_ts_t* ts = (ht_ts_t*) memalign(CACHE_LINE_SIZE, sizeof(ht_ts_t));
  assert(ts != NULL);

  ts->version = h->ht->version;
  ts->id = id;

  do
    {
      ts->next = h->version_list;
    }
  while (CAS_U64((volatile size_t*) &h->version_list, (size_t) ts->next, (size_t) ts) != (size_t) ts->next);

  clht_ts_thread = ts;
}

/* 
 * set the ht version currently used by the current thread
 */
inline void
ht_gc_thread_version(clht_hashtable_t* h)
{
  clht_ts_thread->version = h->version;
}

/* 
 * get the GC id of the current thread
 */
inline int 
clht_gc_get_id()
{
  return clht_ts_thread->id;
}

static int ht_gc_collect_cond(clht_t* hashtable, int collect_not_referenced_only);

/* 
 * perform a GC of the versions of the ht that are not currently used by any
 * of the participating threads
 */
inline int
ht_gc_collect(clht_t* hashtable)
{
#if CLHT_DO_GC == 1
  CLHT_GC_HT_VERSION_USED(hashtable->ht);
  return ht_gc_collect_cond(hashtable, 1);
#else
  return 0;
#endif
}

/* 
 * perform a GC of ALL old versions of the ht, regardless if they are
 * referenced by any of the threads
 */
int
ht_gc_collect_all(clht_t* hashtable)
{
  return ht_gc_collect_cond(hashtable, 0);
}

#define GET_ID(x) x ? clht_gc_get_id() : 99

/* 
 * go over the version metadata of all threads and return the min ht
 * version that is currently used. In other words, all versions, less
 * than the returned value, can be GCed
 */
size_t
ht_gc_min_version_used(clht_t* h)
{
  volatile ht_ts_t* cur = h->version_list;

  size_t min = h->ht->version;
  while (cur != NULL)
    {
      if (cur->version < min)
	{
	  min = cur->version;
	}
      cur = cur->next;
    }

  return min;
}

/* 
 * GC help function:
 * collect_not_referenced_only == 0 -> ht_gc_collect_all();
 * collect_not_referenced_only != 0 -> ht_gc_collect();
 */
static int
ht_gc_collect_cond(clht_t* hashtable, int collect_not_referenced_only)
{
  /* if version_min >= current version there is nothing to collect! */
  if ((hashtable->version_min >= hashtable->ht->version) || TRYLOCK_ACQ(&hashtable->gc_lock))
    {
      /* printf("** someone else is performing gc\n"); */
      return 0;
    }

  ticks s = getticks();

  /* printf("[GCOLLE-%02d] LOCK  : %zu\n", GET_ID(collect_not_referenced_only), hashtable->version); */

  size_t version_min = hashtable->ht->version; 
  if (collect_not_referenced_only)
    {
      version_min = ht_gc_min_version_used(hashtable);
    }

  /* printf("[GCOLLE-%02d] gc collect versions < %3zu - current: %3zu - oldest: %zu\n",  */
  /* 	 GET_ID(collect_not_referenced_only), version_min, hashtable->version, hashtable->version_min); */

  int gced_num = 0;

  if (hashtable->version_min >= version_min)
    {
      /* printf("[GCOLLE-%02d] UNLOCK: %zu (nothing to collect)\n", GET_ID(collect_not_referenced_only), hashtable->ht->version); */
      TRYLOCK_RLS(hashtable->gc_lock);
    }
  else
    {
      /* printf("[GCOLLE-%02d] collect from %zu to %zu\n", GET_ID(collect_not_referenced_only), hashtable->version_min, version_min); */

      clht_hashtable_t* cur = hashtable->ht_oldest;
      while (cur != NULL && cur->version < version_min)
	{
	  gced_num++;
	  clht_hashtable_t* nxt = cur->table_new;
	  /* printf("[GCOLLE-%02d] gc_free version: %6zu | current version: %6zu\n", GET_ID(collect_not_referenced_only), */
	  /* 	 cur->version, hashtable->ht->version); */
	  nxt->table_prev = NULL;
	  ht_gc_free(cur);
	  cur = nxt;
	}

      hashtable->version_min = cur->version;
      hashtable->ht_oldest = cur;

      TRYLOCK_RLS(hashtable->gc_lock);
      /* printf("[GCOLLE-%02d] UNLOCK: %zu\n", GET_ID(collect_not_referenced_only), cur->version); */
    }

  ticks e = getticks() - s;
  printf("[GCOLLE-%02d] collected: %-3d | took: %13llu ti = %8.6f s\n", 
	 GET_ID(collect_not_referenced_only), gced_num, (unsigned long long) e, e / 2.1e9);


  return gced_num;
}

/* 
 * free the given hashtable
 */
int
ht_gc_free(clht_hashtable_t* hashtable)
{
  /* the CLHT_LINKED version does not allocate any extra buckets! */
#if !defined(CLHT_LB_LINKED) && !defined(LOCKFREE_RES)
  uint32_t num_buckets = hashtable->num_buckets;
  volatile bucket_t* bucket = NULL;

  uint32_t bin;
  for (bin = 0; bin < num_buckets; bin++)
    {
      bucket = hashtable->table + bin;
      bucket = bucket->next;
      while (bucket != NULL)
	{
	  volatile bucket_t* cur = bucket;
	  bucket = bucket->next;
	  free((void*) cur);
	}
    }
#endif

  free(hashtable->table);
  free(hashtable);

  return 1;
}

/* 
 * free all hashtable version (inluding the latest)
 */
void
ht_gc_destroy(clht_t* hashtable)
{
#if !defined(CLHT_LINKED)
  ht_gc_collect_all(hashtable);
  ht_gc_free(hashtable->ht);
  free(hashtable);
#endif
}

/* 
 * uses the ssmem_release function to return some memory
 * to the OS (free), when it is safe (nobody is using it
 * anymore)
 */
inline int
ht_gc_release(clht_hashtable_t* hashtable)
{
  /* the CLHT_LINKED version does not allocate any extra buckets! */
#if !defined(CLHT_LINKED) && !defined(LOCKFREE_RES)
  uint32_t num_buckets = hashtable->num_buckets;
  volatile bucket_t* bucket = NULL;

  uint32_t bin;
  for (bin = 0; bin < num_buckets; bin++)
    {
      bucket = hashtable->table + bin;
      bucket = bucket->next;
      while (bucket != NULL)
	{
	  volatile bucket_t* cur = bucket;
	  bucket = bucket->next;
	  ssmem_release(clht_alloc, (void*) cur);
	}
    }
#endif

  ssmem_release(clht_alloc, hashtable->table);
  ssmem_release(clht_alloc, hashtable);

  return 1;
}
