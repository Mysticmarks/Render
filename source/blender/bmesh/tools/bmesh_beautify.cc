/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * Beautify the mesh by rotating edges between triangles
 * to more attractive positions until no more rotations can be made.
 *
 * In principle this is very simple however there is the possibility of
 * going into an eternal loop where edges keep rotating.
 * To avoid this - each edge stores a set of it previous
 * states so as not to rotate back.
 *
 * TODO
 * - Take face normals into account.
 */

#include "BLI_heap.h"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_polyfill_2d_beautify.h"

#include "MEM_guardedalloc.h"

#include "bmesh.hh"
#include "bmesh_beautify.hh" /* own include */

// #define DEBUG_TIME

#ifdef DEBUG_TIME
#  include "BLI_time.h"
#  include "BLI_time_utildefines.h"
#endif

/* -------------------------------------------------------------------- */
/* GSet for edge rotation */

struct EdRotState {
  /**
   * Edge vert indices (ordered small -> large).
   */
  int v_pair[2];
  /**
   * Face vert indices (small -> large).
   *
   * Each face-vertex points to a connected triangles vertex
   * that's isn't part of the edge defined by `v_pair`.
   */
  int f_pair[2];
};

#if 0
/* use BLI_ghashutil_inthash_v4 direct */
static uint erot_gsetutil_hash(const void *ptr)
{
  const EdRotState *e_state = (const EdRotState *)ptr;
  return BLI_ghashutil_inthash_v4(&e_state->v_pair[0]);
}
#endif
#if 0
static int erot_gsetutil_cmp(const void *a, const void *b)
{
  const EdRotState *e_state_a = (const EdRotState *)a;
  const EdRotState *e_state_b = (const EdRotState *)b;
  if (e_state_a->v_pair[0] < e_state_b->v_pair[0]) {
    return -1;
  }
  if (e_state_a->v_pair[0] > e_state_b->v_pair[0]) {
    return 1;
  }
  if (e_state_a->v_pair[1] < e_state_b->v_pair[1]) {
    return -1;
  }
  if (e_state_a->v_pair[1] > e_state_b->v_pair[1]) {
    return 1;
  }
  if (e_state_a->f_pair[0] < e_state_b->f_pair[0]) {
    return -1;
  }
  if (e_state_a->f_pair[0] > e_state_b->f_pair[0]) {
    return 1;
  }
  if (e_state_a->f_pair[1] < e_state_b->f_pair[1]) {
    return -1;
  }
  if (e_state_a->f_pair[1] > e_state_b->f_pair[1]) {
    return 1;
  }
  return 0;
}
#endif
static GSet *erot_gset_new()
{
  return BLI_gset_new(BLI_ghashutil_inthash_v4_p, BLI_ghashutil_inthash_v4_cmp, __func__);
}

/* ensure v0 is smaller */
#define EDGE_ORD(v0, v1) \
  if (v0 > v1) { \
    std::swap(v0, v1); \
  } \
  (void)0

static void erot_state_ex(const BMEdge *e, int v_index[2], int f_index[2])
{
  BLI_assert(BM_edge_is_manifold(e));
  BLI_assert(BM_vert_in_edge(e, e->l->prev->v) == false);
  BLI_assert(BM_vert_in_edge(e, e->l->radial_next->prev->v) == false);

  /* verts of the edge */
  v_index[0] = BM_elem_index_get(e->v1);
  v_index[1] = BM_elem_index_get(e->v2);
  EDGE_ORD(v_index[0], v_index[1]);

  /* verts of each of the 2 faces attached to this edge
   * (that are not a part of this edge) */
  f_index[0] = BM_elem_index_get(e->l->prev->v);
  f_index[1] = BM_elem_index_get(e->l->radial_next->prev->v);
  EDGE_ORD(f_index[0], f_index[1]);
}

static void erot_state_current(const BMEdge *e, EdRotState *e_state)
{
  erot_state_ex(e, e_state->v_pair, e_state->f_pair);
}

static void erot_state_alternate(const BMEdge *e, EdRotState *e_state)
{
  erot_state_ex(e, e_state->f_pair, e_state->v_pair);
}

/* -------------------------------------------------------------------- */
/* Calculate the improvement of rotating the edge */

static float bm_edge_calc_rotate_beauty__angle(const float v1[3],
                                               const float v2[3],
                                               const float v3[3],
                                               const float v4[3])
{
  /* not a loop (only to be able to break out) */
  do {
    float no_a[3], no_b[3];
    float angle_24, angle_13;

    /* edge (2-4), current state */
    normal_tri_v3(no_a, v2, v3, v4);
    normal_tri_v3(no_b, v2, v4, v1);
    angle_24 = angle_normalized_v3v3(no_a, no_b);

    /* edge (1-3), new state */
    /* only check new state for degenerate outcome */
    if ((normal_tri_v3(no_a, v1, v2, v3) == 0.0f) || (normal_tri_v3(no_b, v1, v3, v4) == 0.0f)) {
      break;
    }
    angle_13 = angle_normalized_v3v3(no_a, no_b);

    return angle_13 - angle_24;
  } while (false);

  return FLT_MAX;
}

float BM_verts_calc_rotate_beauty(const BMVert *v1,
                                  const BMVert *v2,
                                  const BMVert *v3,
                                  const BMVert *v4,
                                  const short flag,
                                  const short method)
{
  /* not a loop (only to be able to break out) */
  do {
    if (flag & VERT_RESTRICT_TAG) {
      const BMVert *v_a = v1, *v_b = v3;
      if (BM_elem_flag_test(v_a, BM_ELEM_TAG) == BM_elem_flag_test(v_b, BM_ELEM_TAG)) {
        break;
      }
    }

    if (UNLIKELY(v1 == v3)) {
      // printf("This should never happen, but does sometimes!\n");
      break;
    }

    switch (method) {
      case 0:
        return BLI_polyfill_edge_calc_rotate_beauty__area(
            v1->co, v2->co, v3->co, v4->co, flag & EDGE_RESTRICT_DEGENERATE);
      default:
        return bm_edge_calc_rotate_beauty__angle(v1->co, v2->co, v3->co, v4->co);
    }
  } while (false);

  return FLT_MAX;
}

static float bm_edge_calc_rotate_beauty(const BMEdge *e, const short flag, const short method)
{
  const BMVert *v1, *v2, *v3, *v4;
  v1 = e->l->prev->v;              /* First vert co */
  v2 = e->l->v;                    /* `e->v1` or `e->v2`. */
  v3 = e->l->radial_next->prev->v; /* Second vert co */
  v4 = e->l->next->v;              /* `e->v1` or `e->v2`. */

  return BM_verts_calc_rotate_beauty(v1, v2, v3, v4, flag, method);
}

/* -------------------------------------------------------------------- */
/* Update the edge cost of rotation in the heap */

BLI_INLINE bool edge_in_array(const BMEdge *e, const BMEdge **edge_array, const int edge_array_len)
{
  const int index = BM_elem_index_get(e);
  return ((index >= 0) && (index < edge_array_len) && (e == edge_array[index]));
}

/* recalc an edge in the heap (surrounding geometry has changed) */
static void bm_edge_update_beauty_cost_single(BMEdge *e,
                                              Heap *eheap,
                                              HeapNode **eheap_table,
                                              GSet **edge_state_arr,
                                              /* only for testing the edge is in the array */
                                              const BMEdge **edge_array,
                                              const int edge_array_len,

                                              const short flag,
                                              const short method)
{
  if (edge_in_array(e, edge_array, edge_array_len)) {
    const int i = BM_elem_index_get(e);
    GSet *e_state_set = edge_state_arr[i];

    if (eheap_table[i]) {
      BLI_heap_remove(eheap, eheap_table[i]);
      eheap_table[i] = nullptr;
    }

    /* check if we can add it back */
    BLI_assert(BM_edge_is_manifold(e) == true);

    /* check we're not moving back into a state we have been in before */
    if (e_state_set != nullptr) {
      EdRotState e_state_alt;
      erot_state_alternate(e, &e_state_alt);
      if (BLI_gset_haskey(e_state_set, (void *)&e_state_alt)) {
        // printf("  skipping, we already have this state\n");
        return;
      }
    }

    {
      /* recalculate edge */
      const float cost = bm_edge_calc_rotate_beauty(e, flag, method);
      if (cost < 0.0f) {
        eheap_table[i] = BLI_heap_insert(eheap, cost, e);
      }
      else {
        eheap_table[i] = nullptr;
      }
    }
  }
}

/* we have rotated an edge, tag other edges and clear this one */
static void bm_edge_update_beauty_cost(BMEdge *e,
                                       Heap *eheap,
                                       HeapNode **eheap_table,
                                       GSet **edge_state_arr,
                                       const BMEdge **edge_array,
                                       const int edge_array_len,
                                       /* only for testing the edge is in the array */
                                       const short flag,
                                       const short method)
{
  int i;

  BMEdge *e_arr[4] = {
      e->l->next->e,
      e->l->prev->e,
      e->l->radial_next->next->e,
      e->l->radial_next->prev->e,
  };

  BLI_assert(e->l->f->len == 3 && e->l->radial_next->f->len == 3);

  BLI_assert(BM_edge_face_count_is_equal(e, 2));

  for (i = 0; i < 4; i++) {
    bm_edge_update_beauty_cost_single(
        e_arr[i], eheap, eheap_table, edge_state_arr, edge_array, edge_array_len, flag, method);
  }
}

/* -------------------------------------------------------------------- */
/* Beautify Fill */

void BM_mesh_beautify_fill(BMesh *bm,
                           BMEdge **edge_array,
                           const int edge_array_len,
                           const short flag,
                           const short method,
                           const short oflag_edge,
                           const short oflag_face)
{
  Heap *eheap;            /* edge heap */
  HeapNode **eheap_table; /* edge index aligned table pointing to the eheap */

  GSet **edge_state_arr = MEM_calloc_arrayN<GSet *>(edge_array_len, __func__);
  BLI_mempool *edge_state_pool = BLI_mempool_create(sizeof(EdRotState), 0, 512, BLI_MEMPOOL_NOP);
  int i;

#ifdef DEBUG_TIME
  TIMEIT_START(beautify_fill);
#endif

  eheap = BLI_heap_new_ex(uint(edge_array_len));
  eheap_table = MEM_malloc_arrayN<HeapNode *>(size_t(edge_array_len), __func__);

  /* build heap */
  for (i = 0; i < edge_array_len; i++) {
    BMEdge *e = edge_array[i];
    const float cost = bm_edge_calc_rotate_beauty(e, flag, method);
    if (cost < 0.0f) {
      eheap_table[i] = BLI_heap_insert(eheap, cost, e);
    }
    else {
      eheap_table[i] = nullptr;
    }

    BM_elem_index_set(e, i); /* set_dirty */
  }
  bm->elem_index_dirty |= BM_EDGE;

  while (BLI_heap_is_empty(eheap) == false) {
    BMEdge *e = static_cast<BMEdge *>(BLI_heap_pop_min(eheap));
    i = BM_elem_index_get(e);
    eheap_table[i] = nullptr;

    BLI_assert(BM_edge_face_count_is_equal(e, 2));

    e = BM_edge_rotate(bm, e, false, BM_EDGEROT_CHECK_EXISTS);

    BLI_assert(e == nullptr || BM_edge_face_count_is_equal(e, 2));

    if (LIKELY(e)) {
      GSet *e_state_set = edge_state_arr[i];

      /* add the new state into the set so we don't move into this state again
       * NOTE: we could add the previous state too but this isn't essential)
       *       for avoiding eternal loops */
      EdRotState *e_state = static_cast<EdRotState *>(BLI_mempool_alloc(edge_state_pool));
      erot_state_current(e, e_state);
      if (UNLIKELY(e_state_set == nullptr)) {
        edge_state_arr[i] = e_state_set = erot_gset_new(); /* store previous state */
      }
      BLI_assert(BLI_gset_haskey(e_state_set, (void *)e_state) == false);
      BLI_gset_insert(e_state_set, e_state);

      // printf("  %d -> %d, %d\n", i, BM_elem_index_get(e->v1), BM_elem_index_get(e->v2));

      /* maintain the index array */
      edge_array[i] = e;
      BM_elem_index_set(e, i);

      /* recalculate faces connected on the heap */
      bm_edge_update_beauty_cost(e,
                                 eheap,
                                 eheap_table,
                                 edge_state_arr,
                                 (const BMEdge **)edge_array,
                                 edge_array_len,
                                 flag,
                                 method);

      /* update flags */
      if (oflag_edge) {
        BMO_edge_flag_enable(bm, e, oflag_edge);
      }

      if (oflag_face) {
        BMO_face_flag_enable(bm, e->l->f, oflag_face);
        BMO_face_flag_enable(bm, e->l->radial_next->f, oflag_face);
      }
    }
  }

  BLI_heap_free(eheap, nullptr);
  MEM_freeN(eheap_table);

  for (i = 0; i < edge_array_len; i++) {
    if (edge_state_arr[i]) {
      BLI_gset_free(edge_state_arr[i], nullptr);
    }
  }

  MEM_freeN(edge_state_arr);
  BLI_mempool_destroy(edge_state_pool);

#ifdef DEBUG_TIME
  TIMEIT_END(beautify_fill);
#endif
}
