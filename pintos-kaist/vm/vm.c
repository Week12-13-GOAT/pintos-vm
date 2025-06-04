/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
// Project 3 : VM
#include "kernel/hash.h"
#include "userprog/process.h"

/* 각 서브시스템의 초기화 코드를 호출하여 가상 메모리 서브시스템을 초기화합니다. */
void vm_init(void)
{
   vm_anon_init();
   vm_file_init();
#ifdef EFILESYS /* For project 4 */
   pagecache_init();
#endif
   register_inspect_intr();
   /* 이 위쪽은 수정하지 마세요 !! */
   /* TODO: 이 아래쪽부터 코드를 추가하세요 */
}

/* 페이지의 타입을 가져옵니다. 이 함수는 페이지가 초기화된 후 타입을 알고 싶을 때 유용합니다.
 * 이 함수는 이미 완전히 구현되어 있습니다. */
enum vm_type
page_get_type(struct page *page)
{
   int ty = VM_TYPE(page->operations->type);
   switch (ty)
   {
   case VM_UNINIT:
      return VM_TYPE(page->uninit.type);
   default:
      return ty;
   }
}

/* Helpers */
static void hash_spt_entry_kill(struct hash_elem *e, void *aux);
static struct frame *vm_get_victim(void);
static bool vm_do_claim_page(struct page *page);
static struct frame *vm_evict_frame(void);
static uint64_t my_hash(const struct hash_elem *e, void *aux UNUSED);
static bool my_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);

/* 초기화 함수와 함께 대기 중인 페이지 객체를 생성합니다. 페이지를 직접 생성하지 말고,
 * 반드시 이 함수나 vm_alloc_page를 통해 생성하세요. */
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                    vm_initializer *init, void *aux)
{

   ASSERT(VM_TYPE(type) != VM_UNINIT)

   struct supplemental_page_table *spt = &thread_current()->spt;

   /* 이미 해당 page가 SPT에 존재하는지 확인합니다 */
   if (spt_find_page(spt, upage) == NULL)
   {
      /* TODO: VM 타입에 따라 페이지를 생성하고, 초기화 함수를 가져온 뒤,
       * TODO: uninit_new를 호출하여 "uninit" 페이지 구조체를 생성하세요.
       * TODO: uninit_new 호출 후에는 필요한 필드를 수정해야 합니다. */
      bool (*page_initializer)(struct page *, enum vm_type, void *kva);
      struct page *page = malloc(sizeof(struct page));

      if (page == NULL)
      {
         goto err;
      }

      switch (VM_TYPE(type))
      {
      case VM_ANON:
         page_initializer = anon_initializer;
         break;
      case VM_MMAP:
      case VM_FILE:
         page_initializer = file_backed_initializer;
         break;
      default:
         free(page);
         goto err;
         break;
      }

      uninit_new(page, upage, init, type, aux, page_initializer);
      page->writable = writable;

      /* TODO: 생성한 페이지를 spt에 삽입하세요. */
      if (!spt_insert_page(spt, page))
      {
         // 실패 시 메모리 누수 방지 위해 free
         free(page);
         // 실패 했으니까 에러로 가야겠지?
         goto err;
      }

      return true;
   }

err:
   return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/* 가상 주소를 통해 SPT에서 페이지를 찾아 리턴합니다.
 * 에러가 발생하면 NULL을 리턴하세요 */
struct page *
spt_find_page(struct supplemental_page_table *spt UNUSED, void *va UNUSED)
{
   // 더미 SPT_entry를 생성하여 va 값 기반 hash 조회
   struct page *finding_page = NULL;
   struct SPT_entry lookup;
   lookup.va = pg_round_down(va);

   // 인자는 더미 SPT_entry, 반환된 finding_hash_elem은 실제 SPT_entry 소속 hash_elem
   struct hash_elem *finding_hash_elem = hash_find(&spt->SPT_hash_list, &lookup.elem);

   // 탐색 성공 시, hash_elem로 entry 조회, page 확보
   if (finding_hash_elem != NULL)
      finding_page = hash_entry(finding_hash_elem, struct SPT_entry, elem)->page;

   return finding_page;
}

/* Insert PAGE into spt with validation. */
bool spt_insert_page(struct supplemental_page_table *spt UNUSED,
                     struct page *page UNUSED)
{
   // int succ = false;
   /* TODO: Fill this function. */

   // 예외 처리
   if (spt == NULL || page == NULL)
   {
      return false;
   }

   // 메모리 할당
   struct SPT_entry *entry = malloc(sizeof(struct SPT_entry));
   // 예외 처리
   if (entry == NULL)
   {
      return false;
   }

   entry->va = page->va; // 가상 주소 : page 구조체의 va 필드 -> SPT_entry의 va 필드
   entry->page = page;   // 페이지 참조 : page 구조체의 주소 -> SPT_entry의 page 포인터

   if (hash_insert(&spt->SPT_hash_list, &entry->elem) != NULL)
   {
      // 삽입 실패시 메모리 정리
      free(entry);
      return false; // 삽입 실패
   }

   return true; // SPT 페이지 삽입 성공
}

void spt_remove_page(struct supplemental_page_table *spt, struct page *page)
{
   struct SPT_entry lookup;
   /* 내가 찾을 페이지의 가상 주소를 넣습니다 */
   lookup.va = page->va;
   /* SPT_hash_list에서 해당 가상 주소를 가진 엔트리를 제거합니다 */
   struct hash_elem *delete_elem = hash_delete(&spt->SPT_hash_list, &lookup.elem);
   if (delete_elem == NULL)
   {
      return;
   }
   struct SPT_entry *deleted = hash_entry(delete_elem, struct SPT_entry, elem);

   /* 페이지 테이블에서 해당 가상 페이지 삭제 */
   pml4_clear_page(thread_current()->pml4, page->va);
   vm_dealloc_page(page);
   /** TODO: page 해제
    * 매핑된 프레임을 해제해야하나?
    * 프레임이 스왑되어있는지 체크할것?
    * 아마 pml4_clear_page 사용하면 된대요
    */

   /* 내부 페이지의 요소가 모두 free 된 이후 SPT_entry free */
   free(deleted);
   return true;
}

static struct frame *vm_get_victim(void)
{
   struct list_elem *clock_now;
   struct frame *victim = list_entry(list_begin(&frame_table), struct frame, elem);
   /* TODO: 교체 정책을 여기서 구현해서 희생자 페이지 찾기 */
   if (list_empty(&frame_table))
      return NULL;

   ASSERT(victim != NULL);
   // if(clock_start == NULL || clock_start == list_end(&frame_table))
   //    clock_start = list_begin(&frame_table);

   // clock_now = clock_start;
   // do{
   //    victim = list_entry(clock_now,struct frame, elem);

   //    if(victim->clock_check == false){
   //       clock_start = list_next(clock_now);
   //       return victim;
   //    }

   //    victim->clock_check = false;

   //    clock_now = list_next(clock_now);
   //    if(clock_now == list_end(&frame_table))
   //       clock_now = list_begin(&frame_table);
   // } while(clock_now != clock_start);

   // victim = list_entry(clock_now,struct frame, elem);
   // clock_start = list_next(clock_now);

   return victim;
}

/* 한 페이지를 교체(evict)하고 해당 프레임을 반환합니다.
 * 에러가 발생하면 NULL을 반환합니다.*/
static struct frame *
vm_evict_frame(void)
{
   struct frame *victim = vm_get_victim();
   if (victim == NULL)
      return NULL;
   /* TODO: swap out the victim and return the evicted frame. */

   struct page *victim_page = victim->page;
   if (victim_page == NULL)
      return NULL;
   /** TODO: 여기서 swap_out 매크로를 호출??
    *   pml4_clear_page를 아마 사용?? (잘 모름)
    */

   if (!swap_out(victim_page))
      return NULL;
   pml4_clear_page(thread_current()->pml4, victim_page->va);
   list_remove(&victim->elem);

   return victim;
}

/* palloc()을 사용하여 프레임을 할당합니다.
 * 사용 가능한 페이지가 없으면 페이지를 교체(evict)하여 반환합니다.
 * 이 함수는 항상 유효한 주소를 반환합니다. 즉, 사용자 풀 메모리가 가득 차면,
 * 이 함수는 프레임을 교체하여 사용 가능한 메모리 공간을 확보합니다.*/
static struct frame *
vm_get_frame(void)
{
   /* 반드시 free해라 뒤지기싫으면.. */
   struct frame *frame = malloc(sizeof(struct frame));
   ASSERT(frame != NULL);

   frame->kva = palloc_get_page(PAL_USER | PAL_ZERO);
   if (frame->kva == NULL)
   {
      struct frame *victim1 = vm_evict_frame();

      ASSERT(victim1 != NULL);

      frame->kva = victim1->kva; // victim의 물리 페이지를 재활용
      free(victim1);
   }
   frame->page = NULL;
   frame->ref_cnt = 1;
   frame_table_insert(&frame->elem);

   ASSERT(frame->page == NULL);
   return frame;
}

/* Growing the stack. */
static void
vm_stack_growth(void *addr UNUSED)
{
   /* 스택 최하단에 익명 페이지를 추가하여 사용
    * addr은 PGSIZE로 내림(정렬)하여 사용    */
   vm_alloc_page(VM_ANON, addr, true); // 스택 최하단에 익명 페이지 추가
   vm_claim_page(addr);
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp(struct page *page UNUSED)
{
   if (page == NULL)
   {
      return false;
   }
   struct frame *copy_frame = page->frame;

   if (copy_frame->ref_cnt > 1)
   {
      struct frame *frame = vm_get_frame();
      page->frame = frame;
      memcpy(frame->kva, copy_frame->kva, PGSIZE);
      copy_frame->ref_cnt--;

      if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, true))
         return false;
   }
   else
   {
      copy_frame->page = page;
      if (!pml4_set_page(thread_current()->pml4, page->va, copy_frame->kva, true))
         return false;
   }

   return true;
}

/* Return true on success */
/* bogus 폴트인지? 스택확장 폴트인지?
 * SPT 뒤져서 존재하면 bogus 폴트!!
 * addr이 유저 스택 시작 주소 + 1MB를 넘지 않으면 스택확장 폴트
 * 찐폴트면 false 리턴
 * 아니면 vm_do_claim_page 호출
 * 스택확장 폴트에서 valid를 확인하려면 유저 스택 시작 주소 + 1MB를 넘는지 확인
 * addr = thread 내의 user_rsp
 * addr은 user_rsp보다 크면 안됨
 * stack_growth 호출해야함 */
bool vm_try_handle_fault(struct intr_frame *f UNUSED, void *addr UNUSED,
                         bool user UNUSED, bool write UNUSED, bool not_present UNUSED)
{
   struct supplemental_page_table *spt UNUSED = &thread_current()->spt;
   addr = pg_round_down(addr);

   uintptr_t rsp = thread_current()->user_rsp; // 유저 스택의 rsp 가져오기

   struct page *page = spt_find_page(spt, addr);
   // if (page == NULL)
   // {
   //    return false;
   // }
   // 찐 폴트
   if (page == NULL && (uintptr_t)addr >= rsp - STACK_GROW_RANGE && addr < USER_STACK && addr >= USER_STACK - (1 << 20))
   {
      vm_stack_growth(addr);
      return true;
   }

   if (page == NULL)
      return false;

   if (write == true && !page->writable)
      return false;

   if (write == true && page->writable && page->frame != NULL)
      return vm_handle_wp(page);

   ASSERT(page->operations != NULL && page->operations->swap_in != NULL);

   return vm_do_claim_page(page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void vm_dealloc_page(struct page *page)
{
   if (page->frame)
      page->frame->ref_cnt--;

   destroy(page);
   free(page);
}

/* VA에 할당된 페이지를 요구합니다 . 왜 맨날 요구만 함. */
bool vm_claim_page(void *va UNUSED)
{
   struct page *page = spt_find_page(&thread_current()->spt, va); // 스택 첫번째페이지
   if (page == NULL)
      return false;

   return vm_do_claim_page(page);
}

/* PAGE를 요구하고 mmu를 설정합니다. 근데 저는 안할겁니다.*/
static bool
vm_do_claim_page(struct page *page)
{
   void *temp = page->operations->swap_in;
   struct frame *frame = vm_get_frame();

   /* Set links */
   frame->page = page;
   page->frame = frame;

   /* TODO: Insert page table entry to map page's VA to frame's PA. */
   if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable))
      return false;

   return swap_in(page, frame->kva);
}

bool vm_copy_claim_page(void *va, struct page *parent, struct supplemental_page_table *parent_spt)
{
   struct page *page = spt_find_page(&thread_current()->spt, va); // 스택 첫번째페이지
   if (page == NULL)
      return false;

   void *temp = page->operations->swap_in;
   struct frame *frame = parent->frame;
   frame->ref_cnt++;

   if (frame->ref_cnt == 1)
      return true;

   /* Set links */
   page->frame = frame;

   /* TODO: Insert page table entry to map page's VA to frame's PA. */
   if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, false))
      return false;

   return swap_in(page, frame->kva);
}

/* Initialize new supplemental page table */
void supplemental_page_table_init(struct supplemental_page_table *spt UNUSED)
{
   if (!hash_init(&spt->SPT_hash_list, my_hash, my_less, NULL))
      return;
}

static uint64_t my_hash(const struct hash_elem *e, void *aux UNUSED)
{
   struct SPT_entry *entry = hash_entry(e, struct SPT_entry, elem);
   return hash_int((uint64_t)entry->va);
}

static bool my_less(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
   if (a == NULL)
      return true;
   if (b == NULL)
      return false;
   struct SPT_entry *a_entry = hash_entry(a, struct SPT_entry, elem);
   struct SPT_entry *b_entry = hash_entry(b, struct SPT_entry, elem);
   return a_entry->va < b_entry->va;
}

static void *duplicate_aux(struct page *src_page)
{
   if (src_page->uninit.type == VM_MMAP)
   {
      struct mmap_info *src_mmap_info = (struct mmap_info *)src_page->uninit.aux;
      struct lazy_load_info *src_info = (struct lazy_load_info *)src_mmap_info->info;

      struct mmap_info *dst_mmap_info = malloc(sizeof(struct mmap_info));
      struct lazy_load_info *dst_info = malloc(sizeof(struct lazy_load_info));

      dst_info->file = file_reopen(src_info->file);
      dst_info->offset = src_info->offset;
      dst_info->readbyte = src_info->readbyte;
      dst_info->zerobyte = src_info->zerobyte;

      dst_mmap_info->mapping_count = src_mmap_info->mapping_count;
      dst_mmap_info->info = dst_info;

      return dst_mmap_info;
   }
   else
   {
      struct lazy_load_info *src_info = (struct lazy_load_info *)src_page->uninit.aux;
      struct lazy_load_info *dst_info = malloc(sizeof(struct lazy_load_info));

      dst_info->file = file_reopen(src_info->file);
      dst_info->offset = src_info->offset;
      dst_info->readbyte = src_info->readbyte;
      dst_info->zerobyte = src_info->zerobyte;

      return dst_info;
   }
}

bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED)
{
   struct hash_iterator i;
   hash_first(&i, &src->SPT_hash_list);
   struct thread *cur = thread_current();

   while (hash_next(&i))
   {
      // src_page 정보
      struct SPT_entry *src_entry = hash_entry(hash_cur(&i), struct SPT_entry, elem);
      struct page *src_page = src_entry->page;
      enum vm_type type = src_page->operations->type;
      void *upage = src_page->va;
      bool writable = src_page->writable;

      /* 1) type이 uninit이면 */
      if (type == VM_UNINIT)
      { // uninit page 생성 & 초기화
         vm_initializer *init = src_page->uninit.init;
         void *aux = duplicate_aux(src_page);
         vm_alloc_page_with_initializer(VM_ANON, upage, writable, init, aux);
         continue;
      }

      /* 2) type이 file-backed이면 */
      if (type == VM_FILE)
      {
         struct file_page *src_info = &src_page->file;
         struct lazy_load_info *info = make_info(file_reopen(src_info->file), src_info->offset, src_info->read_byte);
         struct mmap_info *mmap_info = make_mmap_info(info, src_info->mapping_count);
         void *aux = mmap_info;

         if (!vm_alloc_page_with_initializer(type, upage, writable, lazy_load_segment, aux))
            return false;

         /* 부모에서 이미 초기화된 페이지이기에 바로 명시적 초기화 호출 */
         if (!vm_copy_claim_page(upage, src_page, dst))
            return false;

         continue;
      }

      /* 3) type이 anon이면 */
      if (!vm_alloc_page_with_initializer(type, upage, writable, NULL, NULL)) // uninit page 생성 & 초기화
         // init(lazy_load_segment)는 page_fault가 발생할때 호출됨
         // 지금 만드는 페이지는 page_fault가 일어날 때까지 기다리지 않고 바로 내용을 넣어줘야 하므로 필요 없음
         return false;

      // vm_claim_page으로 요청해서 매핑 & 페이지 타입에 맞게 초기화
      if (!vm_copy_claim_page(upage, src_page, dst))
         return false;
      // if (!vm_claim_page(upage))
      // return false;

      // 매핑된 프레임에 내용 로딩
      struct page *dst_page = spt_find_page(dst, upage);
      memcpy(dst_page->frame->kva, src_page->frame->kva, PGSIZE);
   }
   return true;
}

/* Free the resource hold by the supplemental page table */
void supplemental_page_table_kill(struct supplemental_page_table *spt UNUSED)
{
   /* TODO: 스레드가 보유한 모든 supplemental_page_table을 제거하고,
    * TODO: 수정된 내용을 스토리지에 기록(writeback)하세요. */
   struct thread *curr = thread_current();
   hash_clear(&curr->spt, hash_spt_entry_kill);
}

static void hash_spt_entry_kill(struct hash_elem *e, void *aux)
{
   struct SPT_entry *entry = hash_entry(e, struct SPT_entry, elem);
   /** spt_remove_page는 내부적으로 vm_delloc_page를 호출하고,
    * vm_delloc_page는 내부적으로 destroy 매크로를 호출한 다음
    * free(page)를 진행합니다.
    * 따라서 페이지의 타입에 따라 다른 destory 함수가 호출될 것으로 기대됩니다.
    */
   struct thread *curr = thread_current();

   vm_dealloc_page(entry->page);
   free(entry);
}

void frame_table_insert(struct list_elem *elem)
{
   struct thread *cur = thread_current();
   list_push_back(&frame_table, elem);
   return;
}

struct frame *frame_table_remove(void)
{
   struct thread *cur = thread_current();
   if (list_empty(&frame_table))
      return NULL;
   return list_entry(list_pop_front(&frame_table), struct frame, elem);
}