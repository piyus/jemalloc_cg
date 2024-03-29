#define JEMALLOC_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/extent_dss.h"
#include "jemalloc/internal/extent_mmap.h"
#include "jemalloc/internal/hook.h"
#include "jemalloc/internal/jemalloc_internal_types.h"
#include "jemalloc/internal/log.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/rtree.h"
#include "jemalloc/internal/safety_check.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/spin.h"
#include "jemalloc/internal/sz.h"
#include "jemalloc/internal/ticker.h"
#include "jemalloc/internal/util.h"
#include <obstack.h>

#undef obstack_free

#include "largemalloc.c"

#define ALIGN_PAD(y) ((size_t)(y)-1)
#define ALIGN_MASK(y) (~ALIGN_PAD(y))
#define JE_ALIGN(x, y) (char*)(((size_t)(x) + ALIGN_PAD(y)) & ALIGN_MASK(y))

static unsigned long long event_id = 1;
//static unsigned long long min_events = 0; //210524731ULL; //21498951ULL; //8600857659ULL; //0xffff4800000000ULL;
static unsigned long long min_events = 0xffff4800000000ULL;
static int trace_count = 0;
static /*__thread*/ bool signal_handler_invoked = false;
extern void *__text_start, *__text_end;
extern void *MinGlobalAddr;
extern void *MaxGlobalAddr;
extern void *LastCrashAddr1;
extern void *LastCrashAddr2;
static char *program_name = NULL;

#define MAX_FAULTY_PAGES 4096
#define PAGE_SHIFT 12
#define FAULTY_BITS 12
#define FAULTY_MASK ((1ULL<<FAULTY_BITS) - 1)
#define MAX_OFFSET ((1ULL<<15) - 1)

#if 0
static void print_allocations(size_t total_alloc_sz)
{
	static size_t total_alloc = 0;
	static size_t total_large_alloc = 0;
	static size_t temp_alloc = 0;
	static size_t temp_large_alloc = 0;

	if (total_alloc_sz >= MAX_OFFSET) {
		temp_large_alloc += total_alloc_sz;
	}
	else {
		temp_alloc += total_alloc_sz;
	}
	if (temp_alloc > 10000) {
		total_alloc += temp_alloc;
		malloc_printf("total_alloc:%zd\n", total_alloc + total_large_alloc);
		temp_alloc = 0;
	}
	else if (temp_large_alloc > 10000) {
		total_large_alloc += temp_large_alloc;
		malloc_printf("total_large_alloc:%zd\n", total_large_alloc);
		temp_large_alloc = 0;
	}
}
#endif

#define print_allocations(x)

#if 0

static size_t FaultyPages[MAX_FAULTY_PAGES] = {0};

static inline size_t faulty_key(size_t addr) {
	return (addr >> PAGE_SHIFT) & FAULTY_MASK;
}

static inline bool check_faulty_pages(size_t object)
{
	size_t key = faulty_key(object);
	return FaultyPages[key] == object;
}

static inline void add_faulty_pages(size_t object)
{
	size_t key = faulty_key(object);
	FaultyPages[key] = object;
}

static void remove_faulty_pages(size_t object)
{
	if (check_faulty_pages(object)) {
		size_t key = faulty_key(object);
		FaultyPages[key] = 0;
	}
}
#endif

struct LargeBase_t {
	size_t addr;
	size_t base;
};

//static /*__thread*/ struct LargeBase_t LargeBases[MAX_FAULTY_PAGES] = {0};

static inline size_t base_key(size_t addr) {
	return addr & FAULTY_MASK;
}

static int LastLine =  0;
static char LastName[64];

struct obj_header {
	unsigned magic:16;
	unsigned aligned:1;
	unsigned offset:15;
	unsigned size;
};

static FILE *trace_fp = NULL;
#define MAGIC_NUMBER 0xface

static void abort3(const char *msg)
{
	if (trace_fp) {
		fprintf(trace_fp, "event_id:%lld %s\n", event_id, msg);
		syncfs(fileno(trace_fp));
		fclose(trace_fp);
	}
	assert(0);
}

JEMALLOC_EXPORT
void je_san_abort(void *ptr)
{
	if (trace_fp) {
		fprintf(trace_fp, "unexpected pointer: %p\n", ptr);
	}
	abort3("bounds");
}

static inline bool is_valid_obj_header(struct obj_header *head) {
	return head->magic == MAGIC_NUMBER;
}

static bool is_invalid_ptr(size_t ptr) {
	return (ptr >> 48) & 1;
}

static size_t get_offset_from_ptr(size_t ptr) {
	return ptr >> 49;
}


static inline bool can_print_in_trace_fp() {
	if ((event_id < min_events /* && trace_count <= 0*/) || !trace_fp) {
		return false;
	}
	if (trace_count > 0) {
		trace_count--;
	}
	return true;
}


static size_t get_interior(size_t ptr, size_t offset) {
	if (can_print_in_trace_fp()) {
		fprintf(trace_fp, "get_interior:%p %zd\n", (void*)ptr, offset);
	}
	if (offset > MAX_OFFSET) {
		offset = MAX_OFFSET;
	}
	ptr = (ptr << 15) >> 15;
	ptr |= (offset << 49);
	return ptr;
}

static size_t get_interior_add(size_t ptr, size_t offset) {
	size_t old_offset = get_offset_from_ptr(ptr);
	offset = (old_offset == MAX_OFFSET) ? old_offset : offset + old_offset;
	return get_interior(ptr, offset);
}

static inline void* get_fast_base_safe(size_t object, bool *large_offset) {
	size_t offset = (object >> 49);
	if (offset == MAX_OFFSET) {
		*large_offset = true;
		return NULL;
	}
	struct obj_header *head = (struct obj_header*)((object & 0xFFFFFFFFFFFF) - offset - 8);

	//if (check_faulty_pages((size_t)head)) {
		//malloc_printf("FAULTY: %p\n", head);
		//return NULL;
	//}

	int magic_number = 0;
	asm volatile ("movl (%1), %0 \n\t"
								"nop; nop; nop; nop; \n\t"
								"nop; nop; nop; nop; \n\t" : 
								"=r" (magic_number) : "r"(head));

	if (signal_handler_invoked) {
		//add_faulty_pages((size_t)head);
		signal_handler_invoked = false;
		return NULL;
	}

	if (magic_number != MAGIC_NUMBER) {
		/*if ((void*)head < __text_start || (void*)head >= __text_end) {
			assert(0);
		}*/
		return NULL;
	}
	return head;
}


static inline void* get_fast_base(size_t object) {
	//malloc_printf("get_fast_base :: %zd\n", object);
	size_t offset = (object >> 49);
	if (offset == MAX_OFFSET) {
		return NULL;
	}
	struct obj_header *head = (struct obj_header*)((object & 0xFFFFFFFFFFFF) - offset - 8);
	if (!is_valid_obj_header(head)) {
		if (trace_fp) {
			fprintf(trace_fp, "head:%p ptr:%p\n", head, (void*)object);
			abort3("hi");
		}
	}
	return head;
}



#define ABORT4(x) ({ if (!(x)) { abort3(#x); }   })

#ifndef NO_SAFETY
#define OBJ_HEADER_SIZE (sizeof(struct obj_header))
#else
#define OBJ_HEADER_SIZE 0
#endif


static inline size_t get_size_from_obj_header(struct obj_header *h) {
	return h->size + OBJ_HEADER_SIZE + h->offset;
}

#ifndef NO_SAFETY

static void *_je_san_get_base(void *ptr);

static void
extent_interior_register1(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx, extent_t *extent,
    szind_t szind) {
	unsigned long long base = (unsigned long long)extent_base_get(extent);
	size_t size = extent_size_get(extent);
	//malloc_printf("base: %llx size:%zx\n", base, size);
	assert((base & 0xfff) == 0);

	/* Register interior. */
	for (size_t i = 1; i < (size >> LG_PAGE) - 1; i++) {
		rtree_write(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE), extent, szind, false);
	}
}


static void
extent_interior_deregister1(tsdn_t *tsdn, rtree_ctx_t *rtree_ctx,
    extent_t *extent) {
	size_t i;
	unsigned long long base = (unsigned long long)extent_base_get(extent);
	size_t size = extent_size_get(extent);
	//malloc_printf("de: base: %llx size:%zx\n", base, size);
	assert((base & 0xfff) == 0);

	for (i = 1; i < (size >> LG_PAGE) - 1; i++) {
		rtree_clear(tsdn, &extents_rtree, rtree_ctx,
		    (uintptr_t)extent_base_get(extent) + (uintptr_t)(i <<
		    LG_PAGE));
	}
}



static int enable_masking = 0;
static int stack_initialized = 0;
static __thread char *je_stack_begin = NULL;

#define INTERIOR_STR1 0xcaba7fULL
#define TRACK_STR 0 //0xcabaULL
#define TRACK_SHIFT 48
#define UNMASK(x) ((char*)((((uint64_t)(x)) & 0xffffffffffffULL)))
#define _MASK1(x) ((enable_masking) ? ((char*)((((uint64_t)(x)) | (1ULL << 48)))) : (char*)(x))


static bool need_tracking(unsigned long long val) {
	if (!TRACK_STR) {
		return false;
	}
	return (val >> TRACK_SHIFT) == TRACK_STR;
}

static void add_large_pointer(void *ptr, size_t size) {
	if (LastCrashAddr1 >= ptr && LastCrashAddr1 < ptr + size) {
		LastCrashAddr1 = NULL;
	}
	if (LastCrashAddr2 >= ptr && LastCrashAddr2 < ptr + size) {
		LastCrashAddr2 = NULL;
	}
	if (size > SC_SMALL_MAXCLASS && size < MAX_OFFSET) {
		tsd_t *tsd = tsd_fetch_min();
		tsdn_t *tsdn = tsd_tsdn(tsd);
		assert(tsdn);
		extent_t *e = iealloc(tsdn, ptr);
		rtree_ctx_t rtree_ctx_fallback;
  	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
		szind_t szind = extent_szind_get_maybe_invalid(e);
		extent_interior_register1(tsdn, rtree_ctx, e, szind);
	}
}

#define MAX_SAN_HASH_SIZE 4096
//static unsigned* SanHash[4096] = {NULL};
//static int NumSanHashElem = 0;

static void remove_from_san_hash(void *obj)
{
#if 0
	if (!NumSanHashElem) {
		//return;
	}
	int i;
	for (i = 0; i < MAX_SAN_HASH_SIZE; i++) {
		if (SanHash[i] == (unsigned*)obj) {
			SanHash[i] = NULL;
			if (obj >= MaxGlobalAddr) {
				NumSanHashElem--;
			}
		}
	}
#endif
}

#if 0
static void add_to_san_hash(unsigned *header, int Idx)
{
	if (!SanHash[Idx] && (void*)header >= MaxGlobalAddr) {
		NumSanHashElem++;
	}
	SanHash[Idx] = header;
}
#endif

static int san_hash_index(size_t obj)
{
	//return (obj >> 3) & (MAX_SAN_HASH_SIZE-1);
	return 0;
}

static void* lookup_san_hash(void *base, int Idx)
{
#if 0
	unsigned *hashval = SanHash[Idx];
	if (hashval && (unsigned*)base >= hashval && (char*)base < ((char*)hashval) + *(hashval-1)) {
		return hashval;
	}
#endif
	return NULL;
}

static void remove_large_pointer(void *ptr, size_t size) {
	if (size > SC_SMALL_MAXCLASS && size < MAX_OFFSET) {
		tsd_t *tsd = tsd_fetch_min();
		tsdn_t *tsdn = tsd_tsdn(tsd);
		assert(tsdn);
		extent_t *e = iealloc(tsdn, ptr);
		rtree_ctx_t rtree_ctx_fallback;
  	rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);
		extent_interior_deregister1(tsdn, rtree_ctx, e);
	}
}


#define IS_MAGIC(x) (((x) & 0xffff) == MAGIC_NUMBER)

static void *make_obj_header(void *ptr, size_t size, unsigned short offset, bool aligned)
{
	assert(ptr);
	struct obj_header *header = (struct obj_header*)ptr;
	header->magic = MAGIC_NUMBER;
	header->offset = offset;
	header->aligned = aligned;
	header->size = (unsigned)size;
	return &header[1];
}



#else
#define make_obj_header(x, y) x
#define get_obj_header(x) x
#define is_valid_obj_header(x) 1
#endif


/******************************************************************************/
/* Data. */

/* Runtime configuration options. */
const char	*je_malloc_conf
#ifndef _WIN32
    JEMALLOC_ATTR(weak)
#endif
    ;
bool	opt_abort =
#ifdef JEMALLOC_DEBUG
    true
#else
    false
#endif
    ;
bool	opt_abort_conf =
#ifdef JEMALLOC_DEBUG
    true
#else
    false
#endif
    ;
/* Intentionally default off, even with debug builds. */
bool	opt_confirm_conf = false;
const char	*opt_junk =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    "true"
#else
    "false"
#endif
    ;
bool	opt_junk_alloc =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    true
#else
    false
#endif
    ;
bool	opt_junk_free =
#if (defined(JEMALLOC_DEBUG) && defined(JEMALLOC_FILL))
    true
#else
    false
#endif
    ;

bool	opt_utrace = false;
bool	opt_xmalloc = false;
bool	opt_zero = false;
unsigned	opt_narenas = 0;

unsigned	ncpus;

/* Protects arenas initialization. */
malloc_mutex_t arenas_lock;
/*
 * Arenas that are used to service external requests.  Not all elements of the
 * arenas array are necessarily used; arenas are created lazily as needed.
 *
 * arenas[0..narenas_auto) are used for automatic multiplexing of threads and
 * arenas.  arenas[narenas_auto..narenas_total) are only used if the application
 * takes some action to create them and allocate from them.
 *
 * Points to an arena_t.
 */
JEMALLOC_ALIGNED(CACHELINE)
atomic_p_t		arenas[MALLOCX_ARENA_LIMIT];
static atomic_u_t	narenas_total; /* Use narenas_total_*(). */
/* Below three are read-only after initialization. */
static arena_t		*a0; /* arenas[0]. */
unsigned		narenas_auto;
unsigned		manual_arena_base;

typedef enum {
	malloc_init_uninitialized	= 3,
	malloc_init_a0_initialized	= 2,
	malloc_init_recursible		= 1,
	malloc_init_initialized		= 0 /* Common case --> jnz. */
} malloc_init_t;
static malloc_init_t	malloc_init_state = malloc_init_uninitialized;

/* False should be the common case.  Set to true to trigger initialization. */
bool			malloc_slow = true;

/* When malloc_slow is true, set the corresponding bits for sanity check. */
enum {
	flag_opt_junk_alloc	= (1U),
	flag_opt_junk_free	= (1U << 1),
	flag_opt_zero		= (1U << 2),
	flag_opt_utrace		= (1U << 3),
	flag_opt_xmalloc	= (1U << 4)
};
static uint8_t	malloc_slow_flags;

#ifdef JEMALLOC_THREADED_INIT
/* Used to let the initializing thread recursively allocate. */
#  define NO_INITIALIZER	((unsigned long)0)
#  define INITIALIZER		pthread_self()
#  define IS_INITIALIZER	(malloc_initializer == pthread_self())
static pthread_t		malloc_initializer = NO_INITIALIZER;
#else
#  define NO_INITIALIZER	false
#  define INITIALIZER		true
#  define IS_INITIALIZER	malloc_initializer
static bool			malloc_initializer = NO_INITIALIZER;
#endif

/* Used to avoid initialization races. */
#ifdef _WIN32
#if _WIN32_WINNT >= 0x0600
static malloc_mutex_t	init_lock = SRWLOCK_INIT;
#else
static malloc_mutex_t	init_lock;
static bool init_lock_initialized = false;

JEMALLOC_ATTR(constructor)
static void WINAPI
_init_init_lock(void) {
	/*
	 * If another constructor in the same binary is using mallctl to e.g.
	 * set up extent hooks, it may end up running before this one, and
	 * malloc_init_hard will crash trying to lock the uninitialized lock. So
	 * we force an initialization of the lock in malloc_init_hard as well.
	 * We don't try to care about atomicity of the accessed to the
	 * init_lock_initialized boolean, since it really only matters early in
	 * the process creation, before any separate thread normally starts
	 * doing anything.
	 */
	if (!init_lock_initialized) {
		malloc_mutex_init(&init_lock, "init", WITNESS_RANK_INIT,
		    malloc_mutex_rank_exclusive);
	}
	init_lock_initialized = true;
}

#ifdef _MSC_VER
#  pragma section(".CRT$XCU", read)
JEMALLOC_SECTION(".CRT$XCU") JEMALLOC_ATTR(used)
static const void (WINAPI *init_init_lock)(void) = _init_init_lock;
#endif
#endif
#else
static malloc_mutex_t	init_lock = MALLOC_MUTEX_INITIALIZER;
#endif

typedef struct {
	void	*p;	/* Input pointer (as in realloc(p, s)). */
	size_t	s;	/* Request size. */
	void	*r;	/* Result pointer. */
} malloc_utrace_t;

#ifdef JEMALLOC_UTRACE
#  define UTRACE(a, b, c) do {						\
	if (unlikely(opt_utrace)) {					\
		int utrace_serrno = errno;				\
		malloc_utrace_t ut;					\
		ut.p = (a);						\
		ut.s = (b);						\
		ut.r = (c);						\
		utrace(&ut, sizeof(ut));				\
		errno = utrace_serrno;					\
	}								\
} while (0)
#else
#  define UTRACE(a, b, c)
#endif

/* Whether encountered any invalid config options. */
static bool had_conf_error = false;

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static bool	malloc_init_hard_a0(void);
static bool	malloc_init_hard(void);

/******************************************************************************/
/*
 * Begin miscellaneous support functions.
 */

bool
malloc_initialized(void) {
	return (malloc_init_state == malloc_init_initialized);
}

JEMALLOC_ALWAYS_INLINE bool
malloc_init_a0(void) {
	if (unlikely(malloc_init_state == malloc_init_uninitialized)) {
		return malloc_init_hard_a0();
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE bool
malloc_init(void) {
	if (unlikely(!malloc_initialized()) && malloc_init_hard()) {
		return true;
	}
	return false;
}

/*
 * The a0*() functions are used instead of i{d,}alloc() in situations that
 * cannot tolerate TLS variable access.
 */

static void *
a0ialloc(size_t size, bool zero, bool is_internal) {
	if (unlikely(malloc_init_a0())) {
		return NULL;
	}

	return iallocztm(TSDN_NULL, size, sz_size2index(size), zero, NULL,
	    is_internal, arena_get(TSDN_NULL, 0, true), true);
}

static void
a0idalloc(void *ptr, bool is_internal) {
	idalloctm(TSDN_NULL, ptr, NULL, NULL, is_internal, true);
}

void *
a0malloc(size_t size) {
	return a0ialloc(size, false, true);
}

void
a0dalloc(void *ptr) {
	a0idalloc(ptr, true);
}

/*
 * FreeBSD's libc uses the bootstrap_*() functions in bootstrap-senstive
 * situations that cannot tolerate TLS variable access (TLS allocation and very
 * early internal data structure initialization).
 */

void *
bootstrap_malloc(size_t size) {
	if (unlikely(size == 0)) {
		size = 1;
	}

	return a0ialloc(size, false, false);
}

void *
bootstrap_calloc(size_t num, size_t size) {
	size_t num_size;

	num_size = num * size;
	if (unlikely(num_size == 0)) {
		assert(num == 0 || size == 0);
		num_size = 1;
	}

	return a0ialloc(num_size, true, false);
}

void
bootstrap_free(void *ptr) {
	if (unlikely(ptr == NULL)) {
		return;
	}

	a0idalloc(ptr, false);
}

void
arena_set(unsigned ind, arena_t *arena) {
	atomic_store_p(&arenas[ind], arena, ATOMIC_RELEASE);
}

static void
narenas_total_set(unsigned narenas) {
	atomic_store_u(&narenas_total, narenas, ATOMIC_RELEASE);
}

static void
narenas_total_inc(void) {
	atomic_fetch_add_u(&narenas_total, 1, ATOMIC_RELEASE);
}

unsigned
narenas_total_get(void) {
	return atomic_load_u(&narenas_total, ATOMIC_ACQUIRE);
}

/* Create a new arena and insert it into the arenas array at index ind. */
static arena_t *
arena_init_locked(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks) {
	arena_t *arena;

	assert(ind <= narenas_total_get());
	if (ind >= MALLOCX_ARENA_LIMIT) {
		return NULL;
	}
	if (ind == narenas_total_get()) {
		narenas_total_inc();
	}

	/*
	 * Another thread may have already initialized arenas[ind] if it's an
	 * auto arena.
	 */
	arena = arena_get(tsdn, ind, false);
	if (arena != NULL) {
		assert(arena_is_auto(arena));
		return arena;
	}

	/* Actually initialize the arena. */
	arena = arena_new(tsdn, ind, extent_hooks);

	return arena;
}

static void
arena_new_create_background_thread(tsdn_t *tsdn, unsigned ind) {
	if (ind == 0) {
		return;
	}
	/*
	 * Avoid creating a new background thread just for the huge arena, which
	 * purges eagerly by default.
	 */
	if (have_background_thread && !arena_is_huge(ind)) {
		if (background_thread_create(tsdn_tsd(tsdn), ind)) {
			malloc_printf("<jemalloc>: error in background thread "
				      "creation for arena %u. Abort.\n", ind);
			abort();
		}
	}
}

arena_t *
arena_init(tsdn_t *tsdn, unsigned ind, extent_hooks_t *extent_hooks) {
	arena_t *arena;

	malloc_mutex_lock(tsdn, &arenas_lock);
	arena = arena_init_locked(tsdn, ind, extent_hooks);
	malloc_mutex_unlock(tsdn, &arenas_lock);

	arena_new_create_background_thread(tsdn, ind);

	return arena;
}

static void
arena_bind(tsd_t *tsd, unsigned ind, bool internal) {
	arena_t *arena = arena_get(tsd_tsdn(tsd), ind, false);
	arena_nthreads_inc(arena, internal);

	if (internal) {
		tsd_iarena_set(tsd, arena);
	} else {
		tsd_arena_set(tsd, arena);
		unsigned shard = atomic_fetch_add_u(&arena->binshard_next, 1,
		    ATOMIC_RELAXED);
		tsd_binshards_t *bins = tsd_binshardsp_get(tsd);
		for (unsigned i = 0; i < SC_NBINS; i++) {
			assert(bin_infos[i].n_shards > 0 &&
			    bin_infos[i].n_shards <= BIN_SHARDS_MAX);
			bins->binshard[i] = shard % bin_infos[i].n_shards;
		}
	}
}

void
arena_migrate(tsd_t *tsd, unsigned oldind, unsigned newind) {
	arena_t *oldarena, *newarena;

	oldarena = arena_get(tsd_tsdn(tsd), oldind, false);
	newarena = arena_get(tsd_tsdn(tsd), newind, false);
	arena_nthreads_dec(oldarena, false);
	arena_nthreads_inc(newarena, false);
	tsd_arena_set(tsd, newarena);
}

static void
arena_unbind(tsd_t *tsd, unsigned ind, bool internal) {
	arena_t *arena;

	arena = arena_get(tsd_tsdn(tsd), ind, false);
	arena_nthreads_dec(arena, internal);

	if (internal) {
		tsd_iarena_set(tsd, NULL);
	} else {
		tsd_arena_set(tsd, NULL);
	}
}

arena_tdata_t *
arena_tdata_get_hard(tsd_t *tsd, unsigned ind) {
	arena_tdata_t *tdata, *arenas_tdata_old;
	arena_tdata_t *arenas_tdata = tsd_arenas_tdata_get(tsd);
	unsigned narenas_tdata_old, i;
	unsigned narenas_tdata = tsd_narenas_tdata_get(tsd);
	unsigned narenas_actual = narenas_total_get();

	/*
	 * Dissociate old tdata array (and set up for deallocation upon return)
	 * if it's too small.
	 */
	if (arenas_tdata != NULL && narenas_tdata < narenas_actual) {
		arenas_tdata_old = arenas_tdata;
		narenas_tdata_old = narenas_tdata;
		arenas_tdata = NULL;
		narenas_tdata = 0;
		tsd_arenas_tdata_set(tsd, arenas_tdata);
		tsd_narenas_tdata_set(tsd, narenas_tdata);
	} else {
		arenas_tdata_old = NULL;
		narenas_tdata_old = 0;
	}

	/* Allocate tdata array if it's missing. */
	if (arenas_tdata == NULL) {
		bool *arenas_tdata_bypassp = tsd_arenas_tdata_bypassp_get(tsd);
		narenas_tdata = (ind < narenas_actual) ? narenas_actual : ind+1;

		if (tsd_nominal(tsd) && !*arenas_tdata_bypassp) {
			*arenas_tdata_bypassp = true;
			arenas_tdata = (arena_tdata_t *)a0malloc(
			    sizeof(arena_tdata_t) * narenas_tdata);
			*arenas_tdata_bypassp = false;
		}
		if (arenas_tdata == NULL) {
			tdata = NULL;
			goto label_return;
		}
		assert(tsd_nominal(tsd) && !*arenas_tdata_bypassp);
		tsd_arenas_tdata_set(tsd, arenas_tdata);
		tsd_narenas_tdata_set(tsd, narenas_tdata);
	}

	/*
	 * Copy to tdata array.  It's possible that the actual number of arenas
	 * has increased since narenas_total_get() was called above, but that
	 * causes no correctness issues unless two threads concurrently execute
	 * the arenas.create mallctl, which we trust mallctl synchronization to
	 * prevent.
	 */

	/* Copy/initialize tickers. */
	for (i = 0; i < narenas_actual; i++) {
		if (i < narenas_tdata_old) {
			ticker_copy(&arenas_tdata[i].decay_ticker,
			    &arenas_tdata_old[i].decay_ticker);
		} else {
			ticker_init(&arenas_tdata[i].decay_ticker,
			    DECAY_NTICKS_PER_UPDATE);
		}
	}
	if (narenas_tdata > narenas_actual) {
		memset(&arenas_tdata[narenas_actual], 0, sizeof(arena_tdata_t)
		    * (narenas_tdata - narenas_actual));
	}

	/* Read the refreshed tdata array. */
	tdata = &arenas_tdata[ind];
label_return:
	if (arenas_tdata_old != NULL) {
		a0dalloc(arenas_tdata_old);
	}
	return tdata;
}

/* Slow path, called only by arena_choose(). */
arena_t *
arena_choose_hard(tsd_t *tsd, bool internal) {
	arena_t *ret JEMALLOC_CC_SILENCE_INIT(NULL);

	if (have_percpu_arena && PERCPU_ARENA_ENABLED(opt_percpu_arena)) {
		unsigned choose = percpu_arena_choose();
		ret = arena_get(tsd_tsdn(tsd), choose, true);
		assert(ret != NULL);
		arena_bind(tsd, arena_ind_get(ret), false);
		arena_bind(tsd, arena_ind_get(ret), true);

		return ret;
	}

	if (narenas_auto > 1) {
		unsigned i, j, choose[2], first_null;
		bool is_new_arena[2];

		/*
		 * Determine binding for both non-internal and internal
		 * allocation.
		 *
		 *   choose[0]: For application allocation.
		 *   choose[1]: For internal metadata allocation.
		 */

		for (j = 0; j < 2; j++) {
			choose[j] = 0;
			is_new_arena[j] = false;
		}

		first_null = narenas_auto;
		malloc_mutex_lock(tsd_tsdn(tsd), &arenas_lock);
		assert(arena_get(tsd_tsdn(tsd), 0, false) != NULL);
		for (i = 1; i < narenas_auto; i++) {
			if (arena_get(tsd_tsdn(tsd), i, false) != NULL) {
				/*
				 * Choose the first arena that has the lowest
				 * number of threads assigned to it.
				 */
				for (j = 0; j < 2; j++) {
					if (arena_nthreads_get(arena_get(
					    tsd_tsdn(tsd), i, false), !!j) <
					    arena_nthreads_get(arena_get(
					    tsd_tsdn(tsd), choose[j], false),
					    !!j)) {
						choose[j] = i;
					}
				}
			} else if (first_null == narenas_auto) {
				/*
				 * Record the index of the first uninitialized
				 * arena, in case all extant arenas are in use.
				 *
				 * NB: It is possible for there to be
				 * discontinuities in terms of initialized
				 * versus uninitialized arenas, due to the
				 * "thread.arena" mallctl.
				 */
				first_null = i;
			}
		}

		for (j = 0; j < 2; j++) {
			if (arena_nthreads_get(arena_get(tsd_tsdn(tsd),
			    choose[j], false), !!j) == 0 || first_null ==
			    narenas_auto) {
				/*
				 * Use an unloaded arena, or the least loaded
				 * arena if all arenas are already initialized.
				 */
				if (!!j == internal) {
					ret = arena_get(tsd_tsdn(tsd),
					    choose[j], false);
				}
			} else {
				arena_t *arena;

				/* Initialize a new arena. */
				choose[j] = first_null;
				arena = arena_init_locked(tsd_tsdn(tsd),
				    choose[j],
				    (extent_hooks_t *)&extent_hooks_default);
				if (arena == NULL) {
					malloc_mutex_unlock(tsd_tsdn(tsd),
					    &arenas_lock);
					return NULL;
				}
				is_new_arena[j] = true;
				if (!!j == internal) {
					ret = arena;
				}
			}
			arena_bind(tsd, choose[j], !!j);
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &arenas_lock);

		for (j = 0; j < 2; j++) {
			if (is_new_arena[j]) {
				assert(choose[j] > 0);
				arena_new_create_background_thread(
				    tsd_tsdn(tsd), choose[j]);
			}
		}

	} else {
		ret = arena_get(tsd_tsdn(tsd), 0, false);
		arena_bind(tsd, 0, false);
		arena_bind(tsd, 0, true);
	}

	return ret;
}

void
iarena_cleanup(tsd_t *tsd) {
	arena_t *iarena;

	iarena = tsd_iarena_get(tsd);
	if (iarena != NULL) {
		arena_unbind(tsd, arena_ind_get(iarena), true);
	}
}

void
arena_cleanup(tsd_t *tsd) {
	arena_t *arena;

	arena = tsd_arena_get(tsd);
	if (arena != NULL) {
		arena_unbind(tsd, arena_ind_get(arena), false);
	}
}

void
arenas_tdata_cleanup(tsd_t *tsd) {
	arena_tdata_t *arenas_tdata;

	/* Prevent tsd->arenas_tdata from being (re)created. */
	*tsd_arenas_tdata_bypassp_get(tsd) = true;

	arenas_tdata = tsd_arenas_tdata_get(tsd);
	if (arenas_tdata != NULL) {
		tsd_arenas_tdata_set(tsd, NULL);
		a0dalloc(arenas_tdata);
	}
}

static void
stats_print_atexit(void) {
	if (config_stats) {
		tsdn_t *tsdn;
		unsigned narenas, i;

		tsdn = tsdn_fetch();

		/*
		 * Merge stats from extant threads.  This is racy, since
		 * individual threads do not lock when recording tcache stats
		 * events.  As a consequence, the final stats may be slightly
		 * out of date by the time they are reported, if other threads
		 * continue to allocate.
		 */
		for (i = 0, narenas = narenas_total_get(); i < narenas; i++) {
			arena_t *arena = arena_get(tsdn, i, false);
			if (arena != NULL) {
				tcache_t *tcache;

				malloc_mutex_lock(tsdn, &arena->tcache_ql_mtx);
				ql_foreach(tcache, &arena->tcache_ql, link) {
					tcache_stats_merge(tsdn, tcache, arena);
				}
				malloc_mutex_unlock(tsdn,
				    &arena->tcache_ql_mtx);
			}
		}
	}
	je_malloc_stats_print(NULL, NULL, opt_stats_print_opts);
}

/*
 * Ensure that we don't hold any locks upon entry to or exit from allocator
 * code (in a "broad" sense that doesn't count a reentrant allocation as an
 * entrance or exit).
 */
JEMALLOC_ALWAYS_INLINE void
check_entry_exit_locking(tsdn_t *tsdn) {
	if (!config_debug) {
		return;
	}
	if (tsdn_null(tsdn)) {
		return;
	}
	tsd_t *tsd = tsdn_tsd(tsdn);
	/*
	 * It's possible we hold locks at entry/exit if we're in a nested
	 * allocation.
	 */
	int8_t reentrancy_level = tsd_reentrancy_level_get(tsd);
	if (reentrancy_level != 0) {
		return;
	}
	witness_assert_lockless(tsdn_witness_tsdp_get(tsdn));
}

/*
 * End miscellaneous support functions.
 */
/******************************************************************************/
/*
 * Begin initialization functions.
 */

static char *
jemalloc_secure_getenv(const char *name) {
#ifdef JEMALLOC_HAVE_SECURE_GETENV
	return secure_getenv(name);
#else
#  ifdef JEMALLOC_HAVE_ISSETUGID
	if (issetugid() != 0) {
		return NULL;
	}
#  endif
	return getenv(name);
#endif
}

static unsigned
malloc_ncpus(void) {
	long result;

#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	result = si.dwNumberOfProcessors;
#elif defined(JEMALLOC_GLIBC_MALLOC_HOOK) && defined(CPU_COUNT)
	/*
	 * glibc >= 2.6 has the CPU_COUNT macro.
	 *
	 * glibc's sysconf() uses isspace().  glibc allocates for the first time
	 * *before* setting up the isspace tables.  Therefore we need a
	 * different method to get the number of CPUs.
	 */
	{
		cpu_set_t set;

		pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
		result = CPU_COUNT(&set);
	}
#else
	result = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	return ((result == -1) ? 1 : (unsigned)result);
}

static void
init_opt_stats_print_opts(const char *v, size_t vlen) {
	size_t opts_len = strlen(opt_stats_print_opts);
	assert(opts_len <= stats_print_tot_num_options);

	for (size_t i = 0; i < vlen; i++) {
		switch (v[i]) {
#define OPTION(o, v, d, s) case o: break;
			STATS_PRINT_OPTIONS
#undef OPTION
		default: continue;
		}

		if (strchr(opt_stats_print_opts, v[i]) != NULL) {
			/* Ignore repeated. */
			continue;
		}

		opt_stats_print_opts[opts_len++] = v[i];
		opt_stats_print_opts[opts_len] = '\0';
		assert(opts_len <= stats_print_tot_num_options);
	}
	assert(opts_len == strlen(opt_stats_print_opts));
}

/* Reads the next size pair in a multi-sized option. */
static bool
malloc_conf_multi_sizes_next(const char **slab_size_segment_cur,
    size_t *vlen_left, size_t *slab_start, size_t *slab_end, size_t *new_size) {
	const char *cur = *slab_size_segment_cur;
	char *end;
	uintmax_t um;

	set_errno(0);

	/* First number, then '-' */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0 || *end != '-') {
		return true;
	}
	*slab_start = (size_t)um;
	cur = end + 1;

	/* Second number, then ':' */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0 || *end != ':') {
		return true;
	}
	*slab_end = (size_t)um;
	cur = end + 1;

	/* Last number */
	um = malloc_strtoumax(cur, &end, 0);
	if (get_errno() != 0) {
		return true;
	}
	*new_size = (size_t)um;

	/* Consume the separator if there is one. */
	if (*end == '|') {
		end++;
	}

	*vlen_left -= end - *slab_size_segment_cur;
	*slab_size_segment_cur = end;

	return false;
}

static bool
malloc_conf_next(char const **opts_p, char const **k_p, size_t *klen_p,
    char const **v_p, size_t *vlen_p) {
	bool accept;
	const char *opts = *opts_p;

	*k_p = opts;

	for (accept = false; !accept;) {
		switch (*opts) {
		case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
		case 'G': case 'H': case 'I': case 'J': case 'K': case 'L':
		case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
		case 'S': case 'T': case 'U': case 'V': case 'W': case 'X':
		case 'Y': case 'Z':
		case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
		case 'g': case 'h': case 'i': case 'j': case 'k': case 'l':
		case 'm': case 'n': case 'o': case 'p': case 'q': case 'r':
		case 's': case 't': case 'u': case 'v': case 'w': case 'x':
		case 'y': case 'z':
		case '0': case '1': case '2': case '3': case '4': case '5':
		case '6': case '7': case '8': case '9':
		case '_':
			opts++;
			break;
		case ':':
			opts++;
			*klen_p = (uintptr_t)opts - 1 - (uintptr_t)*k_p;
			*v_p = opts;
			accept = true;
			break;
		case '\0':
			if (opts != *opts_p) {
				malloc_write("<jemalloc>: Conf string ends "
				    "with key\n");
			}
			return true;
		default:
			malloc_write("<jemalloc>: Malformed conf string\n");
			return true;
		}
	}

	for (accept = false; !accept;) {
		switch (*opts) {
		case ',':
			opts++;
			/*
			 * Look ahead one character here, because the next time
			 * this function is called, it will assume that end of
			 * input has been cleanly reached if no input remains,
			 * but we have optimistically already consumed the
			 * comma if one exists.
			 */
			if (*opts == '\0') {
				malloc_write("<jemalloc>: Conf string ends "
				    "with comma\n");
			}
			*vlen_p = (uintptr_t)opts - 1 - (uintptr_t)*v_p;
			accept = true;
			break;
		case '\0':
			*vlen_p = (uintptr_t)opts - (uintptr_t)*v_p;
			accept = true;
			break;
		default:
			opts++;
			break;
		}
	}

	*opts_p = opts;
	return false;
}

static void
malloc_abort_invalid_conf(void) {
	assert(opt_abort_conf);
	malloc_printf("<jemalloc>: Abort (abort_conf:true) on invalid conf "
	    "value (see above).\n");
	abort();
}

static void
malloc_conf_error(const char *msg, const char *k, size_t klen, const char *v,
    size_t vlen) {
	malloc_printf("<jemalloc>: %s: %.*s:%.*s\n", msg, (int)klen, k,
	    (int)vlen, v);
	/* If abort_conf is set, error out after processing all options. */
	const char *experimental = "experimental_";
	if (strncmp(k, experimental, strlen(experimental)) == 0) {
		/* However, tolerate experimental features. */
		return;
	}
	had_conf_error = true;
}

static void
malloc_slow_flag_init(void) {
	/*
	 * Combine the runtime options into malloc_slow for fast path.  Called
	 * after processing all the options.
	 */
	malloc_slow_flags |= (opt_junk_alloc ? flag_opt_junk_alloc : 0)
	    | (opt_junk_free ? flag_opt_junk_free : 0)
	    | (opt_zero ? flag_opt_zero : 0)
	    | (opt_utrace ? flag_opt_utrace : 0)
	    | (opt_xmalloc ? flag_opt_xmalloc : 0);

	malloc_slow = (malloc_slow_flags != 0);
}

/* Number of sources for initializing malloc_conf */
#define MALLOC_CONF_NSOURCES 4

static const char *
obtain_malloc_conf(unsigned which_source, char buf[PATH_MAX + 1]) {
	if (config_debug) {
		static unsigned read_source = 0;
		/*
		 * Each source should only be read once, to minimize # of
		 * syscalls on init.
		 */
		assert(read_source++ == which_source);
	}
	assert(which_source < MALLOC_CONF_NSOURCES);

	const char *ret;
	switch (which_source) {
	case 0:
		ret = config_malloc_conf;
		break;
	case 1:
		if (je_malloc_conf != NULL) {
			/* Use options that were compiled into the program. */
			ret = je_malloc_conf;
		} else {
			/* No configuration specified. */
			ret = NULL;
		}
		break;
	case 2: {
		ssize_t linklen = 0;
#ifndef _WIN32
		int saved_errno = errno;
		const char *linkname =
#  ifdef JEMALLOC_PREFIX
		    "/etc/"JEMALLOC_PREFIX"malloc.conf"
#  else
		    "/etc/malloc.conf"
#  endif
		    ;

		/*
		 * Try to use the contents of the "/etc/malloc.conf" symbolic
		 * link's name.
		 */
#ifndef JEMALLOC_READLINKAT
		linklen = readlink(linkname, buf, PATH_MAX);
#else
		linklen = readlinkat(AT_FDCWD, linkname, buf, PATH_MAX);
#endif
		if (linklen == -1) {
			/* No configuration specified. */
			linklen = 0;
			/* Restore errno. */
			set_errno(saved_errno);
		}
#endif
		buf[linklen] = '\0';
		ret = buf;
		break;
	} case 3: {
		const char *envname =
#ifdef JEMALLOC_PREFIX
		    JEMALLOC_CPREFIX"MALLOC_CONF"
#else
		    "MALLOC_CONF"
#endif
		    ;

		if ((ret = jemalloc_secure_getenv(envname)) != NULL) {
			/*
			 * Do nothing; opts is already initialized to the value
			 * of the MALLOC_CONF environment variable.
			 */
		} else {
			/* No configuration specified. */
			ret = NULL;
		}
		break;
	} default:
		not_reached();
		ret = NULL;
	}
	return ret;
}

static void
malloc_conf_init_helper(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS],
    bool initial_call, const char *opts_cache[MALLOC_CONF_NSOURCES],
    char buf[PATH_MAX + 1]) {
	static const char *opts_explain[MALLOC_CONF_NSOURCES] = {
		"string specified via --with-malloc-conf",
		"string pointed to by the global variable malloc_conf",
		"\"name\" of the file referenced by the symbolic link named "
		    "/etc/malloc.conf",
		"value of the environment variable MALLOC_CONF"
	};
	unsigned i;
	const char *opts, *k, *v;
	size_t klen, vlen;

	for (i = 0; i < MALLOC_CONF_NSOURCES; i++) {
		/* Get runtime configuration. */
		if (initial_call) {
			opts_cache[i] = obtain_malloc_conf(i, buf);
		}
		opts = opts_cache[i];
		if (!initial_call && opt_confirm_conf) {
			malloc_printf(
			    "<jemalloc>: malloc_conf #%u (%s): \"%s\"\n",
			    i + 1, opts_explain[i], opts != NULL ? opts : "");
		}
		if (opts == NULL) {
			continue;
		}

		while (*opts != '\0' && !malloc_conf_next(&opts, &k, &klen, &v,
		    &vlen)) {

#define CONF_ERROR(msg, k, klen, v, vlen)				\
			if (!initial_call) {				\
				malloc_conf_error(			\
				    msg, k, klen, v, vlen);		\
				cur_opt_valid = false;			\
			}
#define CONF_CONTINUE	{						\
				if (!initial_call && opt_confirm_conf	\
				    && cur_opt_valid) {			\
					malloc_printf("<jemalloc>: -- "	\
					    "Set conf value: %.*s:%.*s"	\
					    "\n", (int)klen, k,		\
					    (int)vlen, v);		\
				}					\
				continue;				\
			}
#define CONF_MATCH(n)							\
	(sizeof(n)-1 == klen && strncmp(n, k, klen) == 0)
#define CONF_MATCH_VALUE(n)						\
	(sizeof(n)-1 == vlen && strncmp(n, v, vlen) == 0)
#define CONF_HANDLE_BOOL(o, n)						\
			if (CONF_MATCH(n)) {				\
				if (CONF_MATCH_VALUE("true")) {		\
					o = true;			\
				} else if (CONF_MATCH_VALUE("false")) {	\
					o = false;			\
				} else {				\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				}					\
				CONF_CONTINUE;				\
			}
      /*
       * One of the CONF_MIN macros below expands, in one of the use points,
       * to "unsigned integer < 0", which is always false, triggering the
       * GCC -Wtype-limits warning, which we disable here and re-enable below.
       */
      JEMALLOC_DIAGNOSTIC_PUSH
      JEMALLOC_DIAGNOSTIC_IGNORE_TYPE_LIMITS

#define CONF_DONT_CHECK_MIN(um, min)	false
#define CONF_CHECK_MIN(um, min)	((um) < (min))
#define CONF_DONT_CHECK_MAX(um, max)	false
#define CONF_CHECK_MAX(um, max)	((um) > (max))
#define CONF_HANDLE_T_U(t, o, n, min, max, check_min, check_max, clip)	\
			if (CONF_MATCH(n)) {				\
				uintmax_t um;				\
				char *end;				\
									\
				set_errno(0);				\
				um = malloc_strtoumax(v, &end, 0);	\
				if (get_errno() != 0 || (uintptr_t)end -\
				    (uintptr_t)v != vlen) {		\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				} else if (clip) {			\
					if (check_min(um, (t)(min))) {	\
						o = (t)(min);		\
					} else if (			\
					    check_max(um, (t)(max))) {	\
						o = (t)(max);		\
					} else {			\
						o = (t)um;		\
					}				\
				} else {				\
					if (check_min(um, (t)(min)) ||	\
					    check_max(um, (t)(max))) {	\
						CONF_ERROR(		\
						    "Out-of-range "	\
						    "conf value",	\
						    k, klen, v, vlen);	\
					} else {			\
						o = (t)um;		\
					}				\
				}					\
				CONF_CONTINUE;				\
			}
#define CONF_HANDLE_UNSIGNED(o, n, min, max, check_min, check_max,	\
    clip)								\
			CONF_HANDLE_T_U(unsigned, o, n, min, max,	\
			    check_min, check_max, clip)
#define CONF_HANDLE_SIZE_T(o, n, min, max, check_min, check_max, clip)	\
			CONF_HANDLE_T_U(size_t, o, n, min, max,		\
			    check_min, check_max, clip)
#define CONF_HANDLE_SSIZE_T(o, n, min, max)				\
			if (CONF_MATCH(n)) {				\
				long l;					\
				char *end;				\
									\
				set_errno(0);				\
				l = strtol(v, &end, 0);			\
				if (get_errno() != 0 || (uintptr_t)end -\
				    (uintptr_t)v != vlen) {		\
					CONF_ERROR("Invalid conf value",\
					    k, klen, v, vlen);		\
				} else if (l < (ssize_t)(min) || l >	\
				    (ssize_t)(max)) {			\
					CONF_ERROR(			\
					    "Out-of-range conf value",	\
					    k, klen, v, vlen);		\
				} else {				\
					o = l;				\
				}					\
				CONF_CONTINUE;				\
			}
#define CONF_HANDLE_CHAR_P(o, n, d)					\
			if (CONF_MATCH(n)) {				\
				size_t cpylen = (vlen <=		\
				    sizeof(o)-1) ? vlen :		\
				    sizeof(o)-1;			\
				strncpy(o, v, cpylen);			\
				o[cpylen] = '\0';			\
				CONF_CONTINUE;				\
			}

			bool cur_opt_valid = true;

			CONF_HANDLE_BOOL(opt_confirm_conf, "confirm_conf")
			if (initial_call) {
				continue;
			}

			CONF_HANDLE_BOOL(opt_abort, "abort")
			CONF_HANDLE_BOOL(opt_abort_conf, "abort_conf")
			if (strncmp("metadata_thp", k, klen) == 0) {
				int i;
				bool match = false;
				for (i = 0; i < metadata_thp_mode_limit; i++) {
					if (strncmp(metadata_thp_mode_names[i],
					    v, vlen) == 0) {
						opt_metadata_thp = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_HANDLE_BOOL(opt_retain, "retain")
			if (strncmp("dss", k, klen) == 0) {
				int i;
				bool match = false;
				for (i = 0; i < dss_prec_limit; i++) {
					if (strncmp(dss_prec_names[i], v, vlen)
					    == 0) {
						if (extent_dss_prec_set(i)) {
							CONF_ERROR(
							    "Error setting dss",
							    k, klen, v, vlen);
						} else {
							opt_dss =
							    dss_prec_names[i];
							match = true;
							break;
						}
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_HANDLE_UNSIGNED(opt_narenas, "narenas", 1,
			    UINT_MAX, CONF_CHECK_MIN, CONF_DONT_CHECK_MAX,
			    false)
			if (CONF_MATCH("bin_shards")) {
				const char *bin_shards_segment_cur = v;
				size_t vlen_left = vlen;
				do {
					size_t size_start;
					size_t size_end;
					size_t nshards;
					bool err = malloc_conf_multi_sizes_next(
					    &bin_shards_segment_cur, &vlen_left,
					    &size_start, &size_end, &nshards);
					if (err || bin_update_shard_size(
					    bin_shard_sizes, size_start,
					    size_end, nshards)) {
						CONF_ERROR(
						    "Invalid settings for "
						    "bin_shards", k, klen, v,
						    vlen);
						break;
					}
				} while (vlen_left > 0);
				CONF_CONTINUE;
			}
			CONF_HANDLE_SSIZE_T(opt_dirty_decay_ms,
			    "dirty_decay_ms", -1, NSTIME_SEC_MAX * KQU(1000) <
			    QU(SSIZE_MAX) ? NSTIME_SEC_MAX * KQU(1000) :
			    SSIZE_MAX);
			CONF_HANDLE_SSIZE_T(opt_muzzy_decay_ms,
			    "muzzy_decay_ms", -1, NSTIME_SEC_MAX * KQU(1000) <
			    QU(SSIZE_MAX) ? NSTIME_SEC_MAX * KQU(1000) :
			    SSIZE_MAX);
			CONF_HANDLE_BOOL(opt_stats_print, "stats_print")
			if (CONF_MATCH("stats_print_opts")) {
				init_opt_stats_print_opts(v, vlen);
				CONF_CONTINUE;
			}
			if (config_fill) {
				if (CONF_MATCH("junk")) {
					if (CONF_MATCH_VALUE("true")) {
						opt_junk = "true";
						opt_junk_alloc = opt_junk_free =
						    true;
					} else if (CONF_MATCH_VALUE("false")) {
						opt_junk = "false";
						opt_junk_alloc = opt_junk_free =
						    false;
					} else if (CONF_MATCH_VALUE("alloc")) {
						opt_junk = "alloc";
						opt_junk_alloc = true;
						opt_junk_free = false;
					} else if (CONF_MATCH_VALUE("free")) {
						opt_junk = "free";
						opt_junk_alloc = false;
						opt_junk_free = true;
					} else {
						CONF_ERROR(
						    "Invalid conf value",
						    k, klen, v, vlen);
					}
					CONF_CONTINUE;
				}
				CONF_HANDLE_BOOL(opt_zero, "zero")
			}
			if (config_utrace) {
				CONF_HANDLE_BOOL(opt_utrace, "utrace")
			}
			if (config_xmalloc) {
				CONF_HANDLE_BOOL(opt_xmalloc, "xmalloc")
			}
			CONF_HANDLE_BOOL(opt_tcache, "tcache")
			CONF_HANDLE_SSIZE_T(opt_lg_tcache_max, "lg_tcache_max",
			    -1, (sizeof(size_t) << 3) - 1)

			/*
			 * The runtime option of oversize_threshold remains
			 * undocumented.  It may be tweaked in the next major
			 * release (6.0).  The default value 8M is rather
			 * conservative / safe.  Tuning it further down may
			 * improve fragmentation a bit more, but may also cause
			 * contention on the huge arena.
			 */
			CONF_HANDLE_SIZE_T(opt_oversize_threshold,
			    "oversize_threshold", 0, SC_LARGE_MAXCLASS,
			    CONF_DONT_CHECK_MIN, CONF_CHECK_MAX, false)
			CONF_HANDLE_SIZE_T(opt_lg_extent_max_active_fit,
			    "lg_extent_max_active_fit", 0,
			    (sizeof(size_t) << 3), CONF_DONT_CHECK_MIN,
			    CONF_CHECK_MAX, false)

			if (strncmp("percpu_arena", k, klen) == 0) {
				bool match = false;
				for (int i = percpu_arena_mode_names_base; i <
				    percpu_arena_mode_names_limit; i++) {
					if (strncmp(percpu_arena_mode_names[i],
					    v, vlen) == 0) {
						if (!have_percpu_arena) {
							CONF_ERROR(
							    "No getcpu support",
							    k, klen, v, vlen);
						}
						opt_percpu_arena = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_HANDLE_BOOL(opt_background_thread,
			    "background_thread");
			CONF_HANDLE_SIZE_T(opt_max_background_threads,
					   "max_background_threads", 1,
					   opt_max_background_threads,
					   CONF_CHECK_MIN, CONF_CHECK_MAX,
					   true);
			if (CONF_MATCH("slab_sizes")) {
				bool err;
				const char *slab_size_segment_cur = v;
				size_t vlen_left = vlen;
				do {
					size_t slab_start;
					size_t slab_end;
					size_t pgs;
					err = malloc_conf_multi_sizes_next(
					    &slab_size_segment_cur,
					    &vlen_left, &slab_start, &slab_end,
					    &pgs);
					if (!err) {
						sc_data_update_slab_size(
						    sc_data, slab_start,
						    slab_end, (int)pgs);
					} else {
						CONF_ERROR("Invalid settings "
						    "for slab_sizes",
						    k, klen, v, vlen);
					}
				} while (!err && vlen_left > 0);
				CONF_CONTINUE;
			}
			if (config_prof) {
				CONF_HANDLE_BOOL(opt_prof, "prof")
				CONF_HANDLE_CHAR_P(opt_prof_prefix,
				    "prof_prefix", "jeprof")
				CONF_HANDLE_BOOL(opt_prof_active, "prof_active")
				CONF_HANDLE_BOOL(opt_prof_thread_active_init,
				    "prof_thread_active_init")
				CONF_HANDLE_SIZE_T(opt_lg_prof_sample,
				    "lg_prof_sample", 0, (sizeof(uint64_t) << 3)
				    - 1, CONF_DONT_CHECK_MIN, CONF_CHECK_MAX,
				    true)
				CONF_HANDLE_BOOL(opt_prof_accum, "prof_accum")
				CONF_HANDLE_SSIZE_T(opt_lg_prof_interval,
				    "lg_prof_interval", -1,
				    (sizeof(uint64_t) << 3) - 1)
				CONF_HANDLE_BOOL(opt_prof_gdump, "prof_gdump")
				CONF_HANDLE_BOOL(opt_prof_final, "prof_final")
				CONF_HANDLE_BOOL(opt_prof_leak, "prof_leak")
				CONF_HANDLE_BOOL(opt_prof_log, "prof_log")
			}
			if (config_log) {
				if (CONF_MATCH("log")) {
					size_t cpylen = (
					    vlen <= sizeof(log_var_names) ?
					    vlen : sizeof(log_var_names) - 1);
					strncpy(log_var_names, v, cpylen);
					log_var_names[cpylen] = '\0';
					CONF_CONTINUE;
				}
			}
			if (CONF_MATCH("thp")) {
				bool match = false;
				for (int i = 0; i < thp_mode_names_limit; i++) {
					if (strncmp(thp_mode_names[i],v, vlen)
					    == 0) {
						if (!have_madvise_huge) {
							CONF_ERROR(
							    "No THP support",
							    k, klen, v, vlen);
						}
						opt_thp = i;
						match = true;
						break;
					}
				}
				if (!match) {
					CONF_ERROR("Invalid conf value",
					    k, klen, v, vlen);
				}
				CONF_CONTINUE;
			}
			CONF_ERROR("Invalid conf pair", k, klen, v, vlen);
#undef CONF_ERROR
#undef CONF_CONTINUE
#undef CONF_MATCH
#undef CONF_MATCH_VALUE
#undef CONF_HANDLE_BOOL
#undef CONF_DONT_CHECK_MIN
#undef CONF_CHECK_MIN
#undef CONF_DONT_CHECK_MAX
#undef CONF_CHECK_MAX
#undef CONF_HANDLE_T_U
#undef CONF_HANDLE_UNSIGNED
#undef CONF_HANDLE_SIZE_T
#undef CONF_HANDLE_SSIZE_T
#undef CONF_HANDLE_CHAR_P
    /* Re-enable diagnostic "-Wtype-limits" */
    JEMALLOC_DIAGNOSTIC_POP
		}
		if (opt_abort_conf && had_conf_error) {
			malloc_abort_invalid_conf();
		}
	}
	atomic_store_b(&log_init_done, true, ATOMIC_RELEASE);
}

static void
malloc_conf_init(sc_data_t *sc_data, unsigned bin_shard_sizes[SC_NBINS]) {
	const char *opts_cache[MALLOC_CONF_NSOURCES] = {NULL, NULL, NULL, NULL};
	char buf[PATH_MAX + 1];

	/* The first call only set the confirm_conf option and opts_cache */
	malloc_conf_init_helper(NULL, NULL, true, opts_cache, buf);
	malloc_conf_init_helper(sc_data, bin_shard_sizes, false, opts_cache,
	    NULL);
}

#undef MALLOC_CONF_NSOURCES

static bool
malloc_init_hard_needed(void) {
	if (malloc_initialized() || (IS_INITIALIZER && malloc_init_state ==
	    malloc_init_recursible)) {
		/*
		 * Another thread initialized the allocator before this one
		 * acquired init_lock, or this thread is the initializing
		 * thread, and it is recursively allocating.
		 */
		return false;
	}
#ifdef JEMALLOC_THREADED_INIT
	if (malloc_initializer != NO_INITIALIZER && !IS_INITIALIZER) {
		/* Busy-wait until the initializing thread completes. */
		spin_t spinner = SPIN_INITIALIZER;
		do {
			malloc_mutex_unlock(TSDN_NULL, &init_lock);
			spin_adaptive(&spinner);
			malloc_mutex_lock(TSDN_NULL, &init_lock);
		} while (!malloc_initialized());
		return false;
	}
#endif
	return true;
}

static bool
malloc_init_hard_a0_locked() {
	malloc_initializer = INITIALIZER;

	JEMALLOC_DIAGNOSTIC_PUSH
	JEMALLOC_DIAGNOSTIC_IGNORE_MISSING_STRUCT_FIELD_INITIALIZERS
	sc_data_t sc_data = {0};
	JEMALLOC_DIAGNOSTIC_POP

	/*
	 * Ordering here is somewhat tricky; we need sc_boot() first, since that
	 * determines what the size classes will be, and then
	 * malloc_conf_init(), since any slab size tweaking will need to be done
	 * before sz_boot and bin_boot, which assume that the values they read
	 * out of sc_data_global are final.
	 */
	sc_boot(&sc_data);
	unsigned bin_shard_sizes[SC_NBINS];
	bin_shard_sizes_boot(bin_shard_sizes);
	/*
	 * prof_boot0 only initializes opt_prof_prefix.  We need to do it before
	 * we parse malloc_conf options, in case malloc_conf parsing overwrites
	 * it.
	 */
	if (config_prof) {
		prof_boot0();
	}
	malloc_conf_init(&sc_data, bin_shard_sizes);
	sz_boot(&sc_data);
	bin_boot(&sc_data, bin_shard_sizes);

	if (opt_stats_print) {
		/* Print statistics at exit. */
		if (atexit(stats_print_atexit) != 0) {
			malloc_write("<jemalloc>: Error in atexit()\n");
			if (opt_abort) {
				abort();
			}
		}
	}
	if (pages_boot()) {
		return true;
	}
	if (base_boot(TSDN_NULL)) {
		return true;
	}
	if (extent_boot()) {
		return true;
	}
	if (ctl_boot()) {
		return true;
	}
	if (config_prof) {
		prof_boot1();
	}
	arena_boot(&sc_data);
	if (tcache_boot(TSDN_NULL)) {
		return true;
	}
	if (malloc_mutex_init(&arenas_lock, "arenas", WITNESS_RANK_ARENAS,
	    malloc_mutex_rank_exclusive)) {
		return true;
	}
	hook_boot();
	/*
	 * Create enough scaffolding to allow recursive allocation in
	 * malloc_ncpus().
	 */
	narenas_auto = 1;
	manual_arena_base = narenas_auto + 1;
	memset(arenas, 0, sizeof(arena_t *) * narenas_auto);
	/*
	 * Initialize one arena here.  The rest are lazily created in
	 * arena_choose_hard().
	 */
	if (arena_init(TSDN_NULL, 0, (extent_hooks_t *)&extent_hooks_default)
	    == NULL) {
		return true;
	}
	a0 = arena_get(TSDN_NULL, 0, false);
	malloc_init_state = malloc_init_a0_initialized;

	return false;
}

static bool
malloc_init_hard_a0(void) {
	bool ret;

	malloc_mutex_lock(TSDN_NULL, &init_lock);
	ret = malloc_init_hard_a0_locked();
	malloc_mutex_unlock(TSDN_NULL, &init_lock);
	return ret;
}

/* Initialize data structures which may trigger recursive allocation. */
static bool
malloc_init_hard_recursible(void) {
	malloc_init_state = malloc_init_recursible;

	ncpus = malloc_ncpus();

#if (defined(JEMALLOC_HAVE_PTHREAD_ATFORK) && !defined(JEMALLOC_MUTEX_INIT_CB) \
    && !defined(JEMALLOC_ZONE) && !defined(_WIN32) && \
    !defined(__native_client__))
	/* LinuxThreads' pthread_atfork() allocates. */
	if (pthread_atfork(jemalloc_prefork, jemalloc_postfork_parent,
	    jemalloc_postfork_child) != 0) {
		malloc_write("<jemalloc>: Error in pthread_atfork()\n");
		if (opt_abort) {
			abort();
		}
		return true;
	}
#endif

	if (background_thread_boot0()) {
		return true;
	}

	return false;
}

static unsigned
malloc_narenas_default(void) {
	assert(ncpus > 0);
	/*
	 * For SMP systems, create more than one arena per CPU by
	 * default.
	 */
	if (ncpus > 1) {
		return ncpus << 2;
	} else {
		return 1;
	}
}

static percpu_arena_mode_t
percpu_arena_as_initialized(percpu_arena_mode_t mode) {
	assert(!malloc_initialized());
	assert(mode <= percpu_arena_disabled);

	if (mode != percpu_arena_disabled) {
		mode += percpu_arena_mode_enabled_base;
	}

	return mode;
}

static bool
malloc_init_narenas(void) {
	assert(ncpus > 0);

	if (opt_percpu_arena != percpu_arena_disabled) {
		if (!have_percpu_arena || malloc_getcpu() < 0) {
			opt_percpu_arena = percpu_arena_disabled;
			malloc_printf("<jemalloc>: perCPU arena getcpu() not "
			    "available. Setting narenas to %u.\n", opt_narenas ?
			    opt_narenas : malloc_narenas_default());
			if (opt_abort) {
				abort();
			}
		} else {
			if (ncpus >= MALLOCX_ARENA_LIMIT) {
				malloc_printf("<jemalloc>: narenas w/ percpu"
				    "arena beyond limit (%d)\n", ncpus);
				if (opt_abort) {
					abort();
				}
				return true;
			}
			/* NB: opt_percpu_arena isn't fully initialized yet. */
			if (percpu_arena_as_initialized(opt_percpu_arena) ==
			    per_phycpu_arena && ncpus % 2 != 0) {
				malloc_printf("<jemalloc>: invalid "
				    "configuration -- per physical CPU arena "
				    "with odd number (%u) of CPUs (no hyper "
				    "threading?).\n", ncpus);
				if (opt_abort)
					abort();
			}
			unsigned n = percpu_arena_ind_limit(
			    percpu_arena_as_initialized(opt_percpu_arena));
			if (opt_narenas < n) {
				/*
				 * If narenas is specified with percpu_arena
				 * enabled, actual narenas is set as the greater
				 * of the two. percpu_arena_choose will be free
				 * to use any of the arenas based on CPU
				 * id. This is conservative (at a small cost)
				 * but ensures correctness.
				 *
				 * If for some reason the ncpus determined at
				 * boot is not the actual number (e.g. because
				 * of affinity setting from numactl), reserving
				 * narenas this way provides a workaround for
				 * percpu_arena.
				 */
				opt_narenas = n;
			}
		}
	}
	if (opt_narenas == 0) {
		opt_narenas = malloc_narenas_default();
	}
	assert(opt_narenas > 0);

	narenas_auto = opt_narenas;
	/*
	 * Limit the number of arenas to the indexing range of MALLOCX_ARENA().
	 */
	if (narenas_auto >= MALLOCX_ARENA_LIMIT) {
		narenas_auto = MALLOCX_ARENA_LIMIT - 1;
		malloc_printf("<jemalloc>: Reducing narenas to limit (%d)\n",
		    narenas_auto);
	}
	narenas_total_set(narenas_auto);
	if (arena_init_huge()) {
		narenas_total_inc();
	}
	manual_arena_base = narenas_total_get();

	return false;
}

static void
malloc_init_percpu(void) {
	opt_percpu_arena = percpu_arena_as_initialized(opt_percpu_arena);
}

static bool
malloc_init_hard_finish(void) {
	if (malloc_mutex_boot()) {
		return true;
	}

	malloc_init_state = malloc_init_initialized;
	malloc_slow_flag_init();

	return false;
}

static void
malloc_init_hard_cleanup(tsdn_t *tsdn, bool reentrancy_set) {
	malloc_mutex_assert_owner(tsdn, &init_lock);
	malloc_mutex_unlock(tsdn, &init_lock);
	if (reentrancy_set) {
		assert(!tsdn_null(tsdn));
		tsd_t *tsd = tsdn_tsd(tsdn);
		assert(tsd_reentrancy_level_get(tsd) > 0);
		post_reentrancy(tsd);
	}
}

static bool
malloc_init_hard(void) {
	tsd_t *tsd;

#if defined(_WIN32) && _WIN32_WINNT < 0x0600
	_init_init_lock();
#endif
	malloc_mutex_lock(TSDN_NULL, &init_lock);

#define UNLOCK_RETURN(tsdn, ret, reentrancy)		\
	malloc_init_hard_cleanup(tsdn, reentrancy);	\
	return ret;

	if (!malloc_init_hard_needed()) {
		UNLOCK_RETURN(TSDN_NULL, false, false)
	}

	if (malloc_init_state != malloc_init_a0_initialized &&
	    malloc_init_hard_a0_locked()) {
		UNLOCK_RETURN(TSDN_NULL, true, false)
	}

	malloc_mutex_unlock(TSDN_NULL, &init_lock);
	/* Recursive allocation relies on functional tsd. */
	tsd = malloc_tsd_boot0();
	if (tsd == NULL) {
		return true;
	}
	if (malloc_init_hard_recursible()) {
		return true;
	}

	malloc_mutex_lock(tsd_tsdn(tsd), &init_lock);
	/* Set reentrancy level to 1 during init. */
	pre_reentrancy(tsd, NULL);
	/* Initialize narenas before prof_boot2 (for allocation). */
	if (malloc_init_narenas() || background_thread_boot1(tsd_tsdn(tsd))) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}
	if (config_prof && prof_boot2(tsd)) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}

	malloc_init_percpu();

	if (malloc_init_hard_finish()) {
		UNLOCK_RETURN(tsd_tsdn(tsd), true, true)
	}
	post_reentrancy(tsd);
	malloc_mutex_unlock(tsd_tsdn(tsd), &init_lock);

	witness_assert_lockless(witness_tsd_tsdn(
	    tsd_witness_tsdp_get_unsafe(tsd)));
	malloc_tsd_boot1();
	/* Update TSD after tsd_boot1. */
	tsd = tsd_fetch();
	if (opt_background_thread) {
		assert(have_background_thread);
		/*
		 * Need to finish init & unlock first before creating background
		 * threads (pthread_create depends on malloc).  ctl_init (which
		 * sets isthreaded) needs to be called without holding any lock.
		 */
		background_thread_ctl_init(tsd_tsdn(tsd));
		if (background_thread_create(tsd, 0)) {
			return true;
		}
	}
#undef UNLOCK_RETURN
	return false;
}

/*
 * End initialization functions.
 */
/******************************************************************************/
/*
 * Begin allocation-path internal functions and data structures.
 */

/*
 * Settings determined by the documented behavior of the allocation functions.
 */
typedef struct static_opts_s static_opts_t;
struct static_opts_s {
	/* Whether or not allocation size may overflow. */
	bool may_overflow;

	/*
	 * Whether or not allocations (with alignment) of size 0 should be
	 * treated as size 1.
	 */
	bool bump_empty_aligned_alloc;
	/*
	 * Whether to assert that allocations are not of size 0 (after any
	 * bumping).
	 */
	bool assert_nonempty_alloc;

	/*
	 * Whether or not to modify the 'result' argument to malloc in case of
	 * error.
	 */
	bool null_out_result_on_error;
	/* Whether to set errno when we encounter an error condition. */
	bool set_errno_on_error;

	/*
	 * The minimum valid alignment for functions requesting aligned storage.
	 */
	size_t min_alignment;

	/* The error string to use if we oom. */
	const char *oom_string;
	/* The error string to use if the passed-in alignment is invalid. */
	const char *invalid_alignment_string;

	/*
	 * False if we're configured to skip some time-consuming operations.
	 *
	 * This isn't really a malloc "behavior", but it acts as a useful
	 * summary of several other static (or at least, static after program
	 * initialization) options.
	 */
	bool slow;
	/*
	 * Return size.
	 */
	bool usize;
};

JEMALLOC_ALWAYS_INLINE void
static_opts_init(static_opts_t *static_opts) {
	static_opts->may_overflow = false;
	static_opts->bump_empty_aligned_alloc = false;
	static_opts->assert_nonempty_alloc = false;
	static_opts->null_out_result_on_error = false;
	static_opts->set_errno_on_error = false;
	static_opts->min_alignment = 0;
	static_opts->oom_string = "";
	static_opts->invalid_alignment_string = "";
	static_opts->slow = false;
	static_opts->usize = false;
}

/*
 * These correspond to the macros in jemalloc/jemalloc_macros.h.  Broadly, we
 * should have one constant here per magic value there.  Note however that the
 * representations need not be related.
 */
#define TCACHE_IND_NONE ((unsigned)-1)
#define TCACHE_IND_AUTOMATIC ((unsigned)-2)
#define ARENA_IND_AUTOMATIC ((unsigned)-1)

typedef struct dynamic_opts_s dynamic_opts_t;
struct dynamic_opts_s {
	void **result;
	size_t usize;
	size_t num_items;
	size_t item_size;
	size_t alignment;
	bool zero;
	unsigned tcache_ind;
	unsigned arena_ind;
};

JEMALLOC_ALWAYS_INLINE void
dynamic_opts_init(dynamic_opts_t *dynamic_opts) {
	dynamic_opts->result = NULL;
	dynamic_opts->usize = 0;
	dynamic_opts->num_items = 0;
	dynamic_opts->item_size = 0;
	dynamic_opts->alignment = 0;
	dynamic_opts->zero = false;
	dynamic_opts->tcache_ind = TCACHE_IND_AUTOMATIC;
	dynamic_opts->arena_ind = ARENA_IND_AUTOMATIC;
}

/* ind is ignored if dopts->alignment > 0. */
JEMALLOC_ALWAYS_INLINE void *
imalloc_no_sample(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd,
    size_t size, size_t usize, szind_t ind) {
	tcache_t *tcache;
	arena_t *arena;

	/* Fill in the tcache. */
	if (dopts->tcache_ind == TCACHE_IND_AUTOMATIC) {
		if (likely(!sopts->slow)) {
			/* Getting tcache ptr unconditionally. */
			tcache = tsd_tcachep_get(tsd);
			assert(tcache == tcache_get(tsd));
		} else {
			tcache = tcache_get(tsd);
		}
	} else if (dopts->tcache_ind == TCACHE_IND_NONE) {
		tcache = NULL;
	} else {
		tcache = tcaches_get(tsd, dopts->tcache_ind);
	}

	/* Fill in the arena. */
	if (dopts->arena_ind == ARENA_IND_AUTOMATIC) {
		/*
		 * In case of automatic arena management, we defer arena
		 * computation until as late as we can, hoping to fill the
		 * allocation out of the tcache.
		 */
		arena = NULL;
	} else {
		arena = arena_get(tsd_tsdn(tsd), dopts->arena_ind, true);
	}

	if (unlikely(dopts->alignment != 0)) {
		return ipalloct(tsd_tsdn(tsd), usize, dopts->alignment,
		    dopts->zero, tcache, arena);
	}

	return iallocztm(tsd_tsdn(tsd), size, ind, dopts->zero, tcache, false,
	    arena, sopts->slow);
}

JEMALLOC_ALWAYS_INLINE void *
imalloc_sample(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd,
    size_t usize, szind_t ind) {
	void *ret;

	/*
	 * For small allocations, sampling bumps the usize.  If so, we allocate
	 * from the ind_large bucket.
	 */
	szind_t ind_large;
	size_t bumped_usize = usize;

	if (usize <= SC_SMALL_MAXCLASS) {
		assert(((dopts->alignment == 0) ?
		    sz_s2u(SC_LARGE_MINCLASS) :
		    sz_sa2u(SC_LARGE_MINCLASS, dopts->alignment))
			== SC_LARGE_MINCLASS);
		ind_large = sz_size2index(SC_LARGE_MINCLASS);
		bumped_usize = sz_s2u(SC_LARGE_MINCLASS);
		ret = imalloc_no_sample(sopts, dopts, tsd, bumped_usize,
		    bumped_usize, ind_large);
		if (unlikely(ret == NULL)) {
			return NULL;
		}
		arena_prof_promote(tsd_tsdn(tsd), ret, usize);
	} else {
		ret = imalloc_no_sample(sopts, dopts, tsd, usize, usize, ind);
	}

	return ret;
}

/*
 * Returns true if the allocation will overflow, and false otherwise.  Sets
 * *size to the product either way.
 */
JEMALLOC_ALWAYS_INLINE bool
compute_size_with_overflow(bool may_overflow, dynamic_opts_t *dopts,
    size_t *size) {
	/*
	 * This function is just num_items * item_size, except that we may have
	 * to check for overflow.
	 */

	if (!may_overflow) {
		assert(dopts->num_items == 1);
		*size = dopts->item_size;
		return false;
	}

	/* A size_t with its high-half bits all set to 1. */
	static const size_t high_bits = SIZE_T_MAX << (sizeof(size_t) * 8 / 2);

	*size = dopts->item_size * dopts->num_items;

	if (unlikely(*size == 0)) {
		return (dopts->num_items != 0 && dopts->item_size != 0);
	}

	/*
	 * We got a non-zero size, but we don't know if we overflowed to get
	 * there.  To avoid having to do a divide, we'll be clever and note that
	 * if both A and B can be represented in N/2 bits, then their product
	 * can be represented in N bits (without the possibility of overflow).
	 */
	if (likely((high_bits & (dopts->num_items | dopts->item_size)) == 0)) {
		return false;
	}
	if (likely(*size / dopts->item_size == dopts->num_items)) {
		return false;
	}
	return true;
}

JEMALLOC_ALWAYS_INLINE int
imalloc_body(static_opts_t *sopts, dynamic_opts_t *dopts, tsd_t *tsd) {
	/* Where the actual allocated memory will live. */
	void *allocation = NULL;
	/* Filled in by compute_size_with_overflow below. */
	size_t size = 0;
	/*
	 * For unaligned allocations, we need only ind.  For aligned
	 * allocations, or in case of stats or profiling we need usize.
	 *
	 * These are actually dead stores, in that their values are reset before
	 * any branch on their value is taken.  Sometimes though, it's
	 * convenient to pass them as arguments before this point.  To avoid
	 * undefined behavior then, we initialize them with dummy stores.
	 */
	szind_t ind = 0;
	size_t usize = 0;

	/* Reentrancy is only checked on slow path. */
	int8_t reentrancy_level;

	/* Compute the amount of memory the user wants. */
	if (unlikely(compute_size_with_overflow(sopts->may_overflow, dopts,
	    &size))) {
		goto label_oom;
	}

	if (unlikely(dopts->alignment < sopts->min_alignment
	    || (dopts->alignment & (dopts->alignment - 1)) != 0)) {
		goto label_invalid_alignment;
	}

	/* This is the beginning of the "core" algorithm. */

	if (dopts->alignment == 0) {
		ind = sz_size2index(size);
		if (unlikely(ind >= SC_NSIZES)) {
			goto label_oom;
		}
		if (config_stats || (config_prof && opt_prof) || sopts->usize) {
			usize = sz_index2size(ind);
			dopts->usize = usize;
			assert(usize > 0 && usize
			    <= SC_LARGE_MAXCLASS);
		}
	} else {
		if (sopts->bump_empty_aligned_alloc) {
			if (unlikely(size == 0)) {
				size = 1;
			}
		}
		usize = sz_sa2u(size, dopts->alignment);
		dopts->usize = usize;
		if (unlikely(usize == 0
		    || usize > SC_LARGE_MAXCLASS)) {
			goto label_oom;
		}
	}
	/* Validate the user input. */
	if (sopts->assert_nonempty_alloc) {
		assert (size != 0);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	/*
	 * If we need to handle reentrancy, we can do it out of a
	 * known-initialized arena (i.e. arena 0).
	 */
	reentrancy_level = tsd_reentrancy_level_get(tsd);
	if (sopts->slow && unlikely(reentrancy_level > 0)) {
		/*
		 * We should never specify particular arenas or tcaches from
		 * within our internal allocations.
		 */
		assert(dopts->tcache_ind == TCACHE_IND_AUTOMATIC ||
		    dopts->tcache_ind == TCACHE_IND_NONE);
		assert(dopts->arena_ind == ARENA_IND_AUTOMATIC);
		dopts->tcache_ind = TCACHE_IND_NONE;
		/* We know that arena 0 has already been initialized. */
		dopts->arena_ind = 0;
	}

	/* If profiling is on, get our profiling context. */
	if (config_prof && opt_prof) {
		/*
		 * Note that if we're going down this path, usize must have been
		 * initialized in the previous if statement.
		 */
		prof_tctx_t *tctx = prof_alloc_prep(
		    tsd, usize, prof_active_get_unlocked(), true);

		alloc_ctx_t alloc_ctx;
		if (likely((uintptr_t)tctx == (uintptr_t)1U)) {
			alloc_ctx.slab = (usize
			    <= SC_SMALL_MAXCLASS);
			allocation = imalloc_no_sample(
			    sopts, dopts, tsd, usize, usize, ind);
		} else if ((uintptr_t)tctx > (uintptr_t)1U) {
			/*
			 * Note that ind might still be 0 here.  This is fine;
			 * imalloc_sample ignores ind if dopts->alignment > 0.
			 */
			allocation = imalloc_sample(
			    sopts, dopts, tsd, usize, ind);
			alloc_ctx.slab = false;
		} else {
			allocation = NULL;
		}

		if (unlikely(allocation == NULL)) {
			prof_alloc_rollback(tsd, tctx, true);
			goto label_oom;
		}
		prof_malloc(tsd_tsdn(tsd), allocation, usize, &alloc_ctx, tctx);
	} else {
		/*
		 * If dopts->alignment > 0, then ind is still 0, but usize was
		 * computed in the previous if statement.  Down the positive
		 * alignment path, imalloc_no_sample ignores ind and size
		 * (relying only on usize).
		 */
		allocation = imalloc_no_sample(sopts, dopts, tsd, size, usize,
		    ind);
		if (unlikely(allocation == NULL)) {
			goto label_oom;
		}
	}

	/*
	 * Allocation has been done at this point.  We still have some
	 * post-allocation work to do though.
	 */
	assert(dopts->alignment == 0
	    || ((uintptr_t)allocation & (dopts->alignment - 1)) == ZU(0));

	if (config_stats) {
		assert(usize == isalloc(tsd_tsdn(tsd), allocation));
		*tsd_thread_allocatedp_get(tsd) += usize;
	}

	if (sopts->slow) {
		UTRACE(0, size, allocation);
	}

	/* Success! */
	check_entry_exit_locking(tsd_tsdn(tsd));
	*dopts->result = allocation;
	return 0;

label_oom:
	if (unlikely(sopts->slow) && config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write(sopts->oom_string);
		abort();
	}

	if (sopts->slow) {
		UTRACE(NULL, size, NULL);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	if (sopts->set_errno_on_error) {
		set_errno(ENOMEM);
	}

	if (sopts->null_out_result_on_error) {
		*dopts->result = NULL;
	}

	return ENOMEM;

	/*
	 * This label is only jumped to by one goto; we move it out of line
	 * anyways to avoid obscuring the non-error paths, and for symmetry with
	 * the oom case.
	 */
label_invalid_alignment:
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write(sopts->invalid_alignment_string);
		abort();
	}

	if (sopts->set_errno_on_error) {
		set_errno(EINVAL);
	}

	if (sopts->slow) {
		UTRACE(NULL, size, NULL);
	}

	check_entry_exit_locking(tsd_tsdn(tsd));

	if (sopts->null_out_result_on_error) {
		*dopts->result = NULL;
	}

	return EINVAL;
}

JEMALLOC_ALWAYS_INLINE bool
imalloc_init_check(static_opts_t *sopts, dynamic_opts_t *dopts) {
	if (unlikely(!malloc_initialized()) && unlikely(malloc_init())) {
		if (config_xmalloc && unlikely(opt_xmalloc)) {
			malloc_write(sopts->oom_string);
			abort();
		}
		UTRACE(NULL, dopts->num_items * dopts->item_size, NULL);
		set_errno(ENOMEM);
		*dopts->result = NULL;

		return false;
	}

	return true;
}

/* Returns the errno-style error code of the allocation. */
JEMALLOC_ALWAYS_INLINE int
imalloc(static_opts_t *sopts, dynamic_opts_t *dopts) {
	if (tsd_get_allocates() && !imalloc_init_check(sopts, dopts)) {
		return ENOMEM;
	}

	/* We always need the tsd.  Let's grab it right away. */
	tsd_t *tsd = tsd_fetch();
	assert(tsd);
	if (likely(tsd_fast(tsd))) {
		/* Fast and common path. */
		tsd_assert_fast(tsd);
		sopts->slow = false;
		return imalloc_body(sopts, dopts, tsd);
	} else {
		if (!tsd_get_allocates() && !imalloc_init_check(sopts, dopts)) {
			return ENOMEM;
		}

		sopts->slow = true;
		return imalloc_body(sopts, dopts, tsd);
	}
}

JEMALLOC_NOINLINE
void *
malloc_default(size_t size) {
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.malloc.entry", "size: %zu", size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in malloc(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;

	imalloc(&sopts, &dopts);
	/*
	 * Note that this branch gets optimized away -- it immediately follows
	 * the check on tsd_fast that sets sopts.slow.
	 */
	if (sopts.slow) {
		uintptr_t args[3] = {size};
		hook_invoke_alloc(hook_alloc_malloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.malloc.exit", "result: %p", ret);

	return ret;
}

/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

/*
 * malloc() fastpath.
 *
 * Fastpath assumes size <= SC_LOOKUP_MAXCLASS, and that we hit
 * tcache.  If either of these is false, we tail-call to the slowpath,
 * malloc_default().  Tail-calling is used to avoid any caller-saved
 * registers.
 *
 * fastpath supports ticker and profiling, both of which will also
 * tail-call to the slowpath if they fire.
 */
static JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
_je_malloc(size_t size) {
	LOG("core.malloc.entry", "size: %zu", size);

	if (tsd_get_allocates() && unlikely(!malloc_initialized())) {
		return malloc_default(size);
	}

	tsd_t *tsd = tsd_get(false);
	if (unlikely(!tsd || !tsd_fast(tsd) || (size > SC_LOOKUP_MAXCLASS))) {
		return malloc_default(size);
	}

	tcache_t *tcache = tsd_tcachep_get(tsd);

	if (unlikely(ticker_trytick(&tcache->gc_ticker))) {
		return malloc_default(size);
	}

	szind_t ind = sz_size2index_lookup(size);
	size_t usize;
	if (config_stats || config_prof) {
		usize = sz_index2size(ind);
	}
	/* Fast path relies on size being a bin. I.e. SC_LOOKUP_MAXCLASS < SC_SMALL_MAXCLASS */
	assert(ind < SC_NBINS);
	assert(size <= SC_SMALL_MAXCLASS);

	if (config_prof) {
		int64_t bytes_until_sample = tsd_bytes_until_sample_get(tsd);
		bytes_until_sample -= usize;
		tsd_bytes_until_sample_set(tsd, bytes_until_sample);

		if (unlikely(bytes_until_sample < 0)) {
			/*
			 * Avoid a prof_active check on the fastpath.
			 * If prof_active is false, set bytes_until_sample to
			 * a large value.  If prof_active is set to true,
			 * bytes_until_sample will be reset.
			 */
			if (!prof_active) {
				tsd_bytes_until_sample_set(tsd, SSIZE_MAX);
			}
			return malloc_default(size);
		}
	}

	cache_bin_t *bin = tcache_small_bin_get(tcache, ind);
	bool tcache_success;
	void* ret = cache_bin_alloc_easy(bin, &tcache_success);

	if (tcache_success) {
		if (config_stats) {
			*tsd_thread_allocatedp_get(tsd) += usize;
			bin->tstats.nrequests++;
		}
		if (config_prof) {
			tcache->prof_accumbytes += usize;
		}

		LOG("core.malloc.exit", "result: %p", ret);

		/* Fastpath success */
		return ret;
	}

	return malloc_default(size);
}

JEMALLOC_EXPORT
void debug_break(void* v)
{
	asm volatile("int3");
}

#include <elf.h>
#include <sys/stat.h>
#define PATH_SZ 256

#if 0
static size_t
getDataSecInfo(unsigned long long *Start, unsigned long long *End)
{
	char Exec[PATH_SZ];
	static size_t DsecSz = 0;

	if (DsecSz != 0)
	{
		return DsecSz;
	}
	DsecSz = -1;

	ssize_t Count = readlink( "/proc/self/exe", Exec, PATH_SZ);

	if (Count == -1) {
		return -1;
	}
	Exec[Count] = '\0';

	//malloc_printf("Exec: %s\n", Exec);

	int fd = open(Exec, O_RDONLY);
	if (fd == -1) {
		return -1;
	}

	struct stat Statbuf;
	fstat(fd, &Statbuf);

	char *Base = mmap(NULL, Statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (Base == NULL) {
		close(fd);
		return -1;
	}

	Elf64_Ehdr *Header = (Elf64_Ehdr*)Base;

	if (Header->e_ident[0] != 0x7f
		|| Header->e_ident[1] != 'E'
		|| Header->e_ident[2] != 'L'
		|| Header->e_ident[3] != 'F')
	{
		goto out;
	}

	int i;
	Elf64_Shdr *Shdr = (Elf64_Shdr*)(Base + Header->e_shoff);
	char *Strtab = Base + Shdr[Header->e_shstrndx].sh_offset;

	for (i = 0; i < Header->e_shnum; i++)
	{
		char *Name = Strtab + Shdr[i].sh_name;
		if (!strncmp(Name, ".data", 6))
		{
			*Start = (unsigned long long)Shdr[i].sh_addr;
			*End = (unsigned long long)Shdr[i].sh_addr + Shdr[i].sh_size;
		}
	}

out:
	munmap(Base, Statbuf.st_size);
	close(fd);
	return 0;
}
#endif

static int num_global_variables = 0;
static int global_map_size = 0;
static struct obj_header **global_objects;
#define MIN_NEW_SLOTS 16

static void add_global_object(struct obj_header *obj) {
	if (num_global_variables == global_map_size) {
		global_map_size += (num_global_variables < MIN_NEW_SLOTS) ? MIN_NEW_SLOTS : num_global_variables;
		struct obj_header **new_map =
			(struct obj_header**)je_malloc(global_map_size * sizeof(struct obj_header*));
		assert(new_map);
		if (num_global_variables) {
			memcpy(new_map, global_objects, sizeof(struct obj_header*) * num_global_variables);
			free(global_objects);
		}
		global_objects = new_map;
	}
	global_objects[num_global_variables++] = obj;
	//if (obj->size >= MAX_OFFSET) {
		//malloc_printf("adding:%p %d %x\n", obj, obj->size, obj->magic);
	//}
}

static void initialize_globals(struct obj_header *start, struct obj_header *end) {
	struct obj_header *header;
	//malloc_printf("START:%p END:%p\n", start, end);
	for (header = start; header < end; ) {
		//malloc_printf("walking header:%p\n", header);
		if (header->magic == MAGIC_NUMBER) {
			add_global_object(header);
			header = (struct obj_header*)(((char*)header) + header->size + OBJ_HEADER_SIZE);
			continue;
		}
		header = (struct obj_header*)(((char*)header)+1);
	}
}

#define GLOBAL_CACHE_SIZE 4
extern void *GlobalCache[GLOBAL_CACHE_SIZE];
static struct obj_header CacheObj = {MAGIC_NUMBER, 0, 0, 0};

static void addToGlobalCache(void *ptr) {
	int i;
	for (i = GLOBAL_CACHE_SIZE-1; i > 0; i--) {
		GlobalCache[i] = GlobalCache[i-1];
	}
	GlobalCache[0] = ptr;
}

struct Record {
	unsigned long long Addr;
	unsigned long long TargetAddr;
	//unsigned char SBReg;
	unsigned char BaseReg;
	unsigned char len;
	int Offset;
	int BaseOffset;
	int size;
	//unsigned char data[6];
};

static char *stackmap = NULL;
static size_t stackmap_size = 0;
static char *textmap = NULL;
static size_t textmap_size = 0;
static size_t textmap_start = 0;

static int PF_NumRecords = 0;
static int PF_MaxRecords = 0;
static struct Record *PF_Recs = NULL;

static void CreateSpaceForRecords(int NumRecords) {
	struct Record *Tmp;
	PF_MaxRecords += NumRecords;
	Tmp = malloc(sizeof(struct Record) * PF_MaxRecords);
	memcpy(Tmp, PF_Recs, sizeof(struct Record) * PF_NumRecords);
	PF_Recs = Tmp;
}

static void AddRecords(unsigned long long Addr, unsigned char SBReg, 
	unsigned char BaseReg, unsigned char len, int Offset, int BaseOffset, int size) {
	//malloc_printf("AddRecords:: Addr:%llx SBReg:%d BaseReg:%d\n", Addr, SBReg, BaseReg);

	if (PF_NumRecords == PF_MaxRecords) {
		CreateSpaceForRecords(64);
	}
	PF_Recs[PF_NumRecords].Addr = Addr;
	PF_Recs[PF_NumRecords].TargetAddr = 0;
	//PF_Recs[PF_NumRecords].SBReg = SBReg;
	PF_Recs[PF_NumRecords].BaseReg = BaseReg;
	PF_Recs[PF_NumRecords].len = len;
	PF_Recs[PF_NumRecords].Offset = Offset;
	PF_Recs[PF_NumRecords].BaseOffset = BaseOffset;
	PF_Recs[PF_NumRecords].size = size;
	/*assert(len <= 6);
	size_t textoffset = Addr - textmap_start;
	memcpy(PF_Recs[PF_NumRecords].data, &textmap[textoffset], len);*/
	PF_NumRecords++;
}

static struct Record* SearchRecord(unsigned long long Addr)
{
	int i;
	for (i = 0; i < PF_NumRecords; i++) {
		if (i + 100 < PF_NumRecords && PF_Recs[i + 100].Addr < Addr) {
			i += 100;
		}
		if (PF_Recs[i].Addr == Addr) {
			return &PF_Recs[i];
		}
	}
	return NULL;
}


struct Header {
  unsigned char version;       //: Stack Map Version (current version is 3)
  unsigned char Reserved1; //  : Reserved (expected to be 0)
  unsigned short Reserved2; //: Reserved (expected to be 0)
	unsigned NumFunctions;
	unsigned NumConstants;
	unsigned NumRecords;
};

struct FuncInfo { // Num Functions
  unsigned long long FunAddr;
  unsigned long long StackSize;
  unsigned long long RecordCount;
};

// Num Constants
//unsigned long long LargeConstant;

// NumRecords {

struct __attribute__((__packed__)) LocMetadata {
  unsigned long long PatchPointID;
  unsigned InstructionOffset;
  unsigned LastOffset;
  unsigned short Reserved;
  unsigned short NumLocations;
};

struct LocData { // NumLocations
	unsigned char  Type; //: Register | Direct | Indirect | Constant | ConstantIndex
  unsigned char  Reserved1;
  unsigned short LocationSize;
  unsigned short BaseRegister;
  unsigned short Reserved2;
  int  Offset;
};

//check alignment 8

struct LiveMetadata {
	unsigned short padding;
  unsigned short NumLiveOuts;
};

struct LiveData {
	unsigned short RegNum;
	unsigned char Reserved;
	unsigned char Size;
};
//check alignment 8
// } // NumRecords


struct LocMetadata* PrintRecord(unsigned long long FunAddr, struct LocMetadata *l)
{
	assert(l->NumLocations == 2);
	unsigned long long CurAddr = FunAddr + l->InstructionOffset;
	unsigned len = l->LastOffset - l->InstructionOffset;
	struct LocData *ld = (struct LocData*)(l + 1);

	int SBReg = ld[0].BaseRegister;
	int BaseOffset = ld[0].Offset;
	int BaseReg = ld[1].BaseRegister;
	int Disp = ld[1].Offset;
	int Size = ld[1].LocationSize;

	AddRecords(CurAddr, SBReg, BaseReg, len, Disp, BaseOffset, Size);
	struct LiveMetadata *live = (struct LiveMetadata*)(((size_t)(ld+2) + 7) & ~(size_t)7ULL);
	assert(live->NumLiveOuts == 0);
	struct LocMetadata *Ret = (struct LocMetadata*)(((size_t)(live+1) + 7) & ~(size_t)7ULL);
	return Ret;
}


static void print_stackmap(char *Stackmap) {
	struct Header *h = (struct Header*)Stackmap;
	int NumFunctions = h->NumFunctions;
	int NumConstants = h->NumConstants;
	int NumRecords = h->NumRecords;
	struct FuncInfo *f = (struct FuncInfo*)(h+1);
	int j, k;

	CreateSpaceForRecords(NumRecords);

	struct LocMetadata *l = (struct LocMetadata*)(((char*)&f[NumFunctions]) + NumConstants * sizeof(unsigned long long));

	k = 0;
	while (k != NumFunctions) {
		for (j = 0; j < (int)f[k].RecordCount; j++) {
			l = PrintRecord(f[k].FunAddr, l);
		}
		k++;
	}
}

static void
initialize_stackmap()
{
	char Exec[PATH_SZ];
	ssize_t Count = readlink( "/proc/self/exe", Exec, PATH_SZ);
	if (Count == -1) {
		return;
	}
	Exec[Count] = '\0';

	int fd = open(Exec, O_RDONLY);
	if (fd == -1) {
		return;
	}

	struct stat Statbuf;
	fstat(fd, &Statbuf);

	char *Base = mmap(NULL, Statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (Base == NULL) {
		close(fd);
		return;
	}

	Elf64_Ehdr *Header = (Elf64_Ehdr*)Base;

	if (Header->e_ident[0] != 0x7f
		|| Header->e_ident[1] != 'E'
		|| Header->e_ident[2] != 'L'
		|| Header->e_ident[3] != 'F')
	{
		goto out;
	}

	Elf64_Shdr *Shdr = (Elf64_Shdr*)(Base + Header->e_shoff);
	char *Strtab = Base + Shdr[Header->e_shstrndx].sh_offset;
	int i;

	for (i = 0; i < Header->e_shnum; i++)
	{
		char *Name = Strtab + Shdr[i].sh_name;
		if (!strncmp(Name, ".text", 6))
		{
			char *start = Base + Shdr[i].sh_offset;
			size_t size = Shdr[i].sh_size;
			textmap = start;
			textmap_size = size;
			textmap_start = Shdr[i].sh_addr;
			break;
		}
	}

	for (i = 0; i < Header->e_shnum; i++)
	{
		char *Name = Strtab + Shdr[i].sh_name;
		if (!strncmp(Name, ".llvm_stackmaps", 15)) {
			char *start = Base + Shdr[i].sh_offset;
			size_t size = Shdr[i].sh_size;
			stackmap = start;
			stackmap_size = size;
			print_stackmap(stackmap);
		}
	}

out:
	munmap(Base, Statbuf.st_size);
	close(fd);
}


static int RegIDToGregsID(int reg) {
	switch (reg) {
		case 1: return REG_RAX;
		case 2: return REG_RBP; 
		case 3: return REG_RBX;
		case 4: return REG_RCX;
		case 5: return REG_RDI;
		case 6: return REG_RDX;
		case 9: return REG_RSI;
		case 11: return REG_R8; 
		case 12: return REG_R9;
		case 13: return REG_R10; 
		case 14: return REG_R11;
		case 15: return REG_R12;
		case 16: return REG_R13;
		case 17: return REG_R14;
		case 18: return REG_R15;
	}
	return -1;
}

static void CheckValidAccess(unsigned long long StartAddr, unsigned long long FaultAddr, int size)
{
	struct obj_header *head = (struct obj_header*)get_fast_base(StartAddr);
	assert(head);
	if (head == NULL) {
		return;
	}
	unsigned long long Base = (unsigned long long)(head+1);
	unsigned long long Limit = Base + head->size;
	//malloc_printf("limit:%llx Base:%llx\n", Limit, Base);
	if (FaultAddr < (unsigned long long)(head+1)  || (FaultAddr + size) > Limit) {
		//malloc_printf("access violation!\n");
		abort3("access violation");
	}
	//malloc_printf("no access violation!\n");
}

static unsigned long long EmitStub(unsigned char *CurEIP, int len, int BaseReg)
{
	static unsigned char *StubCurAddr = NULL;
	static unsigned char *StubEndAddr = NULL;
	if (StubEndAddr - StubCurAddr < 16) {
		StubCurAddr = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
		assert(StubCurAddr);
	}
	else {
		mprotect((void*)((size_t)StubCurAddr & ~4095ULL), 4096, PROT_READ|PROT_WRITE);
	}
	unsigned long long Ret = (unsigned long long)StubCurAddr;
	memcpy(StubCurAddr, CurEIP, len);
	StubCurAddr += len;
	if (BaseReg >= REG_R8 && BaseReg <= REG_R15) {
		StubCurAddr[0] = 0x41;
		StubCurAddr++;
	}
	switch (BaseReg) {
		case REG_RAX: StubCurAddr[0] = 0x58; break;
		case REG_RCX: StubCurAddr[0] = 0x59; break;
		case REG_RDX: StubCurAddr[0] = 0x5a; break;
		case REG_RBX: StubCurAddr[0] = 0x5b; break;
		case REG_RBP: StubCurAddr[0] = 0x5d; break;
		case REG_RSI: StubCurAddr[0] = 0x5e; break;
		case REG_RDI: StubCurAddr[0] = 0x5f; break;
		case REG_R8: StubCurAddr[0] = 0x58;  break;
		case REG_R9: StubCurAddr[0] = 0x59;  break;
		case REG_R10: StubCurAddr[0] = 0x5a; break;
		case REG_R11: StubCurAddr[0] = 0x5b; break;
		case REG_R12: StubCurAddr[0] = 0x5c; break;
		case REG_R13: StubCurAddr[0] = 0x5d; break;
		case REG_R14: StubCurAddr[0] = 0x5e; break;
		case REG_R15: StubCurAddr[0] = 0x5f; break;
		default: assert(0);
	}
	StubCurAddr++;
	StubCurAddr[0] = 0xc3;
	StubCurAddr++;
	mprotect((void*)((size_t)StubCurAddr & ~4095ULL), 4096, PROT_READ|PROT_EXEC);
	return Ret;
}

static unsigned long long findBaseAddr(unsigned long long Addr, unsigned long long *gregs)
{
	int i;
	for (i = 1; i <= 18; i++) {
		int id = RegIDToGregsID(i);
		if (id && (gregs[id] >> 49)) {
			if (!((gregs[id] ^ Addr) << 16)) {
				return gregs[id];
			}
		}
	}
	return Addr;
}

static void SoftwareEmulate(unsigned long long CurEIP, unsigned long long *gregs, struct Record *Rec)
{
	int Reg = RegIDToGregsID(Rec->BaseReg);
	int len = Rec->len;
	unsigned long long RegAddr = gregs[Reg];
	unsigned long long *RSP = (unsigned long long*)gregs[REG_RSP];

	RSP -= 1;
	RSP[0] = (CurEIP + len);
	RSP -= 1;
	RSP[0] = RegAddr; 
	gregs[REG_RSP] = (unsigned long long)RSP;
	gregs[Reg] = (RegAddr << 16) >> 16;

	if (!Rec->TargetAddr) {
		Rec->TargetAddr = EmitStub((unsigned char*)CurEIP, len, Reg);
	}
	gregs[REG_RIP] = Rec->TargetAddr;
}

static void EmulateInstruction(unsigned long long CurEIP, unsigned long long *gregs)
{
	if (PF_NumRecords == 0) {
		initialize_stackmap(stackmap);
	}
	struct Record *Rec = SearchRecord(CurEIP);
	if (Rec == NULL) {
		assert(0);
		return;
	}
	int Reg = RegIDToGregsID(Rec->BaseReg);
	assert(Reg != -1);
	int Offset = Rec->Offset;
	int BaseOffset = Rec->BaseOffset;
	int Size = Rec->size;
	unsigned long long FaultAddr = gregs[Reg] + Offset;
	unsigned long long StartAddr = FaultAddr - BaseOffset;
	unsigned long long BaseStartAddr = findBaseAddr(StartAddr, gregs);
	FaultAddr = ((FaultAddr << 16) >> 16);
	CheckValidAccess(BaseStartAddr, FaultAddr, Size);
	SoftwareEmulate(CurEIP, gregs, Rec);
}

static void
initialize_sections()
{
	char Exec[PATH_SZ];
	ssize_t Count = readlink( "/proc/self/exe", Exec, PATH_SZ);

	MinGlobalAddr = (void*)0xFFFFFFFFFFFFULL;
	MaxGlobalAddr = NULL;
	LastCrashAddr1 = NULL;
	LastCrashAddr2 = NULL;
	int i;
	for (i = 0; i < GLOBAL_CACHE_SIZE; i++) {
		GlobalCache[i] = ((&CacheObj) + 1);
	}
	__text_start = NULL;
	__text_end = NULL;

	if (Count == -1) {
		return;
	}
	Exec[Count] = '\0';

	int fd = open(Exec, O_RDONLY);
	if (fd == -1) {
		return;
	}

	struct stat Statbuf;
	fstat(fd, &Statbuf);

	char *Base = mmap(NULL, Statbuf.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (Base == NULL) {
		close(fd);
		return;
	}

	Elf64_Ehdr *Header = (Elf64_Ehdr*)Base;

	if (Header->e_ident[0] != 0x7f
		|| Header->e_ident[1] != 'E'
		|| Header->e_ident[2] != 'L'
		|| Header->e_ident[3] != 'F')
	{
		goto out;
	}

	Elf64_Shdr *Shdr = (Elf64_Shdr*)(Base + Header->e_shoff);
	char *Strtab = Base + Shdr[Header->e_shstrndx].sh_offset;

	for (i = 0; i < Header->e_shnum; i++)
	{
		char *Name = Strtab + Shdr[i].sh_name;
		if (strncmp(Name, ".text", 6))
		{
			struct obj_header *start, *end;
			start = (struct obj_header*)Shdr[i].sh_addr;
			end = (struct obj_header*)(Shdr[i].sh_addr + Shdr[i].sh_size);
			if (start) {
				if ((void*)start < MinGlobalAddr) {
					MinGlobalAddr = start;
				}
				if ((void*)end > MaxGlobalAddr) {
					MaxGlobalAddr = end;
				}
				initialize_globals(start, end);
			}
		}
		else {
			__text_start = (void*)Shdr[i].sh_addr;
			__text_end = (void*)(Shdr[i].sh_addr + Shdr[i].sh_size);
		}
	}

out:
	munmap(Base, Statbuf.st_size);
	close(fd);
}

static struct obj_header* find_smaller_or_equal(struct obj_header *e) {

	int lo = 0;
	int hi = num_global_variables - 1;
	struct obj_header *v;

  do {
    int mid = (lo + hi) / 2;
		v = global_objects[mid];

    if (v == e) {
      return e;
    }
    else if (e < v) {
      hi = mid - 1;
    }
    else {
      lo = mid + 1;
    }
  }
  while (lo <= hi);

	if (v < e) {
		return v;
	}
	else if (hi > 0) {
		v = global_objects[hi];
		if (v < e) {
			return v;
		}
	}
	return NULL;
}


static void *get_global_header(char *ptr) {
	struct obj_header *ret = find_smaller_or_equal((struct obj_header*)ptr);
	if (ret && ptr < ((char*)ret) + ret->size + OBJ_HEADER_SIZE) {
		return ret;
	}
	//malloc_printf("unable to find base corresponding to ptr:%p\n", ptr);
	return NULL;
#if 0
	int i;
	struct obj_header *header = global_objects[num_global_variables-1];
	for (i = 0; i < num_global_variables-1; i++) {
		if (ptr >= (char*)global_objects[i] && ptr < (char*)global_objects[i+1]) {
			header = global_objects[i];
		}
	}
	if (ptr >= (char*)header && ptr < ((char*)header) + header->size + OBJ_HEADER_SIZE) {
		return header;
	}
#endif
}

#if 0
static void print_data_section() {
	unsigned long long DataStart = 0;
	unsigned long long DataEnd = 0;
	unsigned long long *start, *end;
	getDataSecInfo(&DataStart, &DataEnd);
	assert(DataStart != 0 && DataEnd != 0);
	start = (unsigned long long*)DataStart;
	end = (unsigned long long*)DataEnd;
	malloc_printf("printing data section\n");
	for (; start < end; start++) {
		malloc_printf("%p -> %llx\n", start, *start);
	}
}
#endif

extern char  etext, edata, end;

#if 0
static bool is_global(unsigned long long val) {
	static unsigned long long DataStart = 0;
	static unsigned long long DataEnd = 0;

	if (DataStart == 0) {
		getDataSecInfo(&DataStart, &DataEnd);
		assert(DataStart > 0 && DataEnd > 0);
	}
	//malloc_printf("DataStart: %llx DataEnd:%llx\n", DataStart, DataEnd);
	return val >= DataStart && val < DataEnd;
}
#endif

#define ENTRY_TY 0
#define EXIT_TY 1
#define ICMP_TY 2
#define LOAD_TY 3
#define STORE_TY 4
#define PTR_TO_INT_TY 5
#define SUB_TY 6

static inline void
set_trace_fp() {
	if (trace_fp == NULL) {
		trace_fp = fopen("trace.txt", "w");
		if (trace_fp == NULL) {
			trace_fp = fopen("trace1.txt", "w");
			assert(trace_fp != NULL);
		}
	}
}


JEMALLOC_EXPORT
void je_san_typeset(uint64_t *ptr, uint64_t size, uint64_t tysize, uint64_t bitmap)
{
	size_t n_elem = size / tysize;
	size_t i, j, k;
	size_t idx = 0;
	size_t max_idx = size / 8;

	for (i = 0; i < n_elem; i++) {
		k = 0;
		for (j = 0; j < tysize/8; j++) {
			if (((bitmap >> k) & 1)) {
				ptr[idx] = 0;
			}
			idx++;
			k = (k+1 == 64) ? 0 : k+1;
		}
	}
	if (idx != max_idx) {
		k = 0;
		while (idx != max_idx) {
			if (((bitmap >> k) & 1)) {
				ptr[idx] = 0;
			}
			k = (k+1 == 64) ? 0 : k+1;
			idx++;
		}
	}
}

JEMALLOC_EXPORT
void je_san_trace(char *_name, int line, int type, unsigned long long val, unsigned long long _val) {
	event_id++;
	if (!can_print_in_trace_fp()) {
		return;
	}
	trace_count--;

	static unsigned long long id = 0;
	char *name = UNMASK(_name);

	if (type == ENTRY_TY) {
		fprintf(trace_fp, "[%lld] enter: %s()(%p, %p) %d\n", id, name, (void*)val, (void*)_val, line);
	}
	else if (type == EXIT_TY) {
		fprintf(trace_fp, "[%lld] exit: %s():%d -> %llx\n", id, name, line, val);
	}
	else if (type == ICMP_TY) {
		//fprintf(trace_fp, "[%lld] icmp: %d -> %llx\n", id, line, val1);
	}
	else if (type == LOAD_TY) {
		//if ((val1 >> 48) == 0xcaba) {
			//fprintf(err_fp, "[%lld] ld: %s:%d -> %llx\n", id, name, line, val1);
		//}
		//if (val) {
			fprintf(trace_fp, "ld: %p = [%p] line:%d\n", *(void**)val, (void*)val, line);
			if (((size_t)val) >> 48) {
				abort3("invalid address ld");
			}
		//}
	}
	else if (type == STORE_TY) {
		//if ((val >> 48) == 0xcaba) {
			//fprintf(err_fp, "[%lld] st: %s:%d -> %llx\n", id, name, line, val1);
		//}
		//if (val) {
			fprintf(trace_fp, "st: [%p] = %p line:%d\n", (void*)_val, (void*)val, line);
		//}
			if (((size_t)_val) >> 48) {
				abort3("invalid address st");
			}
	}
	else if (type == PTR_TO_INT_TY) {
		//if ((val1 >> 48) == 0xcaba) {
			//fprintf(err_fp, "[%lld] pi: %s:%d -> %llx\n", id, name, line, val1);
		//}
		//fprintf(trace_fp, "[%lld] pi: %d -> %llx\n", id, line, val);
	}
	else if (type == SUB_TY) {
		//if ((val1 >> 48) == 0xcaba) {
			//fprintf(err_fp, "[%lld] sub: %s:%d -> %llx\n", id, name, line, val1);
		//}
		//fprintf(trace_fp, "[%lld] sub: %d -> %llx\n", id, line, val);
	}
	else {
		abort3("incorrect_type");
	}
	id++;

	if (id >= 7099) {
		//debug_break(NULL);
	}
	//fsync(fileno(trace_fp));
}


#define MAX_STACK_PTRS 100000

__attribute__((visibility(
    "default"))) __thread void *fasan_stack_ptrs[MAX_STACK_PTRS];

__attribute__((visibility(
    "default"))) __thread int num_fasan_stack_ptrs = 0;
//static __thread void *callstack[MAX_STACK_PTRS];
//static __thread int num_callstack = 0;

JEMALLOC_EXPORT
void* je_san_enter_scope(int num_ptrs) {
	//callstack[num_callstack++] = (void*)&fasan_stack_ptrs[num_fasan_stack_ptrs];
	//if (event_id > min_events) {
		//malloc_printf("enter_scope %d %p\n", num_fasan_stack_ptrs, &fasan_stack_ptrs[num_fasan_stack_ptrs]);
	//}
	void *ret = &fasan_stack_ptrs[num_fasan_stack_ptrs];
	num_fasan_stack_ptrs += num_ptrs;
	return ret;
}

JEMALLOC_EXPORT
void je_san_exit_scope(char *ptr) {
	//num_callstack--;
	num_fasan_stack_ptrs = (ptr - (char*)&fasan_stack_ptrs[0]) / sizeof(void*);
	//if (event_id > min_events) {
		//malloc_printf("exit_scope :%d %p\n", num_fasan_stack_ptrs, ptr);
	//}
	//assert(callstack[num_callstack] == (void*)ptr);
}

JEMALLOC_EXPORT
void je_san_restore_scope(char *ptr) {
	/*assert(num_callstack > 0);
	while (callstack[num_callstack-1] != ptr) {
		num_callstack--;
		assert(num_callstack > 0);
	}*/
	void *top_ptr = (void*)&ptr;
	int i;
	for (i = num_fasan_stack_ptrs-1; i >= 0 && fasan_stack_ptrs[i] <= top_ptr; i--) {
		num_fasan_stack_ptrs--;
	}
	if (event_id > min_events) {
		if (trace_fp) {
			fprintf(trace_fp, "restore_scope :%d %p\n", num_fasan_stack_ptrs, ptr);
		}
		else {
			malloc_printf("restore_scope :%d %p\n", num_fasan_stack_ptrs, ptr);
		}
	}
}

// change llvm before changing name
JEMALLOC_EXPORT
void je_san_record_stack_pointer(void *ptr) {
	assert(0);
	assert(num_fasan_stack_ptrs < MAX_STACK_PTRS);
	assert(ptr);
	fasan_stack_ptrs[num_fasan_stack_ptrs] = ptr;
	if (event_id > min_events) {
		if (trace_fp) {
			fprintf(trace_fp, "%lld recording:%p %d\n", event_id, ptr, num_fasan_stack_ptrs);
		}
		else {
			malloc_printf("recording:%p %d\n", ptr, num_fasan_stack_ptrs);
		}
	}


	num_fasan_stack_ptrs++;
}

static void print_stack(void *ptr) {
	if (trace_fp == NULL) {
		return;
	}
	int i;
	fprintf(trace_fp, "num_fasan_stack_ptrs:%d event_id:%lld begin:%p\n", num_fasan_stack_ptrs, event_id, je_stack_begin);
	for (i = num_fasan_stack_ptrs-1; i >= 0; i--) {
		assert(fasan_stack_ptrs[i]);
		unsigned *sizeptr = ((unsigned*)fasan_stack_ptrs[i]) - 1;
		unsigned size = sizeptr[0];
		fprintf(trace_fp, "size:%d magic:%x start:%p ptr:%p\n", size, *(sizeptr-1), sizeptr+1, ptr);
	}
}

static void* get_stack_ptr_base(void *ptr) {
	int i;
	for (i = num_fasan_stack_ptrs-1; i >= 0; i--) {
		assert(fasan_stack_ptrs[i]);
		unsigned *sizeptr = ((unsigned*)fasan_stack_ptrs[i]) - 1;
		if ((unsigned*)ptr >= sizeptr) {
			unsigned size = sizeptr[0];
			if ((char*)ptr < ((char*)fasan_stack_ptrs[i]) + size) {
				unsigned *ret = sizeptr - 1;
				if (!IS_MAGIC(ret[0])) {
					malloc_printf("no magic\n");
					print_stack(ptr);
					return NULL;
				}
				assert(IS_MAGIC(ret[0]));
				return ret;
			}
		}
	}
	assert(0);
	//print_stack(ptr);
	return NULL;
}



#define STACK_SIZE (8092 * 1024)

static void *get_global_header1(unsigned *ptr) {
	int iter = 0;
	unsigned *base = ptr;
	while (!IS_MAGIC(ptr[0]) && iter++ < 10000000) {
		ptr = (unsigned*)(((char*)ptr) -1);
	}
	if (ptr[0] != MAGIC_NUMBER) {
		malloc_printf("unable to find base corresponding to %p ptr:%p\n", base, ptr);
		assert(0);
		return NULL;
	}
	malloc_printf("base:%p ptr:%p val:%llx\n", ptr, base, *(unsigned long long*)ptr);
	return ptr;
}

static bool is_stack_ptr(char *ptr) {
	if (!stack_initialized) {
		return false;
	}
	//assert(je_stack_begin != NULL);
	if (je_stack_begin == NULL) {
		pthread_attr_t attr;
		void * stackaddr;
		size_t stacksize;
		pthread_t tid = pthread_self();

		pthread_getattr_np(tid, &attr);
		pthread_attr_getstack( &attr, &stackaddr, &stacksize );
		je_stack_begin = ((char*)stackaddr) + stacksize;
	}
	assert(je_stack_begin);

	char stack_var;
	char *lower = &stack_var;
	char *higher = je_stack_begin;
	return ptr >= lower && ptr <= higher;
}

static void *__je_san_get_base(void *ptr) {
	//fprintf(trace_fp, "san_get_base1:%p\n", ptr);
	if (ptr < (void*)0x80000000) {
		void *ret = get_global_header(ptr);
		if (ret == NULL) {
			ret = get_global_header1(ptr);
			assert(0);
		}
		return ret;
	}
	if (is_stack_ptr(ptr)) {
		assert(0);
		unsigned *ret = get_stack_ptr_base(ptr);
		if (ret) {
			return ret;
		}
		else {
			malloc_printf("unable to find base corresponding to : %p\n", ptr);
			//assert(0);
			return NULL;
		}
	}

	tsd_t *tsd = tsd_fetch_min();
	tsdn_t *tsdn = tsd_tsdn(tsd);
	assert(tsdn);
	char *ptr_page = (char*)ptr;
	//extent_t *e = iealloc(tsdn, ptr_page);

	rtree_ctx_t rtree_ctx_fallback;
  rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

  extent_t *e = rtree_extent_read(tsdn, &extents_rtree, rtree_ctx,
      (uintptr_t)ptr_page, false);

	if (e == NULL) {
		void *ret = san_largeheader(ptr);
		assert(ret);
		return ret;
	}


	/*if (e == NULL) {
		ptr_page = search_large_pointer(ptr);
		assert(ptr_page);
		e = iealloc(tsdn, ptr_page);
	}*/
	if (!e) {
		//failed_large(ptr);
		malloc_printf("unable to find the base of : %p base:%p\n", ptr, ptr_page);
	}
	assert(e);
	char *eaddr = extent_addr_get(e);
	if (!extent_slab_get(e)) {
		return eaddr;
	}
	int szind = extent_szind_get(e);
	size_t diff = (size_t)((char*)ptr - eaddr);
	size_t offset = diff % bin_infos[szind].reg_size;
	return ptr - offset;
}

static void *_je_san_get_base(void *ptr) {
	void *fbase = get_fast_base((size_t)ptr);
	if (fbase) {
		return fbase;
	}
	void *_ptr = UNMASK(ptr);
	struct obj_header *head = (struct obj_header*)__je_san_get_base(_ptr);
	assert(IS_MAGIC(head->magic));
	if (head->offset) {
		head = (struct obj_header*)((char*)head + head->offset);
	}
	return head;
}

static void *__je_san_get_base1(void *ptr);

static void *_je_san_get_base3(void *ptr)
{
	void *_ptr = UNMASK(ptr);
	struct obj_header *head = (struct obj_header*)__je_san_get_base1(_ptr);
	assert(IS_MAGIC(head->magic));

	if (head->offset) {
		head = (struct obj_header*)((char*)head + head->offset);
	}
	return head;
}


JEMALLOC_EXPORT
void* je_san_interior(void *_base, void *_ptr, size_t ID) {
	if (_base == NULL) {
		fprintf(trace_fp, "_base:%p _ptr:%p ID:%zd\n", _base, _ptr, ID);
		abort3("hi");
	}
	size_t mask = (((size_t)_base) ^ ((size_t)_ptr)) >> 48;
	assert(mask == 0);
	if (_base == _ptr) {
		return _base;
	}
	if (can_print_in_trace_fp()) {
		fprintf(trace_fp, "%s: ptr:%p base:%p\n", __func__, _ptr, _base);
	}
	char *interior2 = (char*)get_interior_add((size_t)_ptr, (size_t)(_ptr-_base));

#if 0
	void *ptr = UNMASK(_ptr);
	unsigned *head = _je_san_get_base(_base);
	assert(head);
	char *start = (char*)(head + 2);
	char *interior1 = (char*)get_interior((size_t)ptr, (size_t)((char*)ptr-start));
	if (interior1 != interior2) {
		fprintf(trace_fp, "Interior1:%p Interior2:%p base:%p ptr:%p\n", interior1, interior2, _base, _ptr);
		//abort3("hi");
	}
#endif
	return interior2;
}

static void *ptr_to_iptr(void *_ptr) {
	void *ptr = UNMASK(_ptr);
	void *base = _je_san_get_base3(ptr) + 8;
	return je_san_interior(base, ptr, 1000);
}

static void *__je_san_get_base1(void *ptr) {
	//fprintf(trace_fp, "san_get_base2:%p\n", ptr);
	if (ptr < MinGlobalAddr) {
		return NULL;
	}
	if (ptr < (void*)0x80000000) {
		void *ret = get_global_header(ptr);
		return ret;
	}
	if (is_stack_ptr(ptr)) {
		unsigned *ret = get_stack_ptr_base(ptr);
		return ret;
	}

	tsd_t *tsd = tsd_fetch_min();
	tsdn_t *tsdn = tsd_tsdn(tsd);
	assert(tsdn);
	char *ptr_page = (char*)ptr;

	rtree_ctx_t rtree_ctx_fallback;
  rtree_ctx_t *rtree_ctx = tsdn_rtree_ctx(tsdn, &rtree_ctx_fallback);

  extent_t *e = rtree_extent_read(tsdn, &extents_rtree, rtree_ctx,
      (uintptr_t)ptr_page, false);


	//extent_t *e = iealloc(tsdn, ptr_page);
	if (!e) {
		return san_largeheader(ptr);
	}
	assert(e);
	char *eaddr = extent_addr_get(e);
	if (!extent_slab_get(e)) {
		return eaddr;
	}
	int szind = extent_szind_get(e);
	size_t diff = (size_t)((char*)ptr - eaddr);
	size_t offset = diff % bin_infos[szind].reg_size;
	return ptr - offset;
}

static void *_je_san_get_base1(void *ptr) {
	bool large_offset = false;
	void *fbase = get_fast_base_safe((size_t)ptr, &large_offset);
	if (fbase) {
		return fbase;
	}
	if (!large_offset) {
		return NULL;
	}
	void *_ptr = UNMASK(ptr);

	struct obj_header *head = (struct obj_header*)__je_san_get_base1(_ptr);
	if (head == NULL) {
		return head;
	}
	assert(IS_MAGIC(head->magic));
	if (head->offset) {
		 head = (struct obj_header*)((char*)head + head->offset);
	}
	if (!IS_MAGIC(head->magic)) {
		fprintf(trace_fp, "no-magic after offset :%p ptr:%p\n", head, ptr);
		abort3("hi");
	}
	return head;
}

JEMALLOC_EXPORT
void *je_san_base(void *_base)
{
	void *base = UNMASK(_base);
	void *ret = __je_san_get_base1(base);
	return (ret == NULL) ? ret : ret+8;
}


JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
je_malloc(size_t size) {
	if (size == 0) {
		return NULL;
	}

	print_allocations(size);
	void *ret;
	if (size < MAX_OFFSET) {
		ret = _je_malloc(size + OBJ_HEADER_SIZE);
	}
	else {
		ret = san_largealloc(size + OBJ_HEADER_SIZE);
	}
	assert(ret);
	add_large_pointer(ret, size + OBJ_HEADER_SIZE);
	if (can_print_in_trace_fp()) {
		static int reentry = 0;
		if (reentry == 0) {
			reentry = 1;
			fprintf(trace_fp, "%s(): ptr:%p, size:%zd\n", __func__, ret, size);
			reentry = 0;
		}
	}
	//remove_faulty_pages((size_t)ret);
	return make_obj_header(ret, size, 0, 0);
}

JEMALLOC_EXPORT
void* je_san_get_limit(void *ptr) {
	void *ptr1 = UNMASK(ptr);

	struct obj_header *head = (struct obj_header*)__je_san_get_base(ptr1);
	assert(IS_MAGIC(head->magic));
	if (head->offset) {
		head = (struct obj_header*)((char*)head + head->offset);
	}
	return (void*)(head+1);
#if 0
	char *limit = (char*)(head+1) + head->size;
	if (ptr != ptr1) {
		size_t offset = get_offset_from_ptr((size_t)ptr);
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "%s: ptr:%p base:%p limit:%p\n", __func__, ptr, head, limit);
		}
		limit = (char*)get_interior((size_t)limit, offset);
	}
	return limit;
#endif
}

JEMALLOC_EXPORT
char* je_san_make_interior(char *ptr) {
	assert(0);
	char* optr = UNMASK(ptr);
	assert(optr != ptr);
	char *base = (char*)_je_san_get_base(ptr);
	if (base != ptr) {
		if (ptr > base) {
			return (char*)get_interior((size_t)ptr, (size_t)(ptr-base));
		}
	}
	return ptr;
}

static char *null_name = "null_name";

JEMALLOC_EXPORT
void je_san_memcpy(unsigned long long *src, unsigned size, int line, char *name) {
	event_id++;
	unsigned i;
	size = size / 8;
	for (i = 0; i < size; i++) {
		if (need_tracking(src[i])) {
			name = (name < (char*)0x1000) ? null_name : name;
			malloc_printf("%lld %lld memcpy: src:%p sz:%x %s():%d %d %d\n", 
				event_id, min_events, src, size, name, (line & 0xffff), (line>>16), line);
		}
	}
}

JEMALLOC_EXPORT
void je_san_icmp(unsigned long long val1, unsigned long long val2, int line, char *name) {
	event_id++;
	if (val1 == (unsigned long long)-1 || val2 == (unsigned long long)-1) {
		return;
	}
	if ((val1 >> 48) == INTERIOR_STR1 || (val2 >> 48) == INTERIOR_STR1) {
		name = (name < (char*)0x1000) ? null_name : name;
		malloc_printf("%lld %lld icmp: val1:%llx val2:%llx %s():%d %d %d\n", 
			event_id, min_events, val1, val2, name, (line & 0xffff), (line>>16), line);
		//assert(0);
	}
}

JEMALLOC_EXPORT
void* je_san_page_fault_load(void *ptr, int line, char *name) {
	event_id++;
	//malloc_printf("1. store: ptr:%p val:%p\n", ptr, val);
	void *optr = (void*)UNMASK(ptr);
	strncpy(LastName, name, 64);
	LastLine = line;

	if (can_print_in_trace_fp()) {
		name = (name < (char*)0x1000) ? null_name : UNMASK(name);
		if (trace_fp) {
			fprintf(trace_fp, "%lld load: ptr:%p val:%p %s():%d %d %d\n", 
				event_id, ptr, *((char**)optr), name, (line & 0xffff), (line>>16), line);
		}
		else {
			malloc_printf("%lld load: ptr:%p val:%p %s():%d %d %d\n", 
				event_id, ptr, *((char**)optr), name, (line & 0xffff), (line>>16), line);
		}
	}

	if (((((size_t)ptr)>>48) & 1) != 0) {
		unsigned *base = _je_san_get_base1(optr);
			malloc_printf("base:%p ptr:%p\n", base, ptr);
		if (base == NULL) {
			malloc_printf("base:%p ptr:%p\n", base, ptr);
			ABORT4( ((((size_t)ptr)>>48) & 1) == 0 );
		}
		base = (unsigned*)((char*)base + OBJ_HEADER_SIZE);

		if (((char*)base) > (char*)optr || (char*)optr >= ((char*)base)+*(base-1)) {
			malloc_printf("base:%p ptr:%p len:%d\n", base, ptr, *(base-1));
			ABORT4( ((((size_t)ptr)>>48) & 1) == 0 );
		}
	}
	ABORT4( ((((size_t)ptr)>>48) & 1) == 0 );
#if 0
	void *oval = (void*)(((unsigned long long)val) & 0x7fffffffffffffffULL);
	if (oval) {
		void *base = _je_san_get_base(val);
		if (base+1 != oval) {
			if (oval == val) {
				malloc_printf("store: ptr:%p val:%p\n", ptr, val);
				assert(0);
			}
		}
	}
	//void *base = NULL; //je_san_get_base(optr);
	//printf("base:%p optr:%p ptr:%p\n", base, optr, ptr);
#endif
	return optr;
}

JEMALLOC_EXPORT
void* je_san_page_fault_store(void *ptr, void *val, int line, char *name) {
	event_id++;
	void *optr = (void*)UNMASK(ptr);
	LastLine = line;

	//void *oval = (void*)UNMASK(val);
	if (can_print_in_trace_fp()) {
		name = (name < (char*)0x1000) ? null_name : UNMASK(name);
		if (trace_fp) {
			fprintf(trace_fp, "%lld store: ptr:%p val:%p %d %d %d\n", event_id, ptr, val, (line & 0xffff), (line>>16), line);
		}
		else {
			malloc_printf("%lld store: ptr:%p val:%p %d %d %d\n", event_id, ptr, val, (line & 0xffff), (line>>16), line);
		}
	}

	if (((((size_t)ptr)>>48) & 1) != 0) {
		unsigned *base = _je_san_get_base1(optr);
			malloc_printf("st: base:%p ptr:%p\n", base, ptr);
		if (base == NULL) {
			malloc_printf("base:%p ptr:%p\n", base, ptr);
			ABORT4( ((((size_t)ptr)>>48) & 1) == 0 );
		}
		base = (unsigned*)((char*)base + OBJ_HEADER_SIZE);

		if (((char*)base) > (char*)optr || (char*)optr >= ((char*)base)+*(base-1)) {
			malloc_printf("base:%p ptr:%p len:%d\n", base, ptr, *(base-1));
			ABORT4( ((((size_t)ptr)>>48) & 1) == 0 );
		}
	}

	ABORT4( ((((size_t)ptr)>>48) & 1) == 0 );
#if 0
	void *oval = (void*)(((unsigned long long)val) & 0x7fffffffffffffffULL);
	if (oval) {
		void *base = _je_san_get_base(val);
		if (base+1 != oval) {
			if (oval == val) {
				malloc_printf("store: ptr:%p val:%p\n", ptr, val);
				assert(0);
			}
		}
	}
	//void *base = NULL; //je_san_get_base(optr);
	//printf("base:%p optr:%p ptr:%p\n", base, optr, ptr);
#endif
	return optr;
}

#if 0
// change llvm before changing the name
JEMALLOC_EXPORT
void je_san_alloca(void *ptr1, size_t size, int line, char *name) {
	event_id++;
	if (event_id > min_events) {
		name = (name < (char*)0x1000) ? null_name : name;
		malloc_printf("%lld alloca: ptr:%p sz:%zd %s():%d %d %d\n", event_id, ptr1, size, name, (line & 0xffff), (line>>16), line);
	}
}
#endif

JEMALLOC_EXPORT
void je_san_call(void *ptr1, void *ptr2, int line, char *name) {
	event_id++;
	if (event_id > min_events) {
		name = (name < (char*)0x1000) ? null_name : name;
		malloc_printf("%lld call: ptr1:%p ptr2:%p %s():%d %d %d\n", event_id, ptr1, ptr2, name, (line & 0xffff), (line>>16), line);
	}
}

JEMALLOC_EXPORT
void je_san_page_fault_ret(void *ptr, int line, char *name) {
	event_id++;
	if (event_id > min_events) {
		name = (name < (char*)0x1000) ? null_name : name;
		malloc_printf("%lld ret: ptr:%p %s():%d %d %d\n", event_id, ptr, name, (line & 0xffff), (line>>16), line);
	}
	//void *base = NULL; //je_san_get_base(optr);
	//printf("base:%p optr:%p ptr:%p\n", base, optr, ptr);
}

JEMALLOC_EXPORT
void je_san_page_fault_arg(void *ptr, int line, char *name) {
	event_id++;
	if (event_id > min_events) {
		name = (name < (char*)0x1000) ? null_name : name;
		malloc_printf("%lld arg: ptr:%p %s():%d %d %d\n", event_id, ptr, name, (line & 0xffff), (line>>16), line);
	}
	//void *base = NULL; //je_san_get_base(optr);
	//printf("base:%p optr:%p ptr:%p\n", base, optr, ptr);
}

JEMALLOC_EXPORT
void* je_san_page_fault(void *ptr, int line, char *name) {
	event_id++;
	void *optr = (void*)UNMASK(ptr);
	if (event_id > min_events) {
		name = (name < (char*)0x1000) ? null_name : name;
		malloc_printf("%lld ld/st: ptr:%p %s():%d %d %d\n", event_id, ptr, name, (line & 0xffff), (line>>16), line);
	}
	//void *base = NULL; //je_san_get_base(optr);
	//printf("base:%p optr:%p ptr:%p\n", base, optr, ptr);
	return optr;
}

JEMALLOC_EXPORT
void* je_san_page_fault_call(void *ptr, int line, char *name) {
	event_id++;
	void *optr = (void*)UNMASK(ptr);
	if (event_id > min_events) {
		name = (name < (char*)0x1000) ? null_name : name;
		malloc_printf("%lld call: ptr:%p %s():%d %d %d\n", event_id, ptr, name, (line & 0xffff), (line>>16), line);
	}
	//void *base = NULL; //je_san_get_base(optr);
	//printf("base:%p optr:%p ptr:%p\n", base, optr, ptr);
	return optr;
}

static void print_all_obstack();


static
void* je_san_interior1(const void *_base, const void *_ptr) {
	size_t offset = ((size_t)_base) >> 48;
	_ptr = (const void*)((size_t)_ptr | (offset << 48));
	return je_san_interior((void*)_base, (void*)_ptr, 1001);
}

#if 0
static void* je_san_interior2(const void *_base, const void *_ptr)
{
	struct obj_header *head = (struct obj_header*)_je_san_get_base1((void*)_base);
	char *base = ((char*)&head[1]);
	char *ptr = UNMASK(_ptr);
	size_t offset = ptr - base;

	if (offset < head->size) {
		ptr = (char*)((size_t)ptr | (offset << 49));
	}
	else {
		ptr = (char*)((size_t)ptr | (1ULL << 48));
	}
	return ptr;
}
#endif

JEMALLOC_EXPORT
void* je_san_interior_checked(void *_base, void *_ptr, size_t ptrsize, int ID) {
	assert(0);
	if (_base == _ptr) {
		return _ptr;
	}
	void *ptr = UNMASK(_ptr);
	void *base = UNMASK(_base);

	if (ptr < MinGlobalAddr || base < MinGlobalAddr || is_invalid_ptr((size_t)_base)) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior1: ptr:%p base:%p\n", ptr, base);
		}
		return _MASK1(ptr);
	}

#if 0
	bool large_offset = false;
	unsigned *head = get_fast_base_safe((size_t)_base, &large_offset);

	if (head == NULL && !large_offset) {
		return _MASK1(ptr);
	}
#endif

	unsigned *head = _je_san_get_base1(_base);
	if (head == NULL) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior2: ptr:%p base:%p\n", ptr, base);
		}
		return _MASK1(ptr);
	}

	if (!IS_MAGIC(head[0])) {
		fprintf(trace_fp, "_base:%p _ptr:%p head:%p\n", _base, _ptr, head);
		abort3("hi");
	}
	assert(IS_MAGIC(head[0]));

	unsigned size = head[1];
	char *start = (char*)(head + 2);
	char *end = start + size;
	if (ptr < (void*)start || ptr + ptrsize > (void*)end) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior: start:%p ptr:%p base:%p size:%d end:%p ptrsize:%zd lastline:%d lastname:%s\n", 
				start, ptr, base, size, end, ptrsize, LastLine, LastName);
			//trace_count = 10;
		}
		//if (ptrsize == 224 || ptrsize == 96 || (size == 16 && ptrsize==24) || ptrsize==64 || ptrsize == 48 || ptrsize == 4)
			//abort3("hi");
		return _MASK1(ptr);
	}
	if (can_print_in_trace_fp()) {
		fprintf(trace_fp, "%s: ptr:%p base:%p obase:%p\n", __func__, ptr, base, start);
	}
	return (char*)get_interior((size_t)ptr, (size_t)((char*)ptr-start));
}

JEMALLOC_EXPORT
void* je_san_check_size_limit(void *_ptr, size_t ptrsz, void *_limit) {
	if (_ptr + ptrsz > _limit) {
		return _MASK1(_ptr);
	}
	return _ptr;
}

JEMALLOC_EXPORT
void* je_san_interior_limit(void *_base, void *_ptr, size_t ptrsz, void *_limit) {
	//fprintf(trace_fp, "base:%p ptr:%p limit:%p\n", _base, _ptr, _limit);
	if (_ptr + ptrsz > _limit) {
		return _MASK1(_ptr);
	}
	return je_san_interior(_base, _ptr, 1002);
}


JEMALLOC_EXPORT
void* je_san_check_size(void *_ptr, size_t ptrsize) {
	void *ptr = UNMASK(_ptr);
	if (ptr == NULL) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior2: ptr:%p\n", ptr);
		}
		return _MASK1(ptr);
	}
	if (ptr < MinGlobalAddr || is_invalid_ptr((size_t)_ptr)) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior4: ptr:%p\n", ptr);
		}
		return _MASK1(ptr);
	}

	unsigned *head = _je_san_get_base1(_ptr);
	if (head == NULL) {
		assert(0);
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior3: ptr:%p\n", ptr);
		}
		return _MASK1(ptr);
	}

	assert(IS_MAGIC(head[0]));

	unsigned size = head[1];
	char *start = (char*)(head + 2);
	char *end = start + size;
	if (ptr + ptrsize > (void*)end) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior: start:%p ptr:%p size:%d end:%p ptrsize:%zd lastline:%d lastname:%s\n", 
				start, ptr, size, end, ptrsize, LastLine, LastName);
			//trace_count = 10;
		}
		//if (ptrsize == 224 || ptrsize == 96 || (size == 16 && ptrsize==24) || ptrsize==64 || ptrsize==48 || ptrsize==4)
			//abort3("hi");
		return _MASK1(ptr);
	}
	return _ptr;
}


JEMALLOC_EXPORT
void* je_san_interior_must_check(void *_base, void *_ptr, size_t ptrsize, size_t ID) {
	assert(0);
	void *ptr = UNMASK(_ptr);
	void *base = UNMASK(_base);
	if (ptr == NULL || base == NULL) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior2: ptr:%p base:%p\n", ptr, base);
		}
		return _MASK1(ptr);
	}

	//malloc_printf("_base:%p\n", base);
	size_t offset = ((size_t)_base >> 49);
	if (offset == MAX_OFFSET && base > MinLargeAddr) {
		unsigned *head = (unsigned*)san_largeheader(base);
		//malloc_printf("Head:%p\n", head);
		assert(IS_MAGIC(head[0]));
		return head + 2;
	}

	unsigned *head = _je_san_get_base3(_base);
	if (head == NULL) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior3: ptr:%p base:%p\n", ptr, base);
		}
		return _MASK1(ptr);
	}

	assert(IS_MAGIC(head[0]));

	unsigned size = head[1];
	char *start = (char*)(head + 2);
	char *end = start + size;
	if (ptr < (void*)start || ptr + ptrsize > (void*)end) {
		if (can_print_in_trace_fp()) {
			fprintf(trace_fp, "making interior: start:%p ptr:%p base:%p size:%d end:%p ptrsize:%zd lastline:%d lastname:%s\n", 
				start, ptr, base, size, end, ptrsize, LastLine, LastName);
			//trace_count = 10;
		}
		return _MASK1(ptr);
	}
	if (can_print_in_trace_fp()) {
		fprintf(trace_fp, "%s: ptr:%p base:%p\n", __func__, ptr, start);
	}
	return (char*)get_interior((size_t)ptr, (size_t)((char*)ptr-start));
}

JEMALLOC_EXPORT
unsigned je_san_page_fault_len(void *ptr, int line, char *name) {
	event_id++;
	unsigned *optr = (unsigned*)UNMASK(ptr);
	char *orig = (char*)(optr + 1);
	unsigned offset = 0;

	if (can_print_in_trace_fp()) {
		//name = (name < (char*)0x1000) ? null_name : name;
		//malloc_printf("%lld len: ptr:%p %s():%d %d %d\n", event_id, ptr, name, (line & 0xffff), (line>>16), line);
		if (trace_fp) {
			fprintf(trace_fp, "%lld len: ptr:%p %d %d %d\n", event_id, ptr, (line & 0xffff), (line>>16), line);
		}
		else {
			malloc_printf("%lld len: ptr:%p %d %d %d\n", event_id, ptr, (line & 0xffff), (line>>16), line);
		}
	}

	if (((((size_t)ptr)>>48) & 1) != 0 ) {
		malloc_printf("%lld len: ptr:%p %d %d %d name:%s\n", event_id, ptr, (line & 0xffff), (line>>16), line, name);
	}
	ABORT4( ((((size_t)ptr)>>48) & 1) == 0 );


	unsigned magic = *(optr-1);
	unsigned *head;
	//malloc_printf("magic:%x size:%x\n", magic, optr[0]);
	if (!IS_MAGIC(magic) || ptr != optr) {
		head = _je_san_get_base(ptr+4);
		if (head == NULL) {
			if (trace_fp) {
				fprintf(trace_fp, "%lld ptr:%p head:%p line:%d\n", event_id, optr, head, line);
			}
			abort3("no-base");
		}

		if (!IS_MAGIC(head[0])) {
			malloc_printf("optr:%p head:%p\n", optr, head);
		}
		assert(IS_MAGIC(head[0]));
		//if (ptr > (void*)0x80000000/* && !is_stack_ptr(ptr)*/) {
			if (ptr == optr) {
				print_all_obstack();
				malloc_printf("%lld ptr:%p head:%p line:%d\n", event_id, optr, head, line);
				ABORT4(ptr != (void*)optr);
			}
			assert(ptr != (void*)optr);
		//}
		optr = head + 1;
		offset = (unsigned)(orig - ((char*)(optr+1)));
	}
	else {
		if (ptr != optr) {
			//malloc_printf("ptr:%p optr:%p\n", ptr, optr);
		}
		//assert(ptr == (void*)optr);
	}
	//fprintf(trace_fp, "get_limit: %p %x\n", optr, *optr);
	return (*optr) - offset;
}

JEMALLOC_EXPORT
void* je_san_page_fault_limit(void *ptr, int line, char *name) {
	assert(0);
	return ptr + je_san_page_fault_len(ptr-4, line, name);
}

JEMALLOC_EXPORT
void* je_san_page_fault_limit1(void *ptr) {
	void *ret = _je_san_get_base(ptr) + 8;
	addToGlobalCache(ret);
	return ret;
}

JEMALLOC_EXPORT
void* je_san_get_limit_check(void *_base) {
	void *base = UNMASK(_base);
	//size_t offset = ((size_t)_base & (0xFFFFULL << 48));

	if (base < MinGlobalAddr || is_invalid_ptr((size_t)_base)) {
		//fprintf(trace_fp, "base1:%p _limit:%p\n", _base, (void*)offset);
		return NULL; //(void*)offset;
	}

	unsigned *head = _je_san_get_base1(_base);
	if (head == NULL) {
		//fprintf(trace_fp, "base2:%p _limit:%p\n", _base, (void*)offset);
		return NULL; //(void*)offset;
	}

	if (!IS_MAGIC(head[0])) {
		fprintf(trace_fp, "_base:%p head:%p\n", _base, head);
		abort3("hi");
	}
	assert(IS_MAGIC(head[0]));
	addToGlobalCache(head+2);

	return head + 2;
#if 0
	unsigned size = head[1];
	char *start = (char*)(head + 2);
	char *end = start + size;
	void *ret = (void*)((size_t)end | offset);
	//fprintf(trace_fp, "base3:%p _limit:%p\n", _base, ret);
	return ret;
#endif
}


JEMALLOC_EXPORT
void* je_san_get_limit_must_check(void *_base) {
	void *base = UNMASK(_base);

	if (base < MinGlobalAddr || is_invalid_ptr((size_t)_base)) {
		//fprintf(trace_fp, "base11:%p _limit:%p\n", _base, (void*)offset);
		return NULL; //(void*)offset;
	}
	size_t Idx = san_hash_index((size_t)_base);
	unsigned *hashval = lookup_san_hash(base, Idx);
	if (hashval) {
		return hashval;
	}

	//malloc_printf("_base:%p\n", base);
	size_t offset = ((size_t)_base >> 49);
	if (offset == MAX_OFFSET && base > MinLargeAddr) {
		unsigned *head = (unsigned*)san_largeheader(base);
		//malloc_printf("Head:%p\n", head);
		assert(IS_MAGIC(head[0]));
		return head + 2;
	}

	unsigned *head = _je_san_get_base3(_base);
	if (head == NULL) {
		//fprintf(trace_fp, "base12:%p _limit:%p\n", _base, (void*)offset);
		return NULL; //(void*)offset;
	}

	if (!IS_MAGIC(head[0])) {
		fprintf(trace_fp, "_base:%p head:%p\n", _base, head);
		abort3("hi");
	}
	assert(IS_MAGIC(head[0]));
	//if (!is_stack_ptr(base)) {
		//add_to_san_hash(head+2, Idx);
	//}

	return head + 2;
#if 0

	unsigned size = head[1];
	char *start = (char*)(head + 2);
	char *end = start + size;
	void *ret = (void*)((size_t)end | offset);
	//fprintf(trace_fp, "base13:%p _limit:%p\n", _base, ret);
	return ret;
#endif
}


JEMALLOC_EXPORT
void je_san_check2(void *_ptr1, void *_ptr2, size_t ptrsize) {
#if 0
	if (ptrsize == 0) {
		return;
	}
	void *limit1 = je_san_page_fault_limit(_ptr1, 0, "");
	void *limit2 = je_san_page_fault_limit(_ptr2, 0, "");
	assert(ptrsize <= (size_t)((char*)limit1 - (char*)_ptr1));
	assert(ptrsize <= (size_t)((char*)limit2 - (char*)_ptr2));
#endif
}

JEMALLOC_EXPORT
void je_san_check1(void *_ptr1, size_t ptrsize) {
#if 0
	if (ptrsize == 0) {
		return;
	}
	void *limit1 = je_san_page_fault_limit(_ptr1, 0, "");
	size_t size = (size_t)((char*)limit1 - (char*)_ptr1);
	assert(ptrsize <= size);
#endif
}

JEMALLOC_EXPORT
void je_san_check3(void *_ptr1, void *_ptr2) {
#if 0
	void *limit1 = je_san_page_fault_limit(_ptr1, 0, "");
	void *limit2 = je_san_page_fault_limit(_ptr2, 0, "");
	size_t size1 = (size_t)((char*)limit1 - (char*)_ptr1);
	size_t size2 = (size_t)((char*)limit2 - (char*)_ptr2);
	size_t len = strlen(UNMASK(_ptr2)) + 1;
	assert(len <= size2);
	assert(len <= size1);
#endif
}

JEMALLOC_EXPORT
void je_san_check4(void *_ptr1, size_t ptrsize) {
	//void *limit1 = je_san_page_fault_limit(_ptr1, 0, "");
	//assert(ptrsize +1 <= (size_t)((char*)limit1 - (char*)_ptr1));
}

#if 0
struct fooalign {char x; double d;};
#define DEFAULT_ALIGNMENT  \
  ((PTR_INT_TYPE) ((char *) &((struct fooalign *) 0)->d - (char *) 0))

union fooround {long x; double d;};
#define DEFAULT_ROUNDING (sizeof (union fooround))
#ifndef COPYING_UNIT
#define COPYING_UNIT int
#endif

#if defined (__STDC__) && __STDC__
#define CALL_CHUNKFUN(h, size) \
  (((h) -> use_extra_arg) \
   ? (*(h)->chunkfun) ((h)->extra_arg, (size)) \
   : (*(struct _obstack_chunk *(*) (long)) (h)->chunkfun) ((size)))

#define CALL_FREEFUN(h, old_chunk) \
  do { \
    if ((h) -> use_extra_arg) \
      (*(h)->freefun) ((h)->extra_arg, (old_chunk)); \
    else \
      (*(void (*) (void *)) (h)->freefun) ((old_chunk)); \
  } while (0)
#else
#define CALL_CHUNKFUN(h, size) \
  (((h) -> use_extra_arg) \
   ? (*(h)->chunkfun) ((h)->extra_arg, (size)) \
   : (*(struct _obstack_chunk *(*) ()) (h)->chunkfun) ((size)))
#endif
#endif

#define CALL_FREEFUN(h, old_chunk) \
  do { \
    if ((h) -> use_extra_arg) \
      (*(h)->freefun) ((h)->extra_arg, (old_chunk)); \
    else \
      (*(void (*) ()) (h)->freefun) ((old_chunk)); \
  } while (0)


#include <dlfcn.h>

static void *get_func_addr(const char *name, void *wrapper) {
  void *addr = dlsym(RTLD_NEXT, name);
  if (!addr) {
    addr = dlsym(RTLD_DEFAULT, name);
    if (addr == wrapper) {
			assert(0);
      return NULL;
		}
  }
	assert(addr != wrapper);
	assert(addr);
  return addr;
}

JEMALLOC_EXPORT
char *je_setlocale(int category, const char *locale) {
	static char *ret[32] = {NULL};
	assert(category < 32);

	static char* (*fptr)(int, const char*) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("setlocale", je_setlocale);
		assert(fptr);
	}
	char *ret1 = fptr(category, locale);
	if (ret1) {
		size_t len = strlen(ret1);
		if (ret[category]) {
			if (len > 15) {
				je_free(ret[category]);
				ret[category] = je_malloc(len+1);
			}
		}
		else {
			size_t size = (len <= 15) ? 16 : len+1;
			ret[category] = je_malloc(size);
		}
		memcpy(ret[category], ret1, len);
		ret1 = ret[category];
		ret1[len] = '\0';
	}
	return ret1;
}

JEMALLOC_EXPORT
char *je_strerror_r(int errnum, char *buf, size_t buflen)
{
	static char *ret = NULL;
	static char* (*fptr)(int, char*, size_t) = NULL;

	if (fptr == NULL) {
		fptr = get_func_addr("strerror_r", je_strerror_r);
		assert(fptr);
	}
	char *ret1 = fptr(errnum, buf, buflen);
	if (ret1) {
		size_t len = strlen(ret1);
		if (ret == NULL || !strcmp(ret, ret1)) {
			char *ret = je_malloc(len+1);
			memcpy(ret, ret1, len);
			ret[len] = '\0';
		}
	}
	return ret;
}

JEMALLOC_EXPORT
char *je_strerror(int errnum)
{
	static char *ret[256] = {NULL};
	assert(errnum < 256);
	if (ret[errnum] != NULL) {
		return ret[errnum];
	}

	static char* (*fptr)(int) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("strerror", je_strerror);
		assert(fptr);
	}
	char *ret1 = fptr(errnum);
	size_t len = strlen(ret1);
	char *ret2 = je_malloc(len+1);
	ret[errnum] = ret2;
	memcpy(ret2, ret1, len);
	ret2[len] = '\0';
	return ret2;
}

JEMALLOC_EXPORT
ssize_t je_san_recv(int sockfd, void *buf, size_t len, int flags)
{
	static ssize_t (*fptr)(int, void*, size_t, int) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("recv", je_san_recv);
		assert(fptr);
	}
	struct obj_header *h = _je_san_get_base(buf);
	assert(len <= h->size);
	buf = UNMASK(buf);
	ssize_t ret = fptr(sockfd, buf, len, flags);
	return ret;
}

JEMALLOC_EXPORT
ssize_t je_writev(int fd, const struct iovec *iov, int iovcnt)
{
	void *base[iovcnt];
	int i;

	for (i = 0; i < iovcnt; i++) {
		base[i] = iov[i].iov_base;
		*(void**)(&iov[i].iov_base) = UNMASK(base[i]);
	}
	static ssize_t (*fptr)(int, const struct iovec*, int) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("writev", je_writev);
		assert(fptr);
	}
	ssize_t ret = fptr(fd, iov, iovcnt);
	for (i = 0; i < iovcnt; i++) {
		*(void**)(&iov->iov_base) = base[i];
	}
	return ret;
}

JEMALLOC_EXPORT
ssize_t je_readv(int fd, const struct iovec *iov, int iovcnt)
{
	void *base[iovcnt];
	int i;

	for (i = 0; i < iovcnt; i++) {
		base[i] = iov[i].iov_base;
		*(void**)(&iov[i].iov_base) = UNMASK(base[i]);
	}
	static ssize_t (*fptr)(int, const struct iovec*, int) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("readv", je_readv);
		assert(fptr);
	}
	ssize_t ret = fptr(fd, iov, iovcnt);
	for (i = 0; i < iovcnt; i++) {
		*(void**)(&iov->iov_base) = base[i];
	}
	return ret;
}

JEMALLOC_EXPORT
void *je_san_mmap(void *_addr, size_t length, int prot, int flags,
                  int fd, off_t offset)
{
	void *addr = UNMASK(_addr);
	void *ret = mmap(addr, length+PAGE_SIZE, prot, flags, fd, offset);
	if (ret != MAP_FAILED) {
		ret = make_obj_header(ret+PAGE_SIZE-OBJ_HEADER_SIZE, length, 0, 0);
	}
	//malloc_printf("mmap:: %p fd:%d\n", ret, fd);
	return ret;
}

JEMALLOC_EXPORT
void *je_san_mmap64(void *_addr, size_t length, int prot, int flags,
                    int fd, off_t offset)
{
	void *addr = UNMASK(_addr);
	void *ret = mmap64(addr, length+PAGE_SIZE, prot, flags, fd, offset);
	if (ret != MAP_FAILED) {
		ret = make_obj_header(ret+PAGE_SIZE-OBJ_HEADER_SIZE, length, 0, 0);
	}
	//malloc_printf("mmap:: %p fd:%d\n", ret, fd);
	return ret;
}

JEMALLOC_EXPORT
int je_san_munmap(void *_addr, size_t length)
{
	void *addr = UNMASK(_addr);
	//return munmap(addr-PAGE_SIZE, length);
	struct obj_header *head = (struct obj_header*)(addr - OBJ_HEADER_SIZE);
	assert(is_valid_obj_header(head));
	//malloc_printf("munmap:: %p\n", head);
	return munmap(addr-PAGE_SIZE, length+PAGE_SIZE);
}

JEMALLOC_EXPORT
struct tm *je_gmtime(const time_t *timep) {
	static struct tm* ret = NULL;
	if (ret == NULL) {
		ret = je_malloc(sizeof(struct tm));
		assert(ret);
	}

	static struct tm* (*fptr)(const time_t*) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("gmtime", je_gmtime);
		assert(fptr);
	}
	struct tm* ret1 = fptr(timep);
	memcpy(ret, ret1, sizeof(struct tm));
	return ret;
}


JEMALLOC_EXPORT
struct tm *je_localtime(const time_t *timep) {
	static struct tm* ret = NULL;
	if (ret == NULL) {
		ret = je_malloc(sizeof(struct tm));
		assert(ret);
	}

	static struct tm* (*fptr)(const time_t*) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("localtime", je_localtime);
		assert(fptr);
	}
	struct tm* ret1 = fptr(timep);
	memcpy(ret, ret1, sizeof(struct tm));
	return ret;
}


JEMALLOC_EXPORT
ssize_t je___getdelim(char **_lineptr, size_t *_n, int delim, FILE *_stream)
{
	char **lineptr = (char**)UNMASK(_lineptr);
	size_t *n = (size_t*)UNMASK(_n);
	FILE *stream = (FILE*)UNMASK(_stream);
	lineptr[0] = UNMASK(lineptr[0]);

	static ssize_t (*fptr)(char**, size_t*, int, FILE*) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("__getdelim", je___getdelim);
	}
	return fptr(lineptr, n, delim, stream);
}

#define MAX_INTERIOR 10

static bool is_interior(size_t addr) {
	size_t header = addr >> 40;
	return ((header >> 8) > 0) && ((header & 0xff) == 0x7f);
}


static int fix_varg_interiors(va_list ap, unsigned long long **fixes, unsigned long long *vals) {
	unsigned long long *reg_save_area, *mem_save_area;
	int num_fixes = 0;
	int i;

	reg_save_area = *(unsigned long long**)((unsigned long long)ap+16);
  mem_save_area = *(unsigned long long**)((unsigned long long)ap+8);
  if (mem_save_area) {
    for (i = 0; i < 8; i++) {
			if (is_interior(mem_save_area[i])) {
				assert(num_fixes < MAX_INTERIOR);
				vals[num_fixes] = mem_save_area[i];
				fixes[num_fixes++] = &mem_save_area[i];
				mem_save_area[i] = (unsigned long long)UNMASK(mem_save_area[i]);
			}
    }
  }
  if (reg_save_area) {
    for (i = 0; i < 8; i++) {
			if (is_interior(reg_save_area[i])) {
				assert(num_fixes < MAX_INTERIOR);
				vals[num_fixes] = reg_save_area[i];
				fixes[num_fixes++] = &reg_save_area[i];
				reg_save_area[i] = (unsigned long long)UNMASK(reg_save_area[i]);
			}
    }
  }
	return num_fixes;
}

#define MAX_DIRENT 128
struct dir_cache_entry {
	void *src;
	void *dst;
};

static struct dir_cache_entry dir_cache[MAX_DIRENT];
static int num_dir_cache_entries = 0;

static void add_dirent(void *e, void *target) {
	assert(num_dir_cache_entries < MAX_DIRENT);
	dir_cache[num_dir_cache_entries].src = e;
	dir_cache[num_dir_cache_entries].dst = target;
	num_dir_cache_entries++;
}

static void* lookup_dirent(void *e) {
	int i;
	for (i = 0; i < num_dir_cache_entries; i++) {
		if (dir_cache[i].src == e) {
			return dir_cache[i].dst;
		}
	}
	return NULL;
}

#include <dirent.h>

JEMALLOC_EXPORT
struct dirent *je_readdir(DIR *_dirp)
{
	DIR *dirp = (DIR*)UNMASK(_dirp);
	static struct dirent* (*fptr)(DIR*) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("readdir", je_readdir);
		assert(fptr);
	}
  struct dirent *e = fptr(dirp);
	if (e == NULL) {
		return NULL;
	}
	struct dirent *t = (struct dirent*)lookup_dirent(e);
	if (t == NULL) {
		t = (struct dirent*)je_malloc(sizeof(struct dirent));
		add_dirent(e, t);
	}
	memcpy(t, e, sizeof(struct dirent));
  return t;
}

JEMALLOC_EXPORT
struct dirent64 *je_readdir64(DIR *_dirp)
{
	DIR *dirp = (DIR*)UNMASK(_dirp);
	static struct dirent64* (*fptr)(DIR*) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("readdir64", je_readdir64);
		assert(fptr);
	}
  struct dirent64 *e = fptr(dirp);
	if (e == NULL) {
		return NULL;
	}
	struct dirent64 *t = (struct dirent64*)lookup_dirent(e);
	if (t == NULL) {
		t = (struct dirent64*)je_malloc(sizeof(struct dirent64));
		add_dirent(e, t);
	}
	memcpy(t, e, sizeof(struct dirent64));
  return t;
}


#include "wordcopy.c"

// Indirect call 

JEMALLOC_EXPORT
void *
je_memcpy (void *dstpp, const void *srcpp, size_t len)
{
  unsigned long int dstp = (long int) UNMASK(dstpp);
  unsigned long int srcp = (long int) UNMASK(srcpp);

  /* Copy from the beginning to the end.  */

  /* If there not too few bytes to copy, use word copy.  */
  if (len >= OP_T_THRES)
    {
      /* Copy just a few bytes to make DSTP aligned.  */
      len -= (-dstp) % OPSIZ;
      BYTE_COPY_FWD (dstp, srcp, (-dstp) % OPSIZ);

      /* Copy whole pages from SRCP to DSTP by virtual address manipulation,
	 as much as possible.  */

      PAGE_COPY_FWD_MAYBE (dstp, srcp, len, len);

      /* Copy from SRCP to DSTP taking advantage of the known alignment of
	 DSTP.  Number of bytes remaining is put in the third argument,
	 i.e. in LEN.  This number may vary from machine to machine.  */

      WORD_COPY_FWD (dstp, srcp, len, len);

      /* Fall out and copy the tail.  */
    }

  /* There are just a few bytes to copy.  Use byte memory operations.  */
  BYTE_COPY_FWD (dstp, srcp, len);

  return dstpp;
}

#ifdef __GNUC__
typedef __attribute__((__may_alias__)) size_t WT;
#define WS (sizeof(WT))
#endif

// Indirect call 

JEMALLOC_EXPORT
void *je_memmove(void *_dest, const void *_src, size_t n)
{
	void *dest = (void*)UNMASK(_dest);
	void *src = (void*)UNMASK(_src);
	char *d = dest;
	const char *s = src;

	if (d==s) return d;
	if ((uintptr_t)s-(uintptr_t)d-n <= -2*n) return memcpy(d, s, n);

	if (d<s) {
#ifdef __GNUC__
		if ((uintptr_t)s % WS == (uintptr_t)d % WS) {
			while ((uintptr_t)d % WS) {
				if (!n--) return _dest;
				*d++ = *s++;
			}
			for (; n>=WS; n-=WS, d+=WS, s+=WS) *(WT *)d = *(WT *)s;
		}
#endif
		for (; n; n--) *d++ = *s++;
	} else {
#ifdef __GNUC__
		if ((uintptr_t)s % WS == (uintptr_t)d % WS) {
			while ((uintptr_t)(d+n) % WS) {
				if (!n--) return _dest;
				d[n] = s[n];
			}
			while (n>=WS) n-=WS, *(WT *)(d+n) = *(WT *)(s+n);
		}
#endif
		while (n) n--, d[n] = s[n];
	}
	return _dest;
}

static void restore_varg(unsigned long long **fixes, unsigned long long *vals, int num_fixes) {
	int i;
	for (i = 0; i < num_fixes; i++) {
		unsigned long long new_val = *(fixes[i]);
		unsigned long long old_val = vals[i];
		if (!((new_val ^ old_val) << 16)) {
			*(fixes[i]) = vals[i];
		}
	}
}

JEMALLOC_EXPORT
const char *je_hstrerror(int err)
{
	static char *ret[128] = {NULL};
	assert(err < 128);
	if (ret[err] != NULL) {
		return ret[err];
	}

	static char* (*fptr)(int) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("hstrerror", je_hstrerror);
		assert(fptr);
	}
	char *ret1 = fptr(err);
	size_t len = strlen(ret1);
	char *ret2 = je_malloc(len+1);
	ret[err] = ret2;
	memcpy(ret2, ret1, len);
	ret2[len] = '\0';
	return ret2;
}

#if 0
// pthread comes first in library order
// need to fix arg
JEMALLOC_EXPORT
int je_pthread_create(pthread_t *_thread, const pthread_attr_t *_attr,
                   void *(*_start_routine) (void *), void *arg)
{
	pthread_t *thread = (pthread_t*)UNMASK(_thread);
	const pthread_attr_t *attr = (const pthread_attr_t*)UNMASK(_attr);
	void *(*start_routine) (void *) = (void *(*) (void *))UNMASK(_start_routine);

	int (*fptr)(pthread_t *, const pthread_attr_t *,
              void *(*start_routine) (void *), void *) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("pthread_create", je_pthread_create);
		assert(fptr);
	}
	return fptr(thread, attr, start_routine, arg);
}
#endif

#include <netdb.h>

JEMALLOC_EXPORT
int je_gethostbyname_r(const char *name,
               struct hostent *ret, char *buf, size_t buflen,
               struct hostent **result, int *h_errnop)
{
	static int (*fptr)(const char*, struct hostent*, char*, size_t,
		struct hostent**, int*) = NULL;

	if (fptr == NULL) {
		fptr = get_func_addr("gethostbyname_r", je_gethostbyname_r);
		assert(fptr);
	}

	struct hostent *retval = *result;
  int r = fptr(name, ret, buf, buflen, result, h_errnop);
	retval = *result;

	int i = 0;
	size_t offset;
	while (retval->h_addr_list[i]) {
		char *ptr = retval->h_addr_list[i];
		offset = (size_t)(ptr - buf);
		assert(offset < buflen);
		retval->h_addr_list[i] = (char*)get_interior((size_t)retval->h_addr_list[i], offset);
		i++;
	}
	offset = (size_t)((char*)retval->h_addr_list - buf);
	assert(offset < buflen);
	retval->h_addr_list = (char**)get_interior((size_t)retval->h_addr_list, offset);

  return r;
}

JEMALLOC_EXPORT
int je_vsprintf(char *str, const char *format, va_list ap) {
	unsigned long long *fixes[MAX_INTERIOR];
	unsigned long long vals[MAX_INTERIOR];
	int num_fixes = fix_varg_interiors(ap, fixes, vals);

	static int (*fptr)(char*, const char*, va_list) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("vsprintf", je_vsprintf);
		assert(fptr);
	}
  int ret = fptr(str, format, ap);
  restore_varg(fixes, vals, num_fixes);
  return ret;
}

JEMALLOC_EXPORT
char *je_getenv(const char *name)
{
	static char *Cache1[64] = {NULL};
	static char *Cache2[64] = {NULL};
	static char* (*fptr)(const char*) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("getenv", je_getenv);
	}
	char *Ret = fptr(name);
	if (Ret == NULL) {
		return NULL;
	}

	int i;
	int Len = strlen(Ret);
	char *Ret1 = je_malloc(Len+1);
	memcpy(Ret1, Ret, Len);
	Ret1[Len] = '\0';

	for (i = 0; i < 63; i++) {
		if (Cache1[i] == Ret) {
			je_free(Cache2[i]);
			Cache2[i] = Ret1;
			return Ret1;
		}
		else if (Cache1[i] == NULL) {
			Cache1[i] = Ret;
			Cache2[i] = Ret1;
			return Ret1;
		}
	}

	Cache1[i] = Ret;
	Cache2[i] = Ret1;
	return Ret1;
}

#include <pwd.h>
#include <grp.h>

JEMALLOC_EXPORT
struct passwd *je_getpwnam(const char *name)
{
	static struct passwd *Cache1[64] = {NULL};
	static struct passwd *Cache2[64] = {NULL};
	static struct passwd* (*fptr)(const char*) = NULL;
	int i;

	if (fptr == NULL) {
		fptr = get_func_addr("getpwnam", je_getpwnam);
	}
	struct passwd *Ret = fptr(name);
	if (Ret == NULL) {
		return NULL;
	}

	for (i = 0; i < 63 && Cache1[i]; i++) {
		if (Cache1[i] == Ret) {
			memcpy(Cache2[i], Ret, sizeof(struct passwd));
			return Cache2[i];
		}
	}

	struct passwd *Ret1 = (struct passwd*)je_malloc(sizeof(struct passwd));
	memcpy(Ret1, Ret, sizeof(struct passwd));

	Cache1[i] = Ret;
	Cache2[i] = Ret1;
	return Ret1;
}


JEMALLOC_EXPORT
struct group *je_getgrnam(const char *name)
{
	static struct group *Cache1[64] = {NULL};
	static struct group *Cache2[64] = {NULL};
	static struct group* (*fptr)(const char*) = NULL;
	int i;

	if (fptr == NULL) {
		fptr = get_func_addr("getgrnam", je_getgrnam);
	}
	struct group *Ret = fptr(name);
	if (Ret == NULL) {
		return NULL;
	}

	for (i = 0; i < 63 && Cache1[i]; i++) {
		if (Cache1[i] == Ret) {
			memcpy(Cache2[i], Ret, sizeof(struct group));
			return Cache2[i];
		}
	}

	struct group *Ret1 = (struct group*)je_malloc(sizeof(struct group));
	memcpy(Ret1, Ret, sizeof(struct group));

	Cache1[i] = Ret;
	Cache2[i] = Ret1;
	return Ret1;
}


JEMALLOC_EXPORT
int je_vasprintf(char **strp, const char *fmt, va_list ap)
{
	unsigned long long *fixes[MAX_INTERIOR];
	unsigned long long vals[MAX_INTERIOR];
	int num_fixes = fix_varg_interiors(ap, fixes, vals);

	static int (*fptr)(char**, const char*, va_list) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("vasprintf", je_vasprintf);
	}
  int ret = fptr(strp, fmt, ap);
  restore_varg(fixes, vals, num_fixes);
  return ret;
}

JEMALLOC_EXPORT
int je_asprintf (char **string_ptr, const char *format, ...)
{
  va_list arg;
  int done;
	*string_ptr = NULL;
  va_start (arg, format);
  done = je_vasprintf(string_ptr, format, arg);
  va_end (arg);

	struct obj_header *head = (struct obj_header*)(*string_ptr - OBJ_HEADER_SIZE);
	assert(is_valid_obj_header(head));
  return done;
}


//#undef __ctype_b_loc

JEMALLOC_EXPORT
const unsigned short** je____ctype_b_loc(void)
{
	static __thread unsigned short **retptr = NULL;
	if (retptr != NULL) {
		return (const unsigned short**)retptr;
	}
	retptr = (unsigned short**)je_malloc(sizeof(unsigned short*));
	assert(retptr);

	unsigned short *ret;
	size_t size = sizeof(unsigned short) * 384;
	ret = (unsigned short*)je_malloc(size);
	assert(ret);

	unsigned short **orig = (unsigned short**)__ctype_b_loc();
	memcpy(ret, orig[0]-128, size);
	unsigned short *reti = ret + 128;
	retptr[0] = (unsigned short*)get_interior((size_t)reti, 128 * (sizeof(unsigned short)));
	return (const unsigned short**)retptr;
}


//#undef __ctype_toupper_loc

JEMALLOC_EXPORT
const __int32_t** je____ctype_toupper_loc(void)
{
	static __thread int **retptr = NULL;
	if (retptr != NULL) {
		return (const int**)retptr;
	}
	retptr = (int**)je_malloc(sizeof(int*));
	assert(retptr);

	int *ret;
	size_t size = sizeof(int) * 384;
	ret = (int*)je_malloc(size);
	assert(ret);

	const int **orig = __ctype_toupper_loc();
	memcpy(ret, orig[0]-128, size);
	int *reti = ret + 128;
	retptr[0] = (int*)get_interior((size_t)reti, 128 * sizeof(int));
	return (const int**)retptr;
}


//#undef __ctype_tolower_loc

JEMALLOC_EXPORT
const __int32_t** je____ctype_tolower_loc(void)
{
	static __thread int **retptr = NULL;
	if (retptr != NULL) {
		return (const int**)retptr;
	}
	retptr = (int**)je_malloc(sizeof(int*));
	assert(retptr);

	int *ret;
	size_t size = sizeof(int) * 384;
	ret = (int*)je_malloc(size);
	assert(ret);

	const int **orig = __ctype_tolower_loc();
	memcpy(ret, orig[0]-128, size);
	int *reti = ret + 128;
	retptr[0] = (int*)get_interior((size_t)reti, 128*sizeof(int));
	return (const int**)retptr;
}

JEMALLOC_EXPORT
int je_vfprintf(FILE *stream, const char *fmt, va_list ap)
{
	unsigned long long *fixes[MAX_INTERIOR];
	unsigned long long vals[MAX_INTERIOR];
	int num_fixes = fix_varg_interiors(ap, fixes, vals);
	static int (*fptr)(FILE*, const char*, va_list) = NULL;

	if (fptr == NULL) {
		fptr = get_func_addr("vfprintf", je_vfprintf);
	}
	int ret = fptr(stream, fmt, ap);
	restore_varg(fixes, vals, num_fixes);
	return ret;
}


#if 0
JEMALLOC_EXPORT
int je___xstat64(int vers, const char *file, struct stat64 *buf)
{
	static int (*fptr)(int, const char*, struct stat64*) = NULL;

	const char *path = getenv("PATH");
		//malloc_printf("file:%s path:%s cur:%s\n", file, path, get_current_dir_name());
	if (fptr == NULL) {
		fptr = get_func_addr("__xstat64", je___xstat64);
		assert(fptr);
	}
	int ret = fptr(vers, file, buf);
	if (ret < 0) {
		//perror("");
		//assert(0);
	}
	return ret;
}
#endif

JEMALLOC_EXPORT
int je_vsnprintf(char *s, size_t n, const char *fmt, va_list ap)
{
	unsigned long long *fixes[MAX_INTERIOR];
	unsigned long long vals[MAX_INTERIOR];
	int num_fixes = fix_varg_interiors(ap, fixes, vals);
	static int (*fptr)(char*, size_t, const char*, va_list) = NULL;

	if (fptr == NULL) {
		fptr = get_func_addr("vsnprintf", je_vsnprintf);
	}
	int ret = fptr(s, n, fmt, ap);
	restore_varg(fixes, vals, num_fixes);
	return ret;
}

JEMALLOC_EXPORT
void je_obstack_free(struct obstack *h, void *_obj) {
	char *obj = (char*)UNMASK(_obj);
  register struct _obstack_chunk *lp;	/* below addr of any objects in this chunk */
  register struct _obstack_chunk *plp;	/* point to previous chunk if any */

  lp = (struct _obstack_chunk*)UNMASK(h->chunk);
  /* We use >= because there cannot be an object at the beginning of a chunk.
     But there can be an empty object at that address
     at the end of another chunk.  */
  while (lp != 0)
    {
			char *limit = UNMASK(lp->limit);
			if (!((char*)lp >= obj || limit < obj)) {
				break;
			}
      plp = (struct _obstack_chunk*)UNMASK(lp->prev);
      CALL_FREEFUN (h, lp);
      lp = plp;
      /* If we switch chunks, we can't tell whether the new current
	 chunk contains an empty object, so assume that it may.  */
      h->maybe_empty_object = 1;
    }
  if (lp)
    {
      h->object_base = h->next_free = ptr_to_iptr(obj);
      h->chunk_limit = ptr_to_iptr(lp->limit);
      h->chunk = lp;
    }
  else if (obj != 0)
    /* obj is not in any of the chunks! */
    abort ();
}

JEMALLOC_EXPORT
int je__obstack_memory_used (struct obstack *h)
{
  register struct _obstack_chunk* lp;
  register int nbytes = 0;

  for (lp = h->chunk; lp != 0; lp = lp->prev)
    {
			lp = (struct _obstack_chunk*)UNMASK(lp);
      nbytes += UNMASK(lp->limit) - (char *) lp;
    }
  return nbytes;
}


JEMALLOC_EXPORT
void je__obstack_newchunk(struct obstack *h, int length) {
  struct _obstack_chunk *old_chunk = (struct _obstack_chunk*)UNMASK(h->chunk);
	h->chunk = old_chunk;
	old_chunk->limit = UNMASK(old_chunk->limit);

	h->next_free = UNMASK(h->next_free);
	h->object_base = UNMASK(h->object_base);
	h->chunk_limit = UNMASK(h->chunk_limit);
	static void (*fptr)(struct obstack *, int) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("_obstack_newchunk", je__obstack_newchunk);
	}
	fptr(h, length);
	h->next_free = ptr_to_iptr(h->next_free);
	h->object_base = ptr_to_iptr(h->object_base);
	h->chunk_limit = ptr_to_iptr(h->chunk_limit);
	h->chunk->limit = ptr_to_iptr(h->chunk->limit);
	if (h->chunk->prev == old_chunk) {
		old_chunk->limit = ptr_to_iptr(old_chunk->limit);
	}
	return;

#if 0
  register struct _obstack_chunk *old_chunk = (struct _obstack_chunk*)UNMASK(h->chunk);
  register struct _obstack_chunk *new_chunk;
  register long new_size;
	char *object_base = UNMASK(h->object_base);
	char *next_free = UNMASK(h->next_free);
  register long obj_size = next_free - object_base + OBJ_HEADER_SIZE;
  register long i;
	int size;
  long already;
	uint64_t mask = (1ULL << 63);

  /* Compute size for new chunk.  */
  new_size = (obj_size + length + OBJ_HEADER_SIZE) + (obj_size >> 3) + 100;
  if (new_size < h->chunk_size)
    new_size = h->chunk_size;

  /* Allocate and initialize the new chunk.  */
  new_chunk = CALL_CHUNKFUN (h, new_size);
  if (!new_chunk)
    (*obstack_alloc_failed_handler) ();
  h->chunk = new_chunk;
  new_chunk->prev = old_chunk;
  new_chunk->limit = h->chunk_limit = (char *) new_chunk + new_size;

  /* Move the existing object to the new chunk.
     Word at a time is fast and is safe if the object
     is sufficiently aligned.  */
  if (h->alignment_mask + 1 >= DEFAULT_ALIGNMENT)
    {
      for (i = obj_size / sizeof (COPYING_UNIT) - 1;
     i >= 0; i--)
  ((COPYING_UNIT *)new_chunk->contents)[i]
    = ((COPYING_UNIT *)object_base)[i];
      /* We used to copy the odd few remaining bytes as one extra COPYING_UNIT,
   but that can cross a page boundary on a machine
   which does not do strict alignment for COPYING_UNITS.  */
      already = obj_size / sizeof (COPYING_UNIT) * sizeof (COPYING_UNIT);
    }
  else
    already = 0;
  /* Copy remaining bytes one by one.  */
  for (i = already; i < obj_size; i++)
    new_chunk->contents[i] = object_base[i];

	char *old_contents = UNMASK(old_chunk->contents) + OBJ_HEADER_SIZE;
	/* If the object just copied was the only data in OLD_CHUNK,
     free that chunk and remove it from the chain.
     But not if that chunk might contain an empty object.  */
  if (object_base == old_contents && ! h->maybe_empty_object)
    {
      new_chunk->prev = old_chunk->prev;
      CALL_FREEFUN (h, old_chunk);
    }

  h->object_base = new_chunk->contents;
  h->next_free = h->object_base + obj_size;
	size = (h->chunk_limit - h->object_base) - OBJ_HEADER_SIZE;
  h->object_base = (char*)(make_obj_header((void*)(h->object_base), size));
	size = (h->chunk_limit - h->next_free) - OBJ_HEADER_SIZE;
  h->next_free = (char*)(make_obj_header((void*)(h->next_free), size));
	h->chunk_limit = new_chunk->limit = (char*)(((uint64_t)h->chunk_limit) | mask);
  /* The new chunk certainly contains no empty object yet.  */
  h->maybe_empty_object = 0;
#endif
}

struct obstack *_obstacks[1024];
static int num_obstack = 0;

#if 0
static void register_obstack(struct obstack *h) {
	assert(num_obstack < 1024);
	_obstacks[num_obstack] = h;
	num_obstack++;
}
#endif

static void print_all_obstack() {
	int i;
	for (i = 0; i < num_obstack; i++) {
		struct obstack *h = _obstacks[i];
		malloc_printf("ob%d: free:%p base:%p lim:%p\n", i, h->next_free, h->object_base, h->chunk_limit);
	}
}

JEMALLOC_EXPORT
int je__obstack_begin (struct obstack *h, int size, int alignment,
         void *(*chunkfun)(long), void (*freefun)(void *)) {

	h->next_free = UNMASK(h->next_free);
	h->object_base = UNMASK(h->object_base);
	h->chunk_limit = UNMASK(h->chunk_limit);

	static int (*fptr)(struct obstack *, int, int, void *(*chunkfun)(long), void (*freefun)(void *)) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("_obstack_begin", je__obstack_begin);
	}
	int ret = fptr(h, size, alignment, chunkfun, freefun);

	h->next_free = ptr_to_iptr(h->next_free);
	h->object_base = ptr_to_iptr(h->object_base);
	h->chunk_limit = ptr_to_iptr(h->chunk_limit);
	h->chunk->limit = ptr_to_iptr(h->chunk->limit);

	//register_obstack(h);

	//malloc_printf("begin: free:%p base:%p lim:%p\n", h->next_free, h->object_base, h->chunk_limit);

	return ret;

#if 0
	register struct _obstack_chunk *chunk; /* points to new chunk */
	uint64_t mask = 0; //(1ULL << 63);

  if (alignment == 0)
    alignment = (int) DEFAULT_ALIGNMENT;
  if (size == 0)
    /* Default size is what GNU malloc can fit in a 4096-byte block.  */
    {
      /* 12 is sizeof (mhead) and 4 is EXTRA from GNU malloc.
   Use the values for range checking, because if range checking is off,
   the extra bytes won't be missed terribly, but if range checking is on
   and we used a larger request, a whole extra 4096 bytes would be
   allocated.

   These number are irrelevant to the new GNU malloc.  I suspect it is
   less sensitive to the size of the request.  */
      int extra = ((((12 + DEFAULT_ROUNDING - 1) & ~(DEFAULT_ROUNDING - 1))
        + 4 + DEFAULT_ROUNDING - 1)
       & ~(DEFAULT_ROUNDING - 1));
      size = 4096 - extra;
    }

  h->chunkfun = (struct _obstack_chunk * (*)(void *, long)) chunkfun;
  h->freefun = (void (*) (void *, struct _obstack_chunk *)) freefun;
  h->chunk_size = size + OBJ_HEADER_SIZE;
  h->alignment_mask = alignment - 1;
  h->use_extra_arg = 0;

  chunk = h->chunk = CALL_CHUNKFUN (h, h -> chunk_size);
  if (!chunk)
    (*obstack_alloc_failed_handler) ();
	// FIXME: offset of contents
	int content_sz = size - (offsetof(struct _obstack_chunk, contents) + OBJ_HEADER_SIZE);
  h->next_free = h->object_base = (char*)(make_obj_header((void*)(chunk->contents), content_sz));
  h->chunk_limit = chunk->limit
    = (char *) chunk + h->chunk_size;
	h->chunk_limit = chunk->limit = (char*)(((uint64_t)h->chunk_limit) | mask);
  chunk->prev = 0;
  /* The initial chunk now contains no empty object.  */
  h->maybe_empty_object = 0;
  h->alloc_failed = 0;
  return 1;
#endif
}


#if 0
#include <execinfo.h>
#define BT_BUF_SIZE 100

static void myfunc3(void)
{
	int j, nptrs;
	void *buffer[BT_BUF_SIZE];
	char **strings;
	
	nptrs = backtrace(buffer, BT_BUF_SIZE);
	malloc_printf("backtrace() returned %d addresses\n", nptrs);
	
	/* The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
	   would produce similar output to the following: */
	
	strings = backtrace_symbols(buffer, nptrs);
	if (strings == NULL) {
		perror("backtrace_symbols");
		exit(EXIT_FAILURE);
	}
	
	for (j = 0; j < nptrs; j++)
		malloc_printf("%s\n", strings[j]);
	
	free(strings);
}
#endif

JEMALLOC_EXPORT
void je_san_abort2(void *base, void *cur, void *limit, size_t ptrsz, void *callsite) {
	assert(0);
	void *ptrlimit = cur + ptrsz;
	if (UNMASK(base) < (char*)0x80000000 /*|| is_stack_ptr(UNMASK(base))*/) {
		//return;
	}
	if (cur < base) {
		char *_base = (void*)UNMASK(base);
		char *_cur = (void*)UNMASK(cur);
		char *orig_base = _je_san_get_base(base) + 8;
		if (_cur < (char*)0x8000000) {
			if (!(orig_base + *((unsigned*)(orig_base-4)) >= _base)) {
				malloc_printf("orig_base:%p base:%p _base:%p _cur:%p cur:%p\n", orig_base, base, _base, _cur, cur);
				if (orig_base != _base) {
					assert(base != _base);
				}
				assert(0);
			}
		}
		else {
			if (orig_base != _base) {
				if (base == _base) {
					malloc_printf("orig_base:%p base:%p _base:%p _cur:%p cur:%p\n", orig_base, base, _base, _cur, cur);
				}
				assert(base != _base);
			}
		}
		if (orig_base && _cur >= orig_base) {
			return;
		}
	}

	void *_base = (void*)UNMASK(base);
	unsigned *head = _je_san_get_base(base);

	unsigned len = *((unsigned*)_base - 1);
	unsigned magic = *((unsigned*)_base -2);
	char *end = (char*)_base + len;
	if (trace_fp) {
		fprintf(trace_fp, "%lld base:%p cur:%p len:%d magic:%x end:%p\n"
						          "limit:%p ptrlimit:%p callsite:%p\n",
						          event_id, base, cur, len, magic, end, limit, ptrlimit, callsite);
		fprintf(trace_fp, "head:%p head0:%x head1:%x\n", head, head[0], head[1]);
	}
	else {
		malloc_printf("%lld base:%p cur:%p len:%d magic:%x end:%p\n"
									"limit:%p ptrlimit:%p callsite:%p\n",
									event_id, base, cur, len, magic, end, limit, ptrlimit, callsite);
		malloc_printf("head:%p head0:%x head1:%x\n", head, head[0], head[1]);
		myfunc3();
	}
	abort3("abort2");
}


JEMALLOC_EXPORT
void je_san_abort3(void *base, void *cur, void *limit, size_t ptrsz, void *callsite) {
	assert(0);
	void *ptrlimit = cur + ptrsz;
	if (UNMASK(base) < (char*)0x80000000 /*|| is_stack_ptr(UNMASK(base))*/) {
		//return;
	}
	if (cur < base) {
		char *_base = (void*)UNMASK(base);
		char *_cur = (void*)UNMASK(cur);
		char *orig_base = _je_san_get_base3(_base) + 8;
		if (_cur < (char*)0x8000000) {
			if (!(orig_base + *((unsigned*)(orig_base-4)) >= _base)) {
				malloc_printf("orig_base:%p base:%p _base:%p _cur:%p cur:%p\n", orig_base, base, _base, _cur, cur);
				abort3("hi");
			}
		}
		if (orig_base && _cur >= orig_base) {
			return;
		}
	}

	void *_base = (void*)UNMASK(base);
	unsigned *head = _je_san_get_base3(base);

	unsigned len = *((unsigned*)_base - 1);
	unsigned magic = *((unsigned*)_base -2);
	char *end = (char*)_base + len;
	if (trace_fp) {
		fprintf(trace_fp, "%lld base:%p cur:%p len:%d magic:%x end:%p\n"
						          "limit:%p ptrlimit:%p callsite:%p\n",
						          event_id, base, cur, len, magic, end, limit, ptrlimit, callsite);
		fprintf(trace_fp, "head:%p head0:%x head1:%x\n", head, head[0], head[1]);
	}
	else {
		malloc_printf("%lld base:%p cur:%p len:%d magic:%x end:%p\n"
									"limit:%p ptrlimit:%p callsite:%p\n",
									event_id, base, cur, len, magic, end, limit, ptrlimit, callsite);
		malloc_printf("head:%p head0:%x head1:%x\n", head, head[0], head[1]);
		myfunc3();
	}
	abort3("abort2");
}

JEMALLOC_EXPORT
void je_san_abort2_fast(void *base, void *cur, void *limit, void* callsite) {
	if (cur < base) {
		char *_cur = (void*)UNMASK(cur);
		char *orig_base = _je_san_get_base(base) + 8;
		if (orig_base && _cur >= orig_base) {
			return;
		}
	}
	malloc_printf("base:%p cur:%p limit:%p callsite:%p\n", base, cur, limit, callsite);
	abort3("abort2");
}

JEMALLOC_EXPORT
void je_san_abort3_fast(void *base, void *cur, void *limit, void *callsite) {
	if (cur < base) {
		char *_base = (void*)UNMASK(base);
		char *_cur = (void*)UNMASK(cur);
		char *orig_base = _je_san_get_base3(_base) + 8;
		if (orig_base && _cur >= orig_base) {
			return;
		}
	}
	malloc_printf("base:%p cur:%p limit:%p callsite:%p\n", base, cur, limit, callsite);
	abort3("abort3");
}


extern char** environ;

JEMALLOC_EXPORT
void* je_san_copy_env(char **env) {
	static void *env_var = NULL;
	if (env_var ) {
		return env_var;
	}
	int num_env = 0, i;
	while (env[num_env] != NULL) {
		num_env++;
	}

	int env_size = (num_env + 1) * sizeof(char*);
	char **new_env = (char**)je_malloc(env_size);
	assert(new_env);
	for (i = 0; i < num_env; i++) {
		char *e = env[i];
		int len = strlen(e);
		char *new_e = (char*)je_malloc(len+1);
		assert(new_e);
		memcpy(new_e, e, len);
		new_e[len] = '\0';
		new_env[i] = new_e;
	}
	new_env[i] = NULL;
	env_var = new_env;
	return (void*)new_env;
}


JEMALLOC_EXPORT
void je_san_enable_mask() {
	enable_masking = 1;
}

#include <signal.h>
#include <ucontext.h>



void posix_signal_handler(int sig, siginfo_t *siginfo, void *arg) {
	ucontext_t *context = (ucontext_t *)arg;
	unsigned long long *gregs = (unsigned long long*)context->uc_mcontext.gregs;
	//void *addr = siginfo->si_addr;
	//malloc_printf("addr: %p\n", addr);
  //fprintf(trace_fp, "Address from where crash happen is %llx %p %zx\n",context->uc_mcontext.gregs[REG_RIP], addr, (size_t)addr);
	//fprintf(trace_fp, "rax:%p rbx:%p\n", (void*)context->uc_mcontext.gregs[REG_RAX], (void*)context->uc_mcontext.gregs[REG_RBX]);
	//context->uc_mcontext.gregs[REG_RIP] = context->uc_mcontext.gregs[REG_RIP] + 8;
	//assert (*(unsigned char*)(context->uc_mcontext.gregs[REG_RIP]) == 0x90);
	signal_handler_invoked = true;
	EmulateInstruction(gregs[REG_RIP], gregs);
	//san_abort(siginfo->si_addr);
}

void trap_signal_handler(int sig, siginfo_t *siginfo, void *arg) {
	static void* TrapEips[64];
	static int NumTrapEips = 0;
	ucontext_t *context = (ucontext_t *)arg;
	void **stack = (void**)context->uc_mcontext.gregs[REG_RSP];
	void *rip = stack[0];
	int i;

	for (i = 0; i < NumTrapEips; i++) {
		if (TrapEips[i] == rip) {
			return;
		}
	}
	assert(NumTrapEips < 64);
	TrapEips[NumTrapEips++] = rip;

	char syscom[256];
  sprintf(syscom,"addr2line -a %p -fp -e %s", rip, program_name);
  int Ret = system(syscom);
  assert(Ret != -1);
	
}

void install_handler() {
	struct sigaction sig_action = {};
	sig_action.sa_sigaction = posix_signal_handler;
	sigemptyset(&sig_action.sa_mask);
	sig_action.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sig_action, NULL);
	sig_action.sa_sigaction = trap_signal_handler;
	sigaction(SIGTRAP, &sig_action, NULL);
}



JEMALLOC_EXPORT
void* je_san_copy_argv(int argc, char **argv) {
	enable_masking = 1;
	program_name = argv[0];
	MinLargeAddr = (void*)MAX_ULONG;
	install_handler();
	set_trace_fp();
	initialize_sections();
	//print_data_section();
	stack_initialized = 1;
	je_stack_begin = (char*)argv;
	assert(argc >= 1);
	int i;
	int argv_size = (argc + 1) * sizeof(char*);
	char **new_argv = (char**)je_malloc(argv_size);
	assert(new_argv);
	for (i = 0; i < argc; i++) {
		char *arg = argv[i];
		int len = strlen(arg);
		char *new_arg = (char*)je_malloc(len+1);
		assert(new_arg);
		memcpy(new_arg, arg, len);
		new_arg[len] = '\0';
		new_argv[i] = new_arg;
	}
	new_argv[i] = NULL;
	environ = (char**)je_san_copy_env(environ);
	return (void*)new_argv;
}


JEMALLOC_EXPORT
char *je_strstr(const char *_haystack, const char *_needle) {
	const char *haystack = (const char*)UNMASK(_haystack);
	const char *needle = (const char*)UNMASK(_needle);
  size_t len1 = strlen(haystack);
  size_t len2 = strlen(needle);
  if (len1 < len2) return NULL;
  for (size_t pos = 0; pos <= len1 - len2; pos++) {
    if (memcmp(haystack + pos, needle, len2) == 0)
      return (pos == 0) ? (char*)_haystack : (char*)je_san_interior((void*)_haystack, (void*)_haystack + pos, 1003);
  }
  return NULL;
}


#ifndef ULONG_MAX
#define	ULONG_MAX	((unsigned long)(~0L))		/* 0xFFFFFFFF */
#endif

#if 0
extern char *optarg;
extern int optind;


JEMALLOC_EXPORT
int
je_getopt(int argc, char * const argv[], const char *optstring)
{
	int ret;

	static int (*fptr)(int, char * const[], const char*) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("getopt", je_getopt);
	}
	ret = fptr(argc, argv, optstring);
	if (optarg != NULL) {
		struct obj_header *head = ((struct obj_header*)argv[optind-1]) - 1;
		malloc_printf("head:%p optarg:%p\n", head, optarg);
		assert(is_valid_obj_header(head));
		assert(optarg >= (char*)&head[1] && optarg < ((char*)(&head[1]) + head->size));
		optarg = je_san_interior1(&head[1], optarg);
	}
	return ret;
}
#endif

JEMALLOC_EXPORT
int je_san_sigaction(int signum, const struct sigaction *_act,
                     struct sigaction *_oldact)
{
	struct sigaction *act = (struct sigaction*)UNMASK(_act);
	struct sigaction *oldact = (struct sigaction*)UNMASK(_oldact);
	if (act) {
		void (*handler)(int) = (void(*)(int))UNMASK(act->sa_handler);
		void (*action)(int,siginfo_t*,void*) = 
			(void(*)(int,siginfo_t*,void*))UNMASK(act->sa_sigaction);
		void (*restorer)(void) = (void(*)(void))UNMASK(act->sa_restorer);
		act->sa_handler = handler;
		act->sa_sigaction = action;
		act->sa_restorer = restorer;
	}
	return sigaction(signum, act, oldact);
}

JEMALLOC_EXPORT
double
je_strtod(const char *_nptr, char **_endptr) {
	double ret;
	const char *nptr = (const char*)UNMASK(_nptr);
	char **endptr = (char**)UNMASK(_endptr);

	static double (*fptr)(const char*, char**) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("strtod", je_strtod);
	}
	ret = fptr(nptr, endptr);
	if (endptr) {
		if (*endptr == nptr) {
			*endptr = (char*)_nptr;
		}
		else {
			*endptr = je_san_interior1(_nptr, *endptr);
		}
	}
	return ret;
}

JEMALLOC_EXPORT
long int
je_strtol(const char *_nptr, char **_endptr, int base) {
	unsigned long ret;
	const char *nptr = (const char*)UNMASK(_nptr);
	char **endptr = (char**)UNMASK(_endptr);

	static long int (*fptr)(const char*, char**, int) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("strtol", je_strtol);
	}
	ret = fptr(nptr, endptr, base);
	if (endptr) {
		if (*endptr == nptr) {
			*endptr = (char*)_nptr;
		}
		else {
			*endptr = je_san_interior1(_nptr, *endptr);
		}
	}
	return ret;
}

JEMALLOC_EXPORT
unsigned long
je_strtoul(const char *_nptr, char **_endptr, int base) {
	unsigned long ret;
	const char *nptr = (const char*)UNMASK(_nptr);
	char **endptr = (char**)UNMASK(_endptr);

	static unsigned long (*fptr)(const char*, char**, int) = NULL;
	if (fptr == NULL) {
		fptr = get_func_addr("strtoul", je_strtoul);
	}
	ret = fptr(nptr, endptr, base);
	if (endptr) {
		if (*endptr == nptr) {
			*endptr = (char*)_nptr;
		}
		else {
			*endptr = je_san_interior1(_nptr, *endptr);
		}
	}
	return ret;
}

JEMALLOC_EXPORT
char* je_strchr(const char *_s, int c) {
	assert(!is_invalid_ptr((size_t)(_s)));

	const char *s = (const char*)UNMASK(_s);
	if (s[0] == (char)c) {
		return (char*)_s;
	}
  if (s[0] == 0) {
  	return NULL;
	}
	s++;
  while (true) {
    if (*s == (char)c)
      return (char*)(je_san_interior1(_s, s));
    if (*s == 0)
      return NULL;
    s++; 
  } 
}

JEMALLOC_EXPORT
char *
je_strtok(char *_s, const char *delim)
{
	char *s = (char*)UNMASK(_s);
  char *end;
  if (s == NULL) {
    return NULL;
	}

  if (*s == '\0')
    {
      return NULL;
    }
  /* Scan leading delimiters.  */
  s += strspn (s, delim);
  if (*s == '\0')
    {
      return NULL;
    }
  /* Find the end of the token.  */
  end = s + strcspn (s, delim);
  if (*end == '\0')
    {
      return (s == _s) ? _s : je_san_interior1(_s, s);
    }
  /* Terminate the token and make *SAVE_PTR point past it.  */
  *end = '\0';
  return (s == _s) ? _s : je_san_interior1(_s, s);
}


JEMALLOC_EXPORT
int je_execv(const char *path, char *const argv[]) {

	char *targv[16];
	int i = 0, j;

	while (argv[i]) {
		assert(i < 16);
		char *val = UNMASK(argv[i]);
		if (val == NULL) {
			targv[i] = NULL;
			break;
		}
		targv[i++] = val;
	}
	assert(i < 16);
	targv[i] = NULL;

	malloc_printf("exec: %s", path);
	for (j = 0; j < i; j++) {
		malloc_printf(" %s", targv[j]);
	}
	malloc_printf("\n");

	static int (*fptr)(const char *p, char *const a[]) = NULL;

	if (fptr == NULL) {
		fptr = get_func_addr("execv", je_execv);
		assert(fptr);
	}
	return fptr(path, targv);
}

JEMALLOC_EXPORT
int je_execvp(const char *file, char *const argv[]) {
	
	char *targv[16];
	int i = 0, j;

	while (argv[i]) {
		assert(i < 16);
		char *val = UNMASK(argv[i]);
		if (val == NULL) {
			targv[i] = NULL;
			break;
		}
		targv[i++] = val;
	}
	assert(i < 16);
	targv[i] = NULL;


	malloc_printf("exec: %s", file);
	for (j = 0; j < i; j++) {
		malloc_printf(" %s", targv[j]);
	}
	malloc_printf("\n");

	static int (*fptr)(const char *p, char *const a[]) = NULL;

	if (fptr == NULL) {
		fptr = get_func_addr("execvp", je_execvp);
		assert(fptr);
	}
	return fptr(file, targv);
}

JEMALLOC_EXPORT
int
je_putenv(char *_name)
{
	char *name = (char*)UNMASK(_name);
	static int (*fptr)(char*) = NULL;
	int i = 0;
	char *val;

	do {
		val = UNMASK(environ[i]);
		environ[i] = val;
		i++;
	} while (val);

	//malloc_printf("putenv:%s\n", name);

	if (fptr == NULL) {
		fptr = get_func_addr("putenv", je_putenv);
	}
	return fptr(name);
}

#define GET_INTERIOR(x, y) ((x == NULL || ((void*)(x) == (void*)(y))) ? (void*)(x) : (void*)je_san_interior1(y, x))

JEMALLOC_EXPORT
void *
je_memchr(void const *_s, int c_in, size_t n)
{
	void const *s = (void const*)UNMASK(_s);
  typedef unsigned long int longword;
  const unsigned char *char_ptr;
  const longword *longword_ptr;
  longword repeated_one;
  longword repeated_c;
  unsigned char c;

  c = (unsigned char) c_in;

  for (char_ptr = (const unsigned char *) s;
       n > 0 && (size_t) char_ptr % sizeof (longword) != 0;
       --n, ++char_ptr)
    if (*char_ptr == c)
      return GET_INTERIOR(char_ptr, _s);
  longword_ptr = (const longword *) char_ptr;
  repeated_one = 0x01010101;
  repeated_c = c | (c << 8);
  repeated_c |= repeated_c << 16;
  if (0xffffffffU < (longword) -1)
    {
      repeated_one |= repeated_one << 31 << 1;
      repeated_c |= repeated_c << 31 << 1;
      if (8 < sizeof (longword))
        {
          size_t i;
          for (i = 64; i < sizeof (longword) * 8; i *= 2)
            {
              repeated_one |= repeated_one << i;
              repeated_c |= repeated_c << i;
            }
        }
    }

  while (n >= sizeof (longword))
    {
      longword longword1 = *longword_ptr ^ repeated_c;
      if ((((longword1 - repeated_one) & ~longword1)
           & (repeated_one << 7)) != 0)
        break;
      longword_ptr++;
      n -= sizeof (longword);
    }
  char_ptr = (const unsigned char *) longword_ptr;

  for (; n > 0; --n, ++char_ptr)
    {
      if (*char_ptr == c)
      	return GET_INTERIOR(char_ptr, _s);
    }
  return NULL;
}

JEMALLOC_EXPORT
char *je_strrchr(const char *_s, int c) {
  const char *res = NULL;
	const char *s = (const char*)UNMASK(_s);
  for (int i = 0; s[i]; i++) {
    if (s[i] == c) res = s + i;
  }
  return (res == s) ? (char*)(_s) : (char *)(je_san_interior1(_s, res));
}

JEMALLOC_EXPORT
char *je_strncat(char *_dst, const char *_src, size_t n) {
	char *dst = (char*)UNMASK(_dst);
	const char *src = (const char*)UNMASK(_src);
  size_t len = strlen(dst);
  size_t i;
  for (i = 0; i < n && src[i]; i++)
    dst[len + i] = src[i];
  dst[len + i] = 0;
  return _dst;
}

JEMALLOC_EXPORT
char *je_strcat(char *_dst, const char *_src) {
	char *dst = (char*)UNMASK(_dst);
	const char *src = (const char*)UNMASK(_src);
  size_t len = strlen(dst);
  int i;
  for (i = 0; src[i]; i++)
    dst[len + i] = src[i];
  dst[len + i] = 0;
  return _dst;
}

#include "qsort.c"



JEMALLOC_EXPORT int JEMALLOC_NOTHROW
JEMALLOC_ATTR(nonnull(1))
je_posix_memalign(void **memptr, size_t alignment, size_t _size) {
	memptr[0] = je_aligned_alloc(alignment, _size);
	return 0;

#if 0
	assert(0);
	int ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.posix_memalign.entry", "mem ptr: %p, alignment: %zu, "
	    "size: %zu", memptr, alignment, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.bump_empty_aligned_alloc = true;
	sopts.min_alignment = sizeof(void *);
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = memptr;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	ret = imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {(uintptr_t)memptr, (uintptr_t)alignment,
			(uintptr_t)size};
		hook_invoke_alloc(hook_alloc_posix_memalign, *memptr,
		    (uintptr_t)ret, args);
	}

	LOG("core.posix_memalign.exit", "result: %d, alloc ptr: %p", ret,
	    *memptr);
	return ret;
#endif
}





JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(2)
je_aligned_alloc(size_t alignment, size_t _size) {
	if (alignment == 8 || alignment == 4 || alignment <= 2) {
		return je_malloc(_size);
	}
	print_allocations(_size);
	size_t size = _size + ALIGN_PAD(alignment);
	size_t offset;

	char *ret;

	if (_size < MAX_OFFSET) {
		ret = _je_malloc(size + OBJ_HEADER_SIZE);
	}
	else {
		ret = san_largealloc(size + OBJ_HEADER_SIZE);
	}
	assert(ret);
	add_large_pointer(ret, size + OBJ_HEADER_SIZE);
	ret = make_obj_header(ret, size, 0, 0);



	char *head = ret;
	ret = JE_ALIGN(ret, alignment);
	offset = ret - head;
	struct obj_header *header = ((struct obj_header*)head) - 1;

	if (offset == 0) {
		header->size = _size;
		return ret;
	}
	if (offset > OBJ_HEADER_SIZE) {
		assert(offset >= OBJ_HEADER_SIZE * 2);
		struct obj_header *head2 = ((struct obj_header*)ret) - 2;
		head2->offset = offset - OBJ_HEADER_SIZE;
		head2->magic = 0;
	}
	assert(offset >= OBJ_HEADER_SIZE);
	header->offset = offset;
	ret -= OBJ_HEADER_SIZE;
	//remove_faulty_pages((size_t)ret);
	ret = make_obj_header(ret, _size, 0, 1);

	if (can_print_in_trace_fp()) {
		static int reentry = 0;
		if (reentry == 0) {
			reentry = 1;
			fprintf(trace_fp, "%s(): ptr:%p, size:%zd\n", __func__, ret, size);
			reentry = 0;
		}
	}

	return ret;

#if 0
	assert(0);
	void *ret;

	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.aligned_alloc.entry", "alignment: %zu, size: %zu\n",
	    alignment, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.bump_empty_aligned_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.min_alignment = 1;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {(uintptr_t)alignment, (uintptr_t)size};
		hook_invoke_alloc(hook_alloc_aligned_alloc, ret,
		    (uintptr_t)ret, args);
	}

	LOG("core.aligned_alloc.exit", "result: %p", ret);

	return ret;
#endif
}


JEMALLOC_EXPORT
void *
je_san_alloca(size_t size, size_t alignment)
{
	void *ret = je_aligned_alloc(alignment, size);
	//malloc_printf("dalloca: %p\n", ret);
	return ret;
}

JEMALLOC_EXPORT
void
je_san_alloca_free(void *ptr)
{
	//malloc_printf("dfree: %p\n", ptr);
	je_free(ptr);
}

#define MAX_DALLOCA 1024
static __thread void* obj_arr[MAX_DALLOCA];
static __thread int num_obj_arr = 0;


JEMALLOC_EXPORT int
je_san_dalloca_register()
{
	return num_obj_arr;
}

JEMALLOC_EXPORT
void *
je_san_dalloca(size_t size, size_t alignment)
{
	void *ret = je_aligned_alloc(alignment, size);
	//malloc_printf("dalloca: %p\n", ret);
	assert(num_obj_arr < MAX_DALLOCA);
	obj_arr[num_obj_arr++] = ret;
	return ret;
}

JEMALLOC_EXPORT
void
je_san_dalloca_deregister(int num_elem)
{
	int i;
	for (i = num_elem; i < num_obj_arr; i++) {
		//malloc_printf("dfreea: %p\n", obj_arr[i]);
		je_free(obj_arr[i]);
	}
	num_obj_arr = num_elem;
}


#if 0
static JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE2(1, 2)
_je_calloc(size_t num, size_t size) {
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.calloc.entry", "num: %zu, size: %zu\n", num, size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.may_overflow = true;
	sopts.null_out_result_on_error = true;
	sopts.set_errno_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in calloc(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = num;
	dopts.item_size = size;
	dopts.zero = true;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {(uintptr_t)num, (uintptr_t)size};
		hook_invoke_alloc(hook_alloc_calloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.calloc.exit", "result: %p", ret);

	return ret;
}
#endif

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE2(1, 2)
je_calloc(size_t num, size_t size) {
	size_t total_size = num * size;
	void *ret;
	print_allocations(total_size);
	if (total_size < MAX_OFFSET) {
		ret = _je_malloc(total_size + OBJ_HEADER_SIZE);
	}
	else {
		ret = san_largealloc(total_size + OBJ_HEADER_SIZE);
	}
	if (ret) {
		add_large_pointer(ret, total_size + OBJ_HEADER_SIZE);
		//remove_faulty_pages((size_t)ret);
		ret = make_obj_header(ret, total_size, 0, 0);
		bzero(ret, total_size);
	}
	return ret;
}

static void *
irealloc_prof_sample(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t usize,
    prof_tctx_t *tctx, hook_ralloc_args_t *hook_args) {
	void *p;

	if (tctx == NULL) {
		return NULL;
	}
	if (usize <= SC_SMALL_MAXCLASS) {
		p = iralloc(tsd, old_ptr, old_usize,
		    SC_LARGE_MINCLASS, 0, false, hook_args);
		if (p == NULL) {
			return NULL;
		}
		arena_prof_promote(tsd_tsdn(tsd), p, usize);
	} else {
		p = iralloc(tsd, old_ptr, old_usize, usize, 0, false,
		    hook_args);
	}

	return p;
}

JEMALLOC_ALWAYS_INLINE void *
irealloc_prof(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t usize,
   alloc_ctx_t *alloc_ctx, hook_ralloc_args_t *hook_args) {
	void *p;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(tsd_tsdn(tsd), old_ptr, alloc_ctx);
	tctx = prof_alloc_prep(tsd, usize, prof_active, true);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		p = irealloc_prof_sample(tsd, old_ptr, old_usize, usize, tctx,
		    hook_args);
	} else {
		p = iralloc(tsd, old_ptr, old_usize, usize, 0, false,
		    hook_args);
	}
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, true);
		return NULL;
	}
	prof_realloc(tsd, p, usize, tctx, prof_active, true, old_ptr, old_usize,
	    old_tctx);

	return p;
}

JEMALLOC_ALWAYS_INLINE void
ifree(tsd_t *tsd, void *ptr, tcache_t *tcache, bool slow_path) {
	if (!slow_path) {
		tsd_assert_fast(tsd);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));
	if (tsd_reentrancy_level_get(tsd) != 0) {
		assert(slow_path);
	}

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	alloc_ctx_t alloc_ctx;
	rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
	rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
	assert(alloc_ctx.szind != SC_NSIZES);

	size_t usize;
	if (config_prof && opt_prof) {
		usize = sz_index2size(alloc_ctx.szind);
		prof_free(tsd, ptr, usize, &alloc_ctx);
	} else if (config_stats) {
		usize = sz_index2size(alloc_ctx.szind);
	}
	if (config_stats) {
		*tsd_thread_deallocatedp_get(tsd) += usize;
	}

	if (likely(!slow_path)) {
		idalloctm(tsd_tsdn(tsd), ptr, tcache, &alloc_ctx, false,
		    false);
	} else {
		idalloctm(tsd_tsdn(tsd), ptr, tcache, &alloc_ctx, false,
		    true);
	}
}

JEMALLOC_ALWAYS_INLINE void
isfree(tsd_t *tsd, void *ptr, size_t usize, tcache_t *tcache, bool slow_path) {
	if (!slow_path) {
		tsd_assert_fast(tsd);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));
	if (tsd_reentrancy_level_get(tsd) != 0) {
		assert(slow_path);
	}

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	alloc_ctx_t alloc_ctx, *ctx;
	if (!config_cache_oblivious && ((uintptr_t)ptr & PAGE_MASK) != 0) {
		/*
		 * When cache_oblivious is disabled and ptr is not page aligned,
		 * the allocation was not sampled -- usize can be used to
		 * determine szind directly.
		 */
		alloc_ctx.szind = sz_size2index(usize);
		alloc_ctx.slab = true;
		ctx = &alloc_ctx;
		if (config_debug) {
			alloc_ctx_t dbg_ctx;
			rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
			rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree,
			    rtree_ctx, (uintptr_t)ptr, true, &dbg_ctx.szind,
			    &dbg_ctx.slab);
			assert(dbg_ctx.szind == alloc_ctx.szind);
			assert(dbg_ctx.slab == alloc_ctx.slab);
		}
	} else if (config_prof && opt_prof) {
		rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
		rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
		    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
		assert(alloc_ctx.szind == sz_size2index(usize));
		ctx = &alloc_ctx;
	} else {
		ctx = NULL;
	}

	if (config_prof && opt_prof) {
		prof_free(tsd, ptr, usize, ctx);
	}
	if (config_stats) {
		*tsd_thread_deallocatedp_get(tsd) += usize;
	}

	if (likely(!slow_path)) {
		isdalloct(tsd_tsdn(tsd), ptr, usize, tcache, ctx, false);
	} else {
		isdalloct(tsd_tsdn(tsd), ptr, usize, tcache, ctx, true);
	}
}

static JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
_je_realloc(void *ptr, size_t arg_size) {
	void *ret;
	tsdn_t *tsdn JEMALLOC_CC_SILENCE_INIT(NULL);
	size_t usize JEMALLOC_CC_SILENCE_INIT(0);
	size_t old_usize = 0;
	size_t size = arg_size;

	LOG("core.realloc.entry", "ptr: %p, size: %zu\n", ptr, size);

	if (unlikely(size == 0)) {
		if (ptr != NULL) {
			/* realloc(ptr, 0) is equivalent to free(ptr). */
			UTRACE(ptr, 0, 0);
			tcache_t *tcache;
			tsd_t *tsd = tsd_fetch();
			if (tsd_reentrancy_level_get(tsd) == 0) {
				tcache = tcache_get(tsd);
			} else {
				tcache = NULL;
			}

			uintptr_t args[3] = {(uintptr_t)ptr, size};
			hook_invoke_dalloc(hook_dalloc_realloc, ptr, args);

			ifree(tsd, ptr, tcache, true);

			LOG("core.realloc.exit", "result: %p", NULL);
			return NULL;
		}
		size = 1;
	}

	if (likely(ptr != NULL)) {
		assert(malloc_initialized() || IS_INITIALIZER);
		tsd_t *tsd = tsd_fetch();

		check_entry_exit_locking(tsd_tsdn(tsd));


		hook_ralloc_args_t hook_args = {true, {(uintptr_t)ptr,
			(uintptr_t)arg_size, 0, 0}};

		alloc_ctx_t alloc_ctx;
		rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
		rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
		    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
		assert(alloc_ctx.szind != SC_NSIZES);
		old_usize = sz_index2size(alloc_ctx.szind);
		assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));
		if (config_prof && opt_prof) {
			usize = sz_s2u(size);
			if (unlikely(usize == 0
			    || usize > SC_LARGE_MAXCLASS)) {
				ret = NULL;
			} else {
				ret = irealloc_prof(tsd, ptr, old_usize, usize,
				    &alloc_ctx, &hook_args);
			}
		} else {
			if (config_stats) {
				usize = sz_s2u(size);
			}
			ret = iralloc(tsd, ptr, old_usize, size, 0, false,
			    &hook_args);
		}
		tsdn = tsd_tsdn(tsd);
	} else {
		/* realloc(NULL, size) is equivalent to malloc(size). */
		static_opts_t sopts;
		dynamic_opts_t dopts;

		static_opts_init(&sopts);
		dynamic_opts_init(&dopts);

		sopts.null_out_result_on_error = true;
		sopts.set_errno_on_error = true;
		sopts.oom_string =
		    "<jemalloc>: Error in realloc(): out of memory\n";

		dopts.result = &ret;
		dopts.num_items = 1;
		dopts.item_size = size;

		imalloc(&sopts, &dopts);
		if (sopts.slow) {
			uintptr_t args[3] = {(uintptr_t)ptr, arg_size};
			hook_invoke_alloc(hook_alloc_realloc, ret,
			    (uintptr_t)ret, args);
		}

		return ret;
	}

	if (unlikely(ret == NULL)) {
		if (config_xmalloc && unlikely(opt_xmalloc)) {
			malloc_write("<jemalloc>: Error in realloc(): "
			    "out of memory\n");
			abort();
		}
		set_errno(ENOMEM);
	}
	if (config_stats && likely(ret != NULL)) {
		tsd_t *tsd;

		assert(usize == isalloc(tsdn, ret));
		tsd = tsdn_tsd(tsdn);
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
	UTRACE(ptr, size, ret);
	check_entry_exit_locking(tsdn);

	LOG("core.realloc.exit", "result: %p", ret);
	return ret;
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
je_realloc(void *_ptr, size_t arg_size) {
	void *ptr = UNMASK(_ptr);
	struct obj_header *head = NULL;
	size_t sz = 0;
	print_allocations(arg_size);
	if (ptr) {
		head = (struct obj_header*)(ptr - OBJ_HEADER_SIZE);
		assert(head);
		if (!is_valid_obj_header(head)) {
			head -= 1;
			assert(is_valid_obj_header(head));
		}
		sz = head->size;
		assert(ptr == (void*)(&head[1]));
		remove_from_san_hash(&head[1]);
	}

	void *newptr;
	if (arg_size >= MAX_OFFSET && sz >= MAX_OFFSET) {
		newptr = san_largerealloc(head, sz + OBJ_HEADER_SIZE, arg_size + OBJ_HEADER_SIZE);
	}
	else if (arg_size >= MAX_OFFSET || sz >= MAX_OFFSET) {
		newptr = je_malloc(arg_size);
		if (ptr) {
			if (arg_size < sz) {
				sz = arg_size;
			}
			memcpy(newptr, ptr, sz);
			je_free(_ptr);
		}
		return newptr;
	}
	else {
		if (sz) {
			remove_large_pointer(head, sz + OBJ_HEADER_SIZE);
		}
		newptr = _je_realloc(head, arg_size + OBJ_HEADER_SIZE);
 	}
	
	add_large_pointer(newptr, arg_size + OBJ_HEADER_SIZE);
	//remove_faulty_pages((size_t)newptr);
	return make_obj_header(newptr, arg_size, 0, 0);
}

JEMALLOC_NOINLINE
void
free_default(void *ptr) {
	UTRACE(ptr, 0, 0);
	if (likely(ptr != NULL)) {
		/*
		 * We avoid setting up tsd fully (e.g. tcache, arena binding)
		 * based on only free() calls -- other activities trigger the
		 * minimal to full transition.  This is because free() may
		 * happen during thread shutdown after tls deallocation: if a
		 * thread never had any malloc activities until then, a
		 * fully-setup tsd won't be destructed properly.
		 */
		tsd_t *tsd = tsd_fetch_min();
		check_entry_exit_locking(tsd_tsdn(tsd));

		tcache_t *tcache;
		if (likely(tsd_fast(tsd))) {
			tsd_assert_fast(tsd);
			/* Unconditionally get tcache ptr on fast path. */
			tcache = tsd_tcachep_get(tsd);
			ifree(tsd, ptr, tcache, false);
		} else {
			if (likely(tsd_reentrancy_level_get(tsd) == 0)) {
				tcache = tcache_get(tsd);
			} else {
				tcache = NULL;
			}
			uintptr_t args_raw[3] = {(uintptr_t)ptr};
			hook_invoke_dalloc(hook_dalloc_free, ptr, args_raw);
			ifree(tsd, ptr, tcache, true);
		}
		check_entry_exit_locking(tsd_tsdn(tsd));
	}
}

JEMALLOC_ALWAYS_INLINE
bool free_fastpath(void *ptr, size_t size, bool size_hint) {
	tsd_t *tsd = tsd_get(false);
	if (unlikely(!tsd || !tsd_fast(tsd))) {
		return false;
	}

	tcache_t *tcache = tsd_tcachep_get(tsd);

	alloc_ctx_t alloc_ctx;
	/*
	 * If !config_cache_oblivious, we can check PAGE alignment to
	 * detect sampled objects.  Otherwise addresses are
	 * randomized, and we have to look it up in the rtree anyway.
	 * See also isfree().
	 */
	if (!size_hint || config_cache_oblivious) {
		rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
		bool res = rtree_szind_slab_read_fast(tsd_tsdn(tsd), &extents_rtree,
						      rtree_ctx, (uintptr_t)ptr,
						      &alloc_ctx.szind, &alloc_ctx.slab);

		/* Note: profiled objects will have alloc_ctx.slab set */
		if (!res || !alloc_ctx.slab) {
			return false;
		}
		assert(alloc_ctx.szind != SC_NSIZES);
	} else {
		/*
		 * Check for both sizes that are too large, and for sampled objects.
		 * Sampled objects are always page-aligned.  The sampled object check
		 * will also check for null ptr.
		 */
		if (size > SC_LOOKUP_MAXCLASS || (((uintptr_t)ptr & PAGE_MASK) == 0)) {
			return false;
		}
		alloc_ctx.szind = sz_size2index_lookup(size);
	}

	if (unlikely(ticker_trytick(&tcache->gc_ticker))) {
		return false;
	}

	cache_bin_t *bin = tcache_small_bin_get(tcache, alloc_ctx.szind);
	cache_bin_info_t *bin_info = &tcache_bin_info[alloc_ctx.szind];
	if (!cache_bin_dalloc_easy(bin, bin_info, ptr)) {
		return false;
	}

	if (config_stats) {
		size_t usize = sz_index2size(alloc_ctx.szind);
		*tsd_thread_deallocatedp_get(tsd) += usize;
	}

	return true;
}

static void JEMALLOC_NOTHROW
_je_free(void *ptr) {
	LOG("core.free.entry", "ptr: %p", ptr);

	if (!free_fastpath(ptr, 0, false)) {
		free_default(ptr);
	}

	LOG("core.free.exit", "");
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_free(void *_ptr) {
	void *ptr = (void*)UNMASK(_ptr);
	if (ptr == NULL) {
		return;
	}

	if (can_print_in_trace_fp()) {
		static int reentry = 0;
		if (reentry == 0) {
			reentry = 1;
			fprintf(trace_fp, "%s(): ptr:%p\n", __func__, _ptr);
			reentry = 0;
		}
	}

	struct obj_header *head = (struct obj_header*)(ptr - OBJ_HEADER_SIZE);
	if (!is_valid_obj_header(head)) {
		head -= 1;
		/*if (!is_valid_obj_header(head)) {
			malloc_printf("ptr:%p _ptr:%p\n", ptr, _ptr);
			abort();
		}*/
		assert(is_valid_obj_header(head));
	}

	remove_from_san_hash(&head[1]);

	if (head->size >= MAX_OFFSET) {
		if (!(ptr == (void*)((char*)(&head[1]) + head->offset) || ptr == (void*)(&head[2]))) {
			//malloc_printf("ptr:%p head:%p offset:%x\n", ptr, head, head->offset);
			return;
		}
		assert(ptr == (void*)((char*)(&head[1]) + head->offset) || ptr == (void*)(&head[2]));
		if (head->aligned) {
			Segment *S = ADDR_TO_SEGMENT((size_t)head);
			assert(S->MagicString == LARGE_MAGIC);
			head = (struct obj_header*)((size_t)head & S->AlignmentMask);
		}
		san_largefree(head, head->size + OBJ_HEADER_SIZE);
		return;
	}

	if (head->aligned) {
		//struct obj_header *head1 = __je_san_get_base(ptr);
		head = head - 1;
		if (head->magic == 0) {
			head = (struct obj_header*)(((char*)head) - head->offset);
		}
		//assert(head == head1);
		assert(is_valid_obj_header(head));
		assert(ptr == (void*)((char*)(&head[1]) + head->offset) || ptr == (void*)(&head[2]));
	}
	size_t size = get_size_from_obj_header(head);
	remove_large_pointer(head, size);
	_je_free(head);
}

/*
 * End malloc(3)-compatible functions.
 */
/******************************************************************************/
/*
 * Begin non-standard override functions.
 */

#ifdef JEMALLOC_OVERRIDE_MEMALIGN
JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc)
je_memalign(size_t alignment, size_t size) {
	assert(0);
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.memalign.entry", "alignment: %zu, size: %zu\n", alignment,
	    size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.min_alignment = 1;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";
	sopts.null_out_result_on_error = true;

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = alignment;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {alignment, size};
		hook_invoke_alloc(hook_alloc_memalign, ret, (uintptr_t)ret,
		    args);
	}

	LOG("core.memalign.exit", "result: %p", ret);
	return ret;
}
#endif

#ifdef JEMALLOC_OVERRIDE_VALLOC
JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc)
je_valloc(size_t size) {
	assert(0);
	void *ret;

	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.valloc.entry", "size: %zu\n", size);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.null_out_result_on_error = true;
	sopts.min_alignment = PAGE;
	sopts.oom_string =
	    "<jemalloc>: Error allocating aligned memory: out of memory\n";
	sopts.invalid_alignment_string =
	    "<jemalloc>: Error allocating aligned memory: invalid alignment\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	dopts.alignment = PAGE;

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {size};
		hook_invoke_alloc(hook_alloc_valloc, ret, (uintptr_t)ret, args);
	}

	LOG("core.valloc.exit", "result: %p\n", ret);
	return ret;
}
#endif

#if defined(JEMALLOC_IS_MALLOC) && defined(JEMALLOC_GLIBC_MALLOC_HOOK)
/*
 * glibc provides the RTLD_DEEPBIND flag for dlopen which can make it possible
 * to inconsistently reference libc's malloc(3)-compatible functions
 * (https://bugzilla.mozilla.org/show_bug.cgi?id=493541).
 *
 * These definitions interpose hooks in glibc.  The functions are actually
 * passed an extra argument for the caller return address, which will be
 * ignored.
 */
JEMALLOC_EXPORT void (*__free_hook)(void *ptr) = je_free;
JEMALLOC_EXPORT void *(*__malloc_hook)(size_t size) = je_malloc;
JEMALLOC_EXPORT void *(*__realloc_hook)(void *ptr, size_t size) = je_realloc;
#  ifdef JEMALLOC_GLIBC_MEMALIGN_HOOK
JEMALLOC_EXPORT void *(*__memalign_hook)(size_t alignment, size_t size) =
    je_memalign;
#  endif

#  ifdef CPU_COUNT
/*
 * To enable static linking with glibc, the libc specific malloc interface must
 * be implemented also, so none of glibc's malloc.o functions are added to the
 * link.
 */
#    define ALIAS(je_fn)	__attribute__((alias (#je_fn), used))
/* To force macro expansion of je_ prefix before stringification. */
#    define PREALIAS(je_fn)	ALIAS(je_fn)
#    ifdef JEMALLOC_OVERRIDE___LIBC_CALLOC
void *__libc_calloc(size_t n, size_t size) PREALIAS(je_calloc);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_FREE
void __libc_free(void* ptr) PREALIAS(je_free);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_MALLOC
void *__libc_malloc(size_t size) PREALIAS(je_malloc);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_MEMALIGN
void *__libc_memalign(size_t align, size_t s) PREALIAS(je_memalign);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_REALLOC
void *__libc_realloc(void* ptr, size_t size) PREALIAS(je_realloc);
#    endif
#    ifdef JEMALLOC_OVERRIDE___LIBC_VALLOC
void *__libc_valloc(size_t size) PREALIAS(je_valloc);
#    endif
#    ifdef JEMALLOC_OVERRIDE___POSIX_MEMALIGN
int __posix_memalign(void** r, size_t a, size_t s) PREALIAS(je_posix_memalign);
#    endif
#    undef PREALIAS
#    undef ALIAS
#  endif
#endif

/*
 * End non-standard override functions.
 */
/******************************************************************************/
/*
 * Begin non-standard functions.
 */

#ifdef JEMALLOC_EXPERIMENTAL_SMALLOCX_API

#define JEMALLOC_SMALLOCX_CONCAT_HELPER(x, y) x ## y
#define JEMALLOC_SMALLOCX_CONCAT_HELPER2(x, y)  \
  JEMALLOC_SMALLOCX_CONCAT_HELPER(x, y)

typedef struct {
	void *ptr;
	size_t size;
} smallocx_return_t;

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
smallocx_return_t JEMALLOC_NOTHROW
/*
 * The attribute JEMALLOC_ATTR(malloc) cannot be used due to:
 *  - https://gcc.gnu.org/bugzilla/show_bug.cgi?id=86488
 */
JEMALLOC_SMALLOCX_CONCAT_HELPER2(je_smallocx_, JEMALLOC_VERSION_GID_IDENT)
  (size_t size, int flags) {
	/*
	 * Note: the attribute JEMALLOC_ALLOC_SIZE(1) cannot be
	 * used here because it makes writing beyond the `size`
	 * of the `ptr` undefined behavior, but the objective
	 * of this function is to allow writing beyond `size`
	 * up to `smallocx_return_t::size`.
	 */
	smallocx_return_t ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.smallocx.entry", "size: %zu, flags: %d", size, flags);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.assert_nonempty_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in mallocx(): out of memory\n";
	sopts.usize = true;

	dopts.result = &ret.ptr;
	dopts.num_items = 1;
	dopts.item_size = size;
	if (unlikely(flags != 0)) {
		if ((flags & MALLOCX_LG_ALIGN_MASK) != 0) {
			dopts.alignment = MALLOCX_ALIGN_GET_SPECIFIED(flags);
		}

		dopts.zero = MALLOCX_ZERO_GET(flags);

		if ((flags & MALLOCX_TCACHE_MASK) != 0) {
			if ((flags & MALLOCX_TCACHE_MASK)
			    == MALLOCX_TCACHE_NONE) {
				dopts.tcache_ind = TCACHE_IND_NONE;
			} else {
				dopts.tcache_ind = MALLOCX_TCACHE_GET(flags);
			}
		} else {
			dopts.tcache_ind = TCACHE_IND_AUTOMATIC;
		}

		if ((flags & MALLOCX_ARENA_MASK) != 0)
			dopts.arena_ind = MALLOCX_ARENA_GET(flags);
	}

	imalloc(&sopts, &dopts);
	assert(dopts.usize == je_nallocx(size, flags));
	ret.size = dopts.usize;

	LOG("core.smallocx.exit", "result: %p, size: %zu", ret.ptr, ret.size);
	return ret;
}
#undef JEMALLOC_SMALLOCX_CONCAT_HELPER
#undef JEMALLOC_SMALLOCX_CONCAT_HELPER2
#endif

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ATTR(malloc) JEMALLOC_ALLOC_SIZE(1)
je_mallocx(size_t size, int flags) {
	assert(0);
	void *ret;
	static_opts_t sopts;
	dynamic_opts_t dopts;

	LOG("core.mallocx.entry", "size: %zu, flags: %d", size, flags);

	static_opts_init(&sopts);
	dynamic_opts_init(&dopts);

	sopts.assert_nonempty_alloc = true;
	sopts.null_out_result_on_error = true;
	sopts.oom_string = "<jemalloc>: Error in mallocx(): out of memory\n";

	dopts.result = &ret;
	dopts.num_items = 1;
	dopts.item_size = size;
	if (unlikely(flags != 0)) {
		if ((flags & MALLOCX_LG_ALIGN_MASK) != 0) {
			dopts.alignment = MALLOCX_ALIGN_GET_SPECIFIED(flags);
		}

		dopts.zero = MALLOCX_ZERO_GET(flags);

		if ((flags & MALLOCX_TCACHE_MASK) != 0) {
			if ((flags & MALLOCX_TCACHE_MASK)
			    == MALLOCX_TCACHE_NONE) {
				dopts.tcache_ind = TCACHE_IND_NONE;
			} else {
				dopts.tcache_ind = MALLOCX_TCACHE_GET(flags);
			}
		} else {
			dopts.tcache_ind = TCACHE_IND_AUTOMATIC;
		}

		if ((flags & MALLOCX_ARENA_MASK) != 0)
			dopts.arena_ind = MALLOCX_ARENA_GET(flags);
	}

	imalloc(&sopts, &dopts);
	if (sopts.slow) {
		uintptr_t args[3] = {size, flags};
		hook_invoke_alloc(hook_alloc_mallocx, ret, (uintptr_t)ret,
		    args);
	}

	LOG("core.mallocx.exit", "result: %p", ret);
	return ret;
}

static void *
irallocx_prof_sample(tsdn_t *tsdn, void *old_ptr, size_t old_usize,
    size_t usize, size_t alignment, bool zero, tcache_t *tcache, arena_t *arena,
    prof_tctx_t *tctx, hook_ralloc_args_t *hook_args) {
	void *p;

	if (tctx == NULL) {
		return NULL;
	}
	if (usize <= SC_SMALL_MAXCLASS) {
		p = iralloct(tsdn, old_ptr, old_usize,
		    SC_LARGE_MINCLASS, alignment, zero, tcache,
		    arena, hook_args);
		if (p == NULL) {
			return NULL;
		}
		arena_prof_promote(tsdn, p, usize);
	} else {
		p = iralloct(tsdn, old_ptr, old_usize, usize, alignment, zero,
		    tcache, arena, hook_args);
	}

	return p;
}

JEMALLOC_ALWAYS_INLINE void *
irallocx_prof(tsd_t *tsd, void *old_ptr, size_t old_usize, size_t size,
    size_t alignment, size_t *usize, bool zero, tcache_t *tcache,
    arena_t *arena, alloc_ctx_t *alloc_ctx, hook_ralloc_args_t *hook_args) {
	void *p;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(tsd_tsdn(tsd), old_ptr, alloc_ctx);
	tctx = prof_alloc_prep(tsd, *usize, prof_active, false);
	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		p = irallocx_prof_sample(tsd_tsdn(tsd), old_ptr, old_usize,
		    *usize, alignment, zero, tcache, arena, tctx, hook_args);
	} else {
		p = iralloct(tsd_tsdn(tsd), old_ptr, old_usize, size, alignment,
		    zero, tcache, arena, hook_args);
	}
	if (unlikely(p == NULL)) {
		prof_alloc_rollback(tsd, tctx, false);
		return NULL;
	}

	if (p == old_ptr && alignment != 0) {
		/*
		 * The allocation did not move, so it is possible that the size
		 * class is smaller than would guarantee the requested
		 * alignment, and that the alignment constraint was
		 * serendipitously satisfied.  Additionally, old_usize may not
		 * be the same as the current usize because of in-place large
		 * reallocation.  Therefore, query the actual value of usize.
		 */
		*usize = isalloc(tsd_tsdn(tsd), p);
	}
	prof_realloc(tsd, p, *usize, tctx, prof_active, false, old_ptr,
	    old_usize, old_tctx);

	return p;
}

JEMALLOC_EXPORT JEMALLOC_ALLOCATOR JEMALLOC_RESTRICT_RETURN
void JEMALLOC_NOTHROW *
JEMALLOC_ALLOC_SIZE(2)
je_rallocx(void *ptr, size_t size, int flags) {
	assert(0);
	void *p;
	tsd_t *tsd;
	size_t usize;
	size_t old_usize;
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	bool zero = flags & MALLOCX_ZERO;
	arena_t *arena;
	tcache_t *tcache;

	LOG("core.rallocx.entry", "ptr: %p, size: %zu, flags: %d", ptr,
	    size, flags);


	assert(ptr != NULL);
	assert(size != 0);
	assert(malloc_initialized() || IS_INITIALIZER);
	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	if (unlikely((flags & MALLOCX_ARENA_MASK) != 0)) {
		unsigned arena_ind = MALLOCX_ARENA_GET(flags);
		arena = arena_get(tsd_tsdn(tsd), arena_ind, true);
		if (unlikely(arena == NULL)) {
			goto label_oom;
		}
	} else {
		arena = NULL;
	}

	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE) {
			tcache = NULL;
		} else {
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
		}
	} else {
		tcache = tcache_get(tsd);
	}

	alloc_ctx_t alloc_ctx;
	rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
	rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
	assert(alloc_ctx.szind != SC_NSIZES);
	old_usize = sz_index2size(alloc_ctx.szind);
	assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));

	hook_ralloc_args_t hook_args = {false, {(uintptr_t)ptr, size, flags,
		0}};
	if (config_prof && opt_prof) {
		usize = (alignment == 0) ?
		    sz_s2u(size) : sz_sa2u(size, alignment);
		if (unlikely(usize == 0
		    || usize > SC_LARGE_MAXCLASS)) {
			goto label_oom;
		}
		p = irallocx_prof(tsd, ptr, old_usize, size, alignment, &usize,
		    zero, tcache, arena, &alloc_ctx, &hook_args);
		if (unlikely(p == NULL)) {
			goto label_oom;
		}
	} else {
		p = iralloct(tsd_tsdn(tsd), ptr, old_usize, size, alignment,
		    zero, tcache, arena, &hook_args);
		if (unlikely(p == NULL)) {
			goto label_oom;
		}
		if (config_stats) {
			usize = isalloc(tsd_tsdn(tsd), p);
		}
	}
	assert(alignment == 0 || ((uintptr_t)p & (alignment - 1)) == ZU(0));

	if (config_stats) {
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
	UTRACE(ptr, size, p);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.rallocx.exit", "result: %p", p);
	return p;
label_oom:
	if (config_xmalloc && unlikely(opt_xmalloc)) {
		malloc_write("<jemalloc>: Error in rallocx(): out of memory\n");
		abort();
	}
	UTRACE(ptr, size, 0);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.rallocx.exit", "result: %p", NULL);
	return NULL;
}

JEMALLOC_ALWAYS_INLINE size_t
ixallocx_helper(tsdn_t *tsdn, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero) {
	size_t newsize;

	if (ixalloc(tsdn, ptr, old_usize, size, extra, alignment, zero,
	    &newsize)) {
		return old_usize;
	}

	return newsize;
}

static size_t
ixallocx_prof_sample(tsdn_t *tsdn, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero, prof_tctx_t *tctx) {
	size_t usize;

	if (tctx == NULL) {
		return old_usize;
	}
	usize = ixallocx_helper(tsdn, ptr, old_usize, size, extra, alignment,
	    zero);

	return usize;
}

JEMALLOC_ALWAYS_INLINE size_t
ixallocx_prof(tsd_t *tsd, void *ptr, size_t old_usize, size_t size,
    size_t extra, size_t alignment, bool zero, alloc_ctx_t *alloc_ctx) {
	size_t usize_max, usize;
	bool prof_active;
	prof_tctx_t *old_tctx, *tctx;

	prof_active = prof_active_get_unlocked();
	old_tctx = prof_tctx_get(tsd_tsdn(tsd), ptr, alloc_ctx);
	/*
	 * usize isn't knowable before ixalloc() returns when extra is non-zero.
	 * Therefore, compute its maximum possible value and use that in
	 * prof_alloc_prep() to decide whether to capture a backtrace.
	 * prof_realloc() will use the actual usize to decide whether to sample.
	 */
	if (alignment == 0) {
		usize_max = sz_s2u(size+extra);
		assert(usize_max > 0
		    && usize_max <= SC_LARGE_MAXCLASS);
	} else {
		usize_max = sz_sa2u(size+extra, alignment);
		if (unlikely(usize_max == 0
		    || usize_max > SC_LARGE_MAXCLASS)) {
			/*
			 * usize_max is out of range, and chances are that
			 * allocation will fail, but use the maximum possible
			 * value and carry on with prof_alloc_prep(), just in
			 * case allocation succeeds.
			 */
			usize_max = SC_LARGE_MAXCLASS;
		}
	}
	tctx = prof_alloc_prep(tsd, usize_max, prof_active, false);

	if (unlikely((uintptr_t)tctx != (uintptr_t)1U)) {
		usize = ixallocx_prof_sample(tsd_tsdn(tsd), ptr, old_usize,
		    size, extra, alignment, zero, tctx);
	} else {
		usize = ixallocx_helper(tsd_tsdn(tsd), ptr, old_usize, size,
		    extra, alignment, zero);
	}
	if (usize == old_usize) {
		prof_alloc_rollback(tsd, tctx, false);
		return usize;
	}
	prof_realloc(tsd, ptr, usize, tctx, prof_active, false, ptr, old_usize,
	    old_tctx);

	return usize;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_xallocx(void *ptr, size_t size, size_t extra, int flags) {
	assert(0);
	tsd_t *tsd;
	size_t usize, old_usize;
	size_t alignment = MALLOCX_ALIGN_GET(flags);
	bool zero = flags & MALLOCX_ZERO;

	LOG("core.xallocx.entry", "ptr: %p, size: %zu, extra: %zu, "
	    "flags: %d", ptr, size, extra, flags);

	assert(ptr != NULL);
	assert(size != 0);
	assert(SIZE_T_MAX - size >= extra);
	assert(malloc_initialized() || IS_INITIALIZER);
	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));

	alloc_ctx_t alloc_ctx;
	rtree_ctx_t *rtree_ctx = tsd_rtree_ctx(tsd);
	rtree_szind_slab_read(tsd_tsdn(tsd), &extents_rtree, rtree_ctx,
	    (uintptr_t)ptr, true, &alloc_ctx.szind, &alloc_ctx.slab);
	assert(alloc_ctx.szind != SC_NSIZES);
	old_usize = sz_index2size(alloc_ctx.szind);
	assert(old_usize == isalloc(tsd_tsdn(tsd), ptr));
	/*
	 * The API explicitly absolves itself of protecting against (size +
	 * extra) numerical overflow, but we may need to clamp extra to avoid
	 * exceeding SC_LARGE_MAXCLASS.
	 *
	 * Ordinarily, size limit checking is handled deeper down, but here we
	 * have to check as part of (size + extra) clamping, since we need the
	 * clamped value in the above helper functions.
	 */
	if (unlikely(size > SC_LARGE_MAXCLASS)) {
		usize = old_usize;
		goto label_not_resized;
	}
	if (unlikely(SC_LARGE_MAXCLASS - size < extra)) {
		extra = SC_LARGE_MAXCLASS - size;
	}

	if (config_prof && opt_prof) {
		usize = ixallocx_prof(tsd, ptr, old_usize, size, extra,
		    alignment, zero, &alloc_ctx);
	} else {
		usize = ixallocx_helper(tsd_tsdn(tsd), ptr, old_usize, size,
		    extra, alignment, zero);
	}
	if (unlikely(usize == old_usize)) {
		goto label_not_resized;
	}

	if (config_stats) {
		*tsd_thread_allocatedp_get(tsd) += usize;
		*tsd_thread_deallocatedp_get(tsd) += old_usize;
	}
label_not_resized:
	if (unlikely(!tsd_fast(tsd))) {
		uintptr_t args[4] = {(uintptr_t)ptr, size, extra, flags};
		hook_invoke_expand(hook_expand_xallocx, ptr, old_usize,
		    usize, (uintptr_t)usize, args);
	}

	UTRACE(ptr, size, ptr);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.xallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
JEMALLOC_ATTR(pure)
je_sallocx(const void *ptr, int flags) {
	assert(0);
	size_t usize;
	tsdn_t *tsdn;

	LOG("core.sallocx.entry", "ptr: %p, flags: %d", ptr, flags);

	assert(malloc_initialized() || IS_INITIALIZER);
	assert(ptr != NULL);

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	if (config_debug || force_ivsalloc) {
		usize = ivsalloc(tsdn, ptr);
		assert(force_ivsalloc || usize != 0);
	} else {
		usize = isalloc(tsdn, ptr);
	}

	check_entry_exit_locking(tsdn);

	LOG("core.sallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_dallocx(void *ptr, int flags) {
	assert(0);
	LOG("core.dallocx.entry", "ptr: %p, flags: %d", ptr, flags);

	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	tsd_t *tsd = tsd_fetch();
	bool fast = tsd_fast(tsd);
	check_entry_exit_locking(tsd_tsdn(tsd));

	tcache_t *tcache;
	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		/* Not allowed to be reentrant and specify a custom tcache. */
		assert(tsd_reentrancy_level_get(tsd) == 0);
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE) {
			tcache = NULL;
		} else {
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
		}
	} else {
		if (likely(fast)) {
			tcache = tsd_tcachep_get(tsd);
			assert(tcache == tcache_get(tsd));
		} else {
			if (likely(tsd_reentrancy_level_get(tsd) == 0)) {
				tcache = tcache_get(tsd);
			}  else {
				tcache = NULL;
			}
		}
	}

	UTRACE(ptr, 0, 0);
	if (likely(fast)) {
		tsd_assert_fast(tsd);
		ifree(tsd, ptr, tcache, false);
	} else {
		uintptr_t args_raw[3] = {(uintptr_t)ptr, flags};
		hook_invoke_dalloc(hook_dalloc_dallocx, ptr, args_raw);
		ifree(tsd, ptr, tcache, true);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.dallocx.exit", "");
}

JEMALLOC_ALWAYS_INLINE size_t
inallocx(tsdn_t *tsdn, size_t size, int flags) {
	check_entry_exit_locking(tsdn);

	size_t usize;
	if (likely((flags & MALLOCX_LG_ALIGN_MASK) == 0)) {
		usize = sz_s2u(size);
	} else {
		usize = sz_sa2u(size, MALLOCX_ALIGN_GET_SPECIFIED(flags));
	}
	check_entry_exit_locking(tsdn);
	return usize;
}

JEMALLOC_NOINLINE void
sdallocx_default(void *ptr, size_t size, int flags) {
	assert(ptr != NULL);
	assert(malloc_initialized() || IS_INITIALIZER);

	tsd_t *tsd = tsd_fetch();
	bool fast = tsd_fast(tsd);
	size_t usize = inallocx(tsd_tsdn(tsd), size, flags);
	assert(usize == isalloc(tsd_tsdn(tsd), ptr));
	check_entry_exit_locking(tsd_tsdn(tsd));

	tcache_t *tcache;
	if (unlikely((flags & MALLOCX_TCACHE_MASK) != 0)) {
		/* Not allowed to be reentrant and specify a custom tcache. */
		assert(tsd_reentrancy_level_get(tsd) == 0);
		if ((flags & MALLOCX_TCACHE_MASK) == MALLOCX_TCACHE_NONE) {
			tcache = NULL;
		} else {
			tcache = tcaches_get(tsd, MALLOCX_TCACHE_GET(flags));
		}
	} else {
		if (likely(fast)) {
			tcache = tsd_tcachep_get(tsd);
			assert(tcache == tcache_get(tsd));
		} else {
			if (likely(tsd_reentrancy_level_get(tsd) == 0)) {
				tcache = tcache_get(tsd);
			} else {
				tcache = NULL;
			}
		}
	}

	UTRACE(ptr, 0, 0);
	if (likely(fast)) {
		tsd_assert_fast(tsd);
		isfree(tsd, ptr, usize, tcache, false);
	} else {
		uintptr_t args_raw[3] = {(uintptr_t)ptr, size, flags};
		hook_invoke_dalloc(hook_dalloc_sdallocx, ptr, args_raw);
		isfree(tsd, ptr, usize, tcache, true);
	}
	check_entry_exit_locking(tsd_tsdn(tsd));

}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_sdallocx(void *ptr, size_t size, int flags) {
	assert(0);
	LOG("core.sdallocx.entry", "ptr: %p, size: %zu, flags: %d", ptr,
		size, flags);

	if (flags !=0 || !free_fastpath(ptr, size, true)) {
		sdallocx_default(ptr, size, flags);
	}

	LOG("core.sdallocx.exit", "");
}

void JEMALLOC_NOTHROW
je_sdallocx_noflags(void *ptr, size_t size) {
	LOG("core.sdallocx.entry", "ptr: %p, size: %zu, flags: 0", ptr,
		size);

	if (!free_fastpath(ptr, size, true)) {
		sdallocx_default(ptr, size, 0);
	}

	LOG("core.sdallocx.exit", "");
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
JEMALLOC_ATTR(pure)
je_nallocx(size_t size, int flags) {
	assert(0);
	size_t usize;
	tsdn_t *tsdn;

	assert(size != 0);

	if (unlikely(malloc_init())) {
		LOG("core.nallocx.exit", "result: %zu", ZU(0));
		return 0;
	}

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	usize = inallocx(tsdn, size, flags);
	if (unlikely(usize > SC_LARGE_MAXCLASS)) {
		LOG("core.nallocx.exit", "result: %zu", ZU(0));
		return 0;
	}

	check_entry_exit_locking(tsdn);
	LOG("core.nallocx.exit", "result: %zu", usize);
	return usize;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctl(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen) {
	assert(0);
	int ret;
	tsd_t *tsd;

	LOG("core.mallctl.entry", "name: %s", name);

	if (unlikely(malloc_init())) {
		LOG("core.mallctl.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_byname(tsd, name, oldp, oldlenp, newp, newlen);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.mallctl.exit", "result: %d", ret);
	return ret;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctlnametomib(const char *name, size_t *mibp, size_t *miblenp) {
	assert(0);
	int ret;

	LOG("core.mallctlnametomib.entry", "name: %s", name);

	if (unlikely(malloc_init())) {
		LOG("core.mallctlnametomib.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd_t *tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_nametomib(tsd, name, mibp, miblenp);
	check_entry_exit_locking(tsd_tsdn(tsd));

	LOG("core.mallctlnametomib.exit", "result: %d", ret);
	return ret;
}

JEMALLOC_EXPORT int JEMALLOC_NOTHROW
je_mallctlbymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
  void *newp, size_t newlen) {
	assert(0);
	int ret;
	tsd_t *tsd;

	LOG("core.mallctlbymib.entry", "");

	if (unlikely(malloc_init())) {
		LOG("core.mallctlbymib.exit", "result: %d", EAGAIN);
		return EAGAIN;
	}

	tsd = tsd_fetch();
	check_entry_exit_locking(tsd_tsdn(tsd));
	ret = ctl_bymib(tsd, mib, miblen, oldp, oldlenp, newp, newlen);
	check_entry_exit_locking(tsd_tsdn(tsd));
	LOG("core.mallctlbymib.exit", "result: %d", ret);
	return ret;
}

JEMALLOC_EXPORT void JEMALLOC_NOTHROW
je_malloc_stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts) {
	assert(0);
	tsdn_t *tsdn;

	LOG("core.malloc_stats_print.entry", "");

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);
	stats_print(write_cb, cbopaque, opts);
	check_entry_exit_locking(tsdn);
	LOG("core.malloc_stats_print.exit", "");
}

JEMALLOC_EXPORT size_t JEMALLOC_NOTHROW
je_malloc_usable_size(JEMALLOC_USABLE_SIZE_CONST void *ptr) {
	assert(0);
	size_t ret;
	tsdn_t *tsdn;

	LOG("core.malloc_usable_size.entry", "ptr: %p", ptr);

	assert(malloc_initialized() || IS_INITIALIZER);

	tsdn = tsdn_fetch();
	check_entry_exit_locking(tsdn);

	if (unlikely(ptr == NULL)) {
		ret = 0;
	} else {
		if (config_debug || force_ivsalloc) {
			ret = ivsalloc(tsdn, ptr);
			assert(force_ivsalloc || ret != 0);
		} else {
			ret = isalloc(tsdn, ptr);
		}
	}

	check_entry_exit_locking(tsdn);
	LOG("core.malloc_usable_size.exit", "result: %zu", ret);
	return ret;
}

/*
 * End non-standard functions.
 */
/******************************************************************************/
/*
 * The following functions are used by threading libraries for protection of
 * malloc during fork().
 */

/*
 * If an application creates a thread before doing any allocation in the main
 * thread, then calls fork(2) in the main thread followed by memory allocation
 * in the child process, a race can occur that results in deadlock within the
 * child: the main thread may have forked while the created thread had
 * partially initialized the allocator.  Ordinarily jemalloc prevents
 * fork/malloc races via the following functions it registers during
 * initialization using pthread_atfork(), but of course that does no good if
 * the allocator isn't fully initialized at fork time.  The following library
 * constructor is a partial solution to this problem.  It may still be possible
 * to trigger the deadlock described above, but doing so would involve forking
 * via a library constructor that runs before jemalloc's runs.
 */
#ifndef JEMALLOC_JET
JEMALLOC_ATTR(constructor)
static void
jemalloc_constructor(void) {
	malloc_init();
}
#endif

#ifndef JEMALLOC_MUTEX_INIT_CB
void
jemalloc_prefork(void)
#else
JEMALLOC_EXPORT void
_malloc_prefork(void)
#endif
{
	tsd_t *tsd;
	unsigned i, j, narenas;
	arena_t *arena;

#ifdef JEMALLOC_MUTEX_INIT_CB
	if (!malloc_initialized()) {
		return;
	}
#endif
	assert(malloc_initialized());

	tsd = tsd_fetch();

	narenas = narenas_total_get();

	witness_prefork(tsd_witness_tsdp_get(tsd));
	/* Acquire all mutexes in a safe order. */
	ctl_prefork(tsd_tsdn(tsd));
	tcache_prefork(tsd_tsdn(tsd));
	malloc_mutex_prefork(tsd_tsdn(tsd), &arenas_lock);
	if (have_background_thread) {
		background_thread_prefork0(tsd_tsdn(tsd));
	}
	prof_prefork0(tsd_tsdn(tsd));
	if (have_background_thread) {
		background_thread_prefork1(tsd_tsdn(tsd));
	}
	/* Break arena prefork into stages to preserve lock order. */
	for (i = 0; i < 8; i++) {
		for (j = 0; j < narenas; j++) {
			if ((arena = arena_get(tsd_tsdn(tsd), j, false)) !=
			    NULL) {
				switch (i) {
				case 0:
					arena_prefork0(tsd_tsdn(tsd), arena);
					break;
				case 1:
					arena_prefork1(tsd_tsdn(tsd), arena);
					break;
				case 2:
					arena_prefork2(tsd_tsdn(tsd), arena);
					break;
				case 3:
					arena_prefork3(tsd_tsdn(tsd), arena);
					break;
				case 4:
					arena_prefork4(tsd_tsdn(tsd), arena);
					break;
				case 5:
					arena_prefork5(tsd_tsdn(tsd), arena);
					break;
				case 6:
					arena_prefork6(tsd_tsdn(tsd), arena);
					break;
				case 7:
					arena_prefork7(tsd_tsdn(tsd), arena);
					break;
				default: not_reached();
				}
			}
		}
	}
	prof_prefork1(tsd_tsdn(tsd));
	tsd_prefork(tsd);
}

#ifndef JEMALLOC_MUTEX_INIT_CB
void
jemalloc_postfork_parent(void)
#else
JEMALLOC_EXPORT void
_malloc_postfork(void)
#endif
{
	tsd_t *tsd;
	unsigned i, narenas;

#ifdef JEMALLOC_MUTEX_INIT_CB
	if (!malloc_initialized()) {
		return;
	}
#endif
	assert(malloc_initialized());

	tsd = tsd_fetch();

	tsd_postfork_parent(tsd);

	witness_postfork_parent(tsd_witness_tsdp_get(tsd));
	/* Release all mutexes, now that fork() has completed. */
	for (i = 0, narenas = narenas_total_get(); i < narenas; i++) {
		arena_t *arena;

		if ((arena = arena_get(tsd_tsdn(tsd), i, false)) != NULL) {
			arena_postfork_parent(tsd_tsdn(tsd), arena);
		}
	}
	prof_postfork_parent(tsd_tsdn(tsd));
	if (have_background_thread) {
		background_thread_postfork_parent(tsd_tsdn(tsd));
	}
	malloc_mutex_postfork_parent(tsd_tsdn(tsd), &arenas_lock);
	tcache_postfork_parent(tsd_tsdn(tsd));
	ctl_postfork_parent(tsd_tsdn(tsd));
}

void
jemalloc_postfork_child(void) {
	tsd_t *tsd;
	unsigned i, narenas;

	assert(malloc_initialized());

	tsd = tsd_fetch();

	tsd_postfork_child(tsd);

	witness_postfork_child(tsd_witness_tsdp_get(tsd));
	/* Release all mutexes, now that fork() has completed. */
	for (i = 0, narenas = narenas_total_get(); i < narenas; i++) {
		arena_t *arena;

		if ((arena = arena_get(tsd_tsdn(tsd), i, false)) != NULL) {
			arena_postfork_child(tsd_tsdn(tsd), arena);
		}
	}
	prof_postfork_child(tsd_tsdn(tsd));
	if (have_background_thread) {
		background_thread_postfork_child(tsd_tsdn(tsd));
	}
	malloc_mutex_postfork_child(tsd_tsdn(tsd), &arenas_lock);
	tcache_postfork_child(tsd_tsdn(tsd));
	ctl_postfork_child(tsd_tsdn(tsd));
}

/******************************************************************************/
