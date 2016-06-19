/**
 * See Copyright Notice in picrin.h
 */

#include "picrin.h"
#include "picrin/private/object.h"
#include "picrin/private/state.h"

union header {
  struct {
    union header *ptr;
    size_t size;
  } s;
};

struct object {
  union {
    struct basic basic;
    struct identifier id;
    struct string str;
    struct blob blob;
    struct pair pair;
    struct vector vec;
    struct dict dict;
    struct weak weak;
    struct data data;
    struct record rec;
    struct env env;
    struct proc proc;
    struct context cxt;
    struct port port;
    struct error err;
    struct checkpoint cp;
  } u;
};

struct heap {
#if !PIC_USE_BITMAPGC
  union header base, *freep;
#endif
  struct heap_page *pages;
  struct weak *weaks;           /* weak map chain */
};

#if PIC_USE_LIBC
void *
pic_default_allocf(void *PIC_UNUSED(userdata), void *ptr, size_t size)
{
  if (size != 0) {
    return realloc(ptr, size);
  }
  free(ptr);
  return NULL;
}
#endif

void *
pic_malloc(pic_state *pic, size_t size)
{
  void *ptr;

  ptr = pic->allocf(pic->userdata, NULL, size);
  if (ptr == NULL && size > 0) {
    pic_panic(pic, "memory exhausted");
  }
  return ptr;
}

void *
pic_realloc(pic_state *pic, void *ptr, size_t size)
{
  ptr = pic->allocf(pic->userdata, ptr, size);
  if (ptr == NULL && size > 0) {
    pic_panic(pic, "memory exhausted");
  }
  return ptr;
}

void *
pic_calloc(pic_state *pic, size_t count, size_t size)
{
  void *ptr;

  size *= count;
  ptr = pic->allocf(pic->userdata, NULL, size);
  if (ptr == NULL && size > 0) {
    pic_panic(pic, "memory exhausted");
  }
  memset(ptr, 0, size);
  return ptr;
}

void
pic_free(pic_state *pic, void *ptr)
{
  pic->allocf(pic->userdata, ptr, 0);
}

static void
gc_protect(pic_state *pic, struct object *obj)
{
  if (pic->arena_idx >= pic->arena_size) {
    pic->arena_size = pic->arena_size * 2 + 1;
    pic->arena = pic_realloc(pic, pic->arena, sizeof(struct object *) * pic->arena_size);
  }
  pic->arena[pic->arena_idx++] = obj;
}

pic_value
pic_protect(pic_state *pic, pic_value v)
{
  if (! pic_obj_p(pic, v))
    return v;

  gc_protect(pic, pic_obj_ptr(v));

  return v;
}

size_t
pic_enter(pic_state *pic)
{
  return pic->arena_idx;
}

void
pic_leave(pic_state *pic, size_t state)
{
  pic->arena_idx = state;
}

void *
pic_alloca(pic_state *pic, size_t n)
{
  static const pic_data_type t = { "pic_alloca", pic_free, 0 };

  return pic_data(pic, pic_data_value(pic, pic_malloc(pic, n), &t)); /* TODO optimize */
}

static bool is_marked(pic_state *pic, struct object *obj);
static void mark(pic_state *pic, struct object *obj);

/* MARK */

static void gc_mark_object(pic_state *, struct object *);

static void
gc_mark(pic_state *pic, pic_value v)
{
  if (! pic_obj_p(pic, v))
    return;

  gc_mark_object(pic, pic_obj_ptr(v));
}

static void
gc_mark_object(pic_state *pic, struct object *obj)
{
 loop:

  if (is_marked(pic, obj))
    return;

  mark(pic, obj);

#define LOOP(o) obj = (struct object *)(o); goto loop

  switch (obj->u.basic.tt) {
  case PIC_TYPE_PAIR: {
    gc_mark(pic, obj->u.pair.car);
    if (pic_obj_p(pic, obj->u.pair.cdr)) {
      LOOP(pic_obj_ptr(obj->u.pair.cdr));
    }
    break;
  }
  case PIC_TYPE_CXT: {
    int i;

    for (i = 0; i < obj->u.cxt.regc; ++i) {
      gc_mark(pic, obj->u.cxt.regs[i]);
    }
    if (obj->u.cxt.up) {
      LOOP(obj->u.cxt.up);
    }
    break;
  }
  case PIC_TYPE_FUNC: {
    int i;
    for (i = 0; i < obj->u.proc.u.f.localc; ++i) {
      gc_mark(pic, obj->u.proc.locals[i]);
    }
    break;
  }
  case PIC_TYPE_IREP: {
    if (obj->u.proc.u.i.cxt) {
      LOOP(obj->u.proc.u.i.cxt);
    }
    break;
  }
  case PIC_TYPE_PORT: {
    break;
  }
  case PIC_TYPE_ERROR: {
    gc_mark_object(pic, (struct object *)obj->u.err.type);
    gc_mark_object(pic, (struct object *)obj->u.err.msg);
    gc_mark(pic, obj->u.err.irrs);
    LOOP(obj->u.err.stack);
    break;
  }
  case PIC_TYPE_STRING: {
    break;
  }
  case PIC_TYPE_VECTOR: {
    int i;
    for (i = 0; i < obj->u.vec.len; ++i) {
      gc_mark(pic, obj->u.vec.data[i]);
    }
    break;
  }
  case PIC_TYPE_BLOB: {
    break;
  }
  case PIC_TYPE_ID: {
    gc_mark_object(pic, (struct object *)obj->u.id.u.id);
    LOOP(obj->u.id.env);
    break;
  }
  case PIC_TYPE_ENV: {
    khash_t(env) *h = &obj->u.env.map;
    int it;

    for (it = kh_begin(h); it != kh_end(h); ++it) {
      if (kh_exist(h, it)) {
        gc_mark_object(pic, (struct object *)kh_key(h, it));
        gc_mark_object(pic, (struct object *)kh_val(h, it));
      }
    }
    if (obj->u.env.up) {
      LOOP(obj->u.env.up);
    }
    break;
  }
  case PIC_TYPE_DATA: {
    if (obj->u.data.type->mark) {
      obj->u.data.type->mark(pic, obj->u.data.data, gc_mark);
    }
    break;
  }
  case PIC_TYPE_DICT: {
    pic_value key, val;
    int it = 0;

    while (pic_dict_next(pic, pic_obj_value(&obj->u.dict), &it, &key, &val)) {
      gc_mark(pic, key);
      gc_mark(pic, val);
    }
    break;
  }
  case PIC_TYPE_RECORD: {
    gc_mark(pic, obj->u.rec.type);
    if (pic_obj_p(pic, obj->u.rec.datum)) {
      LOOP(pic_obj_ptr(obj->u.rec.datum));
    }
    break;
  }
  case PIC_TYPE_SYMBOL: {
    LOOP(obj->u.id.u.str);
    break;
  }
  case PIC_TYPE_WEAK: {
    struct weak *weak = (struct weak *)obj;

    weak->prev = pic->heap->weaks;
    pic->heap->weaks = weak;
    break;
  }
  case PIC_TYPE_CP: {
    if (obj->u.cp.prev) {
      gc_mark_object(pic, (struct object *)obj->u.cp.prev);
    }
    if (obj->u.cp.in) {
      gc_mark_object(pic, (struct object *)obj->u.cp.in);
    }
    if (obj->u.cp.out) {
      LOOP((struct object *)obj->u.cp.out);
    }
    break;
  }
  default:
    PIC_UNREACHABLE();
  }
}

static void
gc_mark_phase(pic_state *pic)
{
  pic_value *stack;
  struct callinfo *ci;
  struct list_head *list;
  int it;
  size_t j;

  assert(pic->heap->weaks == NULL);

  /* checkpoint */
  if (pic->cp) {
    gc_mark_object(pic, (struct object *)pic->cp);
  }

  /* stack */
  for (stack = pic->stbase; stack != pic->sp; ++stack) {
    gc_mark(pic, *stack);
  }

  /* callinfo */
  for (ci = pic->ci; ci != pic->cibase; --ci) {
    if (ci->cxt) {
      gc_mark_object(pic, (struct object *)ci->cxt);
    }
  }

  /* arena */
  for (j = 0; j < pic->arena_idx; ++j) {
    gc_mark_object(pic, (struct object *)pic->arena[j]);
  }

  /* ireps */
  for (list = pic->ireps.next; list != &pic->ireps; list = list->next) {
    struct irep *irep = (struct irep *)list;
    for (j = 0; j < irep->npool; ++j) {
      gc_mark_object(pic, irep->pool[j]);
    }
  }

  /* global variables */
  gc_mark(pic, pic->globals);

  /* macro objects */
  gc_mark(pic, pic->macros);

  /* error object */
  gc_mark(pic, pic->err);

  /* features */
  gc_mark(pic, pic->features);

  /* library table */
  for (it = kh_begin(&pic->ltable); it != kh_end(&pic->ltable); ++it) {
    if (! kh_exist(&pic->ltable, it)) {
      continue;
    }
    gc_mark_object(pic, (struct object *)kh_val(&pic->ltable, it).name);
    gc_mark_object(pic, (struct object *)kh_val(&pic->ltable, it).env);
    gc_mark_object(pic, (struct object *)kh_val(&pic->ltable, it).exports);
  }

  /* weak maps */
  do {
    struct object *key;
    pic_value val;
    int it;
    khash_t(weak) *h;
    struct weak *weak;

    j = 0;
    weak = pic->heap->weaks;

    while (weak != NULL) {
      h = &weak->hash;
      for (it = kh_begin(h); it != kh_end(h); ++it) {
        if (! kh_exist(h, it))
          continue;
        key = kh_key(h, it);
        val = kh_val(h, it);
        if (is_marked(pic, key)) {
          if (pic_obj_p(pic, val) && ! is_marked(pic, pic_obj_ptr(val))) {
            gc_mark(pic, val);
            ++j;
          }
        }
      }
      weak = weak->prev;
    }
  } while (j > 0);
}

/* SWEEP */

static void
gc_finalize_object(pic_state *pic, struct object *obj)
{
  switch (obj->u.basic.tt) {
  case PIC_TYPE_VECTOR: {
    pic_free(pic, obj->u.vec.data);
    break;
  }
  case PIC_TYPE_BLOB: {
    pic_free(pic, obj->u.blob.data);
    break;
  }
  case PIC_TYPE_STRING: {
    pic_rope_decref(pic, obj->u.str.rope);
    break;
  }
  case PIC_TYPE_ENV: {
    kh_destroy(env, &obj->u.env.map);
    break;
  }
  case PIC_TYPE_DATA: {
    if (obj->u.data.type->dtor) {
      obj->u.data.type->dtor(pic, obj->u.data.data);
    }
    break;
  }
  case PIC_TYPE_DICT: {
    kh_destroy(dict, &obj->u.dict.hash);
    break;
  }
  case PIC_TYPE_SYMBOL: {
    /* TODO: remove this symbol's entry from pic->syms immediately */
    break;
  }
  case PIC_TYPE_WEAK: {
    kh_destroy(weak, &obj->u.weak.hash);
    break;
  }
  case PIC_TYPE_IREP: {
    pic_irep_decref(pic, obj->u.proc.u.i.irep);
    break;
  }

  case PIC_TYPE_PAIR:
  case PIC_TYPE_CXT:
  case PIC_TYPE_PORT:
  case PIC_TYPE_ERROR:
  case PIC_TYPE_ID:
  case PIC_TYPE_RECORD:
  case PIC_TYPE_CP:
  case PIC_TYPE_FUNC:
    break;

  default:
    PIC_UNREACHABLE();
  }
}

#if PIC_USE_BITMAPGC
# include "./gc/bitmap.c"
#else
# include "./gc/markandsweep.c"
#endif

struct heap *
pic_heap_open(pic_state *pic)
{
  struct heap *heap;

  heap = pic_malloc(pic, sizeof(struct heap));

#if !PIC_USE_BITMAPGC
  heap->base.s.ptr = &heap->base;
  heap->base.s.size = 0; /* not 1, since it must never be used for allocation */
  heap->freep = &heap->base;
#endif

  heap->pages = NULL;
  heap->weaks = NULL;

  return heap;
}

void
pic_heap_close(pic_state *pic, struct heap *heap)
{
  struct heap_page *page;

  while (heap->pages) {
    page = heap->pages;
    heap->pages = heap->pages->next;
    pic_free(pic, page);
  }
  pic_free(pic, heap);
}

static void
gc_sweep_phase(pic_state *pic)
{
  struct heap_page *page;
  int it;
  khash_t(weak) *h;
  khash_t(oblist) *s = &pic->oblist;
  symbol *sym;
  struct object *obj;
  size_t total = 0, inuse = 0;

  /* weak maps */
  while (pic->heap->weaks != NULL) {
    h = &pic->heap->weaks->hash;
    for (it = kh_begin(h); it != kh_end(h); ++it) {
      if (! kh_exist(h, it))
        continue;
      obj = kh_key(h, it);
      if (! is_marked(pic, obj)) {
        kh_del(weak, h, it);
      }
    }
    pic->heap->weaks = pic->heap->weaks->prev;
  }

  /* symbol table */
  for (it = kh_begin(s); it != kh_end(s); ++it) {
    if (! kh_exist(s, it))
      continue;
    sym = kh_val(s, it);
    if (sym && ! is_marked(pic, (struct object *)sym)) {
      kh_del(oblist, s, it);
    }
  }

  page = pic->heap->pages;
  while (page) {
    inuse += gc_sweep_page(pic, page);
    total += PAGE_UNITS;
    page = page->next;
  }

  if (PIC_PAGE_REQUEST_THRESHOLD(total) <= inuse) {
    heap_morecore(pic);
  }
}

void
pic_gc(pic_state *pic)
{
  if (! pic->gc_enable) {
    return;
  }

  gc_init(pic);

  gc_mark_phase(pic);
  gc_sweep_phase(pic);
}

struct object *
pic_obj_alloc_unsafe(pic_state *pic, size_t size, int type)
{
  struct object *obj;

#if GC_STRESS
  pic_gc(pic);
#endif

  obj = (struct object *)heap_alloc(pic, size);
  if (obj == NULL) {
    pic_gc(pic);
    obj = (struct object *)heap_alloc(pic, size);
    if (obj == NULL) {
      heap_morecore(pic);
      obj = (struct object *)heap_alloc(pic, size);
      if (obj == NULL)
	pic_panic(pic, "GC memory exhausted");
    }
  }
#if !PIC_USE_BITMAPGC
  obj->u.basic.gc_mark = WHITE;
#endif
  obj->u.basic.tt = type;

  return obj;
}

struct object *
pic_obj_alloc(pic_state *pic, size_t size, int type)
{
  struct object *obj;

  obj = pic_obj_alloc_unsafe(pic, size, type);

  gc_protect(pic, obj);
  return obj;
}
