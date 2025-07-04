#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "threads/palloc.h"
#include "hash.h"

enum vm_type
{
	/* 초기화되지 않은 페이지 */
	VM_UNINIT = 0,
	/* 파일과 관련 없는 페이지, 즉 익명 페이지 */
	VM_ANON = 1,
	/* 파일과 관련된 페이지 */
	VM_FILE = 2,
	/* 페이지 캐시를 보유하는 페이지, 프로젝트 4용 */
	VM_PAGE_CACHE = 3,
	/* mmap 파일 페이지 */
	VM_MMAP = 4,

	/* 상태를 저장하기 위한 비트 플래그 */

	/* 추가 정보를 저장하기 위한 보조 비트 플래그 마커입니다.
	 * int 범위 내에서 값을 추가할 수 있습니다. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* 이 값을 초과하지 마세요. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)
#define STACK_GROW_RANGE 4192

/* "page"의 표현입니다.
 * 이것은 일종의 "부모 클래스"로, 네 개의 "자식 클래스"를 가집니다:
 * uninit_page, file_page, anon_page, 그리고 페이지 캐시(project4).
 * 이 구조체의 미리 정의된 멤버는 제거/수정하지 마세요. */
struct page
{
	const struct page_operations *operations;
	void *va;			 /* 사용자 공간 기준의 주소 */
	struct frame *frame; /* frame에 대한 역참조 */

	/* 구현 필드 */
	bool writable;
	// 매핑된 프레임이 스왑되어있는가??
	bool is_swap;

	/* 타입별 데이터는 union에 바인딩됩니다.
	 * 각 함수는 현재 union을 자동으로 감지합니다. */
	union
	{
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* The representation of "frame" */
struct frame
{
	void *kva;
	/* extra-cow */
	int ref_cnt;
	struct page *page;
	// frame_table 소속 elem
	struct list_elem elem;
};

/* 페이지 작업을 위한 함수 테이블입니다.
 * 이는 C에서 "인터페이스"를 구현하는 한 가지 방법입니다.
 * "메서드"의 테이블을 구조체의 멤버에 넣고,
 * 필요할 때마다 호출하면 됩니다. */
struct page_operations
{
	bool (*swap_in)(struct page *, void *);
	bool (*swap_out)(struct page *);
	void (*destroy)(struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in((page), v)
#define swap_out(page) (page)->operations->swap_out(page)
#define destroy(page)                \
	if ((page)->operations->destroy) \
	(page)->operations->destroy(page)

/* 현재 프로세스의 메모리 공간을 나타냅니다.
 * 이 구조체에 대해 특정 설계를 강제하지 않습니다.
 * 모든 설계는 여러분에게 달려 있습니다. */
struct supplemental_page_table
{
	struct hash SPT_hash_list;
};

struct SPT_entry
{
	/* 이 엔트리의 키 값이 될 가상 주소 */
	void *va;
	/* 페이지 */
	struct page *page;
	/* 해시 테이블 요소*/
	struct hash_elem elem;
};

/* 
 * 페이지가 lazy loading 될 때 사용될 보조 정보 구조체
 * 이 구조체는 실제 페이지 fault가 발생했을 때 필요한 파일 위치 및 읽기 정보를 포함함
 */
struct lazy_load_info {
    struct file *file;       // 읽을 파일 포인터 (reopen된 파일 핸들)
    off_t offset;            // 해당 페이지의 파일 내 시작 위치 (offset)
    size_t readbyte;         // 파일에서 읽어야 할 바이트 수
    size_t zerobyte;         // 나머지를 0으로 채워야 할 바이트 수 (ex. .bss 영역 같은 경우)
};

/*
 * mmap을 통해 매핑된 각 페이지에 대한 식별자 역할을 하는 구조체
 * - info는 해당 페이지의 lazy loading 정보를 포함
 * - mapping_count는 mmap 전체 중 몇 번째 페이지인지 나타냄
 */
struct mmap_info {
    struct lazy_load_info *info;  // 지연 로딩에 필요한 보조 정보
    int mapping_count;            // mmap된 페이지 중 순서 (ex. 0번째 페이지, 1번째 ...)
};

#include "threads/thread.h"
void supplemental_page_table_init(struct supplemental_page_table *spt);
bool supplemental_page_table_copy(struct supplemental_page_table *dst,
								  struct supplemental_page_table *src);
void supplemental_page_table_kill(struct supplemental_page_table *spt);
struct page *spt_find_page(struct supplemental_page_table *spt,
						   void *va);
bool spt_insert_page(struct supplemental_page_table *spt, struct page *page);
void spt_remove_page(struct supplemental_page_table *spt, struct page *page);

void vm_init(void);
bool vm_try_handle_fault(struct intr_frame *f, void *addr, bool user,
						 bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage,
									bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page(struct page *page);
bool vm_claim_page(void *va);
enum vm_type page_get_type(struct page *page);

void frame_table_insert(struct list_elem *elem);
struct frame *frame_table_remove(void);

struct mmap_info *make_mmap_info(struct lazy_load_info *info,
								 int mapping_count);
struct lazy_load_info *make_info(
	struct file *file, off_t offset, size_t read_byte);

bool vm_copy_claim_page(void *va, struct page *parent, struct supplemental_page_table *parent_spt);

#endif /* VM_VM_H */
