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
static struct list_elem *clock_start;

/* 이 함수는 가상 주소(upage)에 해당하는 '페이지 객체'를 생성하고,
   초기화 함수(init)와 보조 데이터(aux)를 등록
   반드시 이 함수나 vm_alloc_page()를 통해서만 페이지를 생성해야 합니다.
   생성된 페이지는 SPT에 등록
*/
bool vm_alloc_page_with_initializer(enum vm_type type, void *upage, bool writable,
                                    vm_initializer *init, void *aux)
{
   // type이 VM_UNINIT(아직 초기화되지 않은 타입)이면 안 됨을 보장
   ASSERT(VM_TYPE(type) != VM_UNINIT)

   // 현재 스레드의 SPT(보조 페이지 테이블) 포인터를 가져옴
   struct supplemental_page_table *spt = &thread_current()->spt;

   /* 이미 해당 page가 SPT에 존재하는지 확인합니다 */
   if (spt_find_page(spt, upage) == NULL)
   {
      /* TODO: VM 타입에 따라 페이지를 생성하고, 초기화 함수를 가져온 뒤,
       * TODO: uninit_new를 호출하여 "uninit" 페이지 구조체를 생성하세요.
       * TODO: uninit_new 호출 후에는 필요한 필드를 수정해야 합니다. */
      // 페이지 타입에 따라 적절한 초기화 함수(페이지 이니셜라이저)를 선택
      bool (*page_initializer)(struct page *, enum vm_type, void *kva);
      struct page *page = malloc(sizeof(struct page));
      
      // 메모리 할당 실패 시 에러 처리(메모리 부족 등)
      if (page == NULL)
      {
         goto err;
      }

      // VM_TYPE에 따른 초기화 방법
      switch (VM_TYPE(type))
      {
      case VM_ANON:
         page_initializer = anon_initializer;         // 익명 메모리
         break;
      case VM_MMAP:
      case VM_FILE:
         page_initializer = file_backed_initializer;  // 파일 기반 메모리
         break;
      default:
         free(page);                                  // 지원하지 않는 타입이면 메모리 해제 후 에러 처리
         goto err;
         break;
      }

      // 미초기화 페이지 생성 : 실제 데이터는 아직 없고, 초기화 정보만 등록
      uninit_new(page, upage, init, type, aux, page_initializer);
      // 페이지의 쓰기 권한 설정
      page->writable = writable;

      /* TODO: 생성한 페이지를 spt에 삽입하세요. */
      // SPT에 새 페이지 등록
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
   struct frame *victim;

   ASSERT(victim != NULL);
   if(clock_start == NULL || clock_start == list_end(&frame_table))
      clock_start = list_begin(&frame_table);

   clock_now = clock_start;
   do{
      victim = list_entry(clock_now,struct frame, elem);

      bool success = pml4_is_accessed(thread_current()->pml4,victim->page->va);
      if(success == false){
         clock_start = list_next(clock_now);
         return victim;
      }


      pml4_set_accessed(thread_current()->pml4,victim->page->va, false);
      clock_now = list_next(clock_now);
      if(clock_now == list_end(&frame_table))
         clock_now = list_begin(&frame_table);
   } while(clock_now != clock_start);

   victim = list_entry(clock_now,struct frame, elem);
   clock_start = list_next(clock_now);

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
   /* 1. 새로운 frame 구조체를 메모리에 할당
      이는 물리 페이지의 메타데이터를 저장할 공간
      반드시 free해라 뒤지기싫으면.. 
   */
   struct frame *frame = malloc(sizeof(struct frame));
   // 예외 처리 → 할당 실패시 시스템 중단
   ASSERT(frame != NULL);  

   /* 2. 실제 물리 페이지(4KB) 할당 시도 
      PAL_USER : 사용자 프로세스용 메모리 풀에서 할당
      PAL_ZERO : 할당된 페이지를 0으로 초기화
   */
   frame->kva = palloc_get_page(PAL_USER | PAL_ZERO);

   /* 3. 메모리 부족 상황 처리
      사용 가능한 물리 페이지가 없는 경우 처리
   */
   if (frame->kva == NULL)
   {  
      /* 3.1. 희생자(victim) 프레인 선택 및 교체
         vm_evict_frame()은 페이지 교체 알고리즘(처음: FIFO, 지금: Clock)을 사용하여
         교체할 프레임을 선택, 해당 페이지를 디스코로 내보냅니다.
      */
      struct frame *victim1 = vm_evict_frame();

      // 예외 처리 → 교체 실패시 시스템 중단
      ASSERT(victim1 != NULL);

      /* 3.2. 희생자의 물리 페이지를 재활용
         교체된 프레임의 물리 주소를 새로운 프레임이 사용
      */
      frame->kva = victim1->kva; // victim의 물리 페이지를 재활용

      /* 3.3. 희생자 프레임 구조체 해제
         물리 페이지는 재활용하지만, 메타데이터는 새로 만듦
      */
      free(victim1);
   }

   /* 4. 새로운 프레임 초기화
      아직 어떤 가상 페이지와도 연결되지 않은 상태
   */
   frame->page = NULL;                 // 연결된 가상 페이지 X
   frame->ref_cnt = 1;                 // 참조 카운터 초기화(COW extra 과제 용)
   
   /* 5. 프레임 테이블에 등록
      시스템이 이 프레임을 추적할 수 있도록 전역 프레임 테이블에 추가
   */
   frame_table_insert(&frame->elem);   

   /* 예외 처리 → 프레임이 올바르게 초기화 되었나? */
   ASSERT(frame->page == NULL);

   /* 6. 할당된 프레임 반환
      이제 이 프레임은 가상 페이지와 연결된 준비가 완료되었습니다.
   */
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

/* VA에 할당된 페이지를 요구합니다. 
   이 함수는 Page Fault 발생 시 호출되어 가상 페이지를 물리 메모리에 로드합니다.    
*/
bool vm_claim_page(void *va UNUSED)
{  
   /* 1. 가상 주소로 SPT에서 해당 페이지 구조체 찾기 */
   struct page *page = spt_find_page(&thread_current()->spt,   // 현재 스레드의 보조 페이지 테이블
                                     va);                      // 요청된 가상 주소(e.g. 0x400000000(예시임 너무 신경 ㄴㄴ), 스택 주소, 힙 주소 등등)
   /* 2. 예외 처리 → 페이지 존재 여부 검증
      page == NULL인 case :
      - 할당되지 않은 메모리 영역에 접근 (segmentation fault 상황)
      - 잘못된 가상 주소 접근 
      - 아직 vm_alloc_page로 생성되지 않은 페이지
   */
   if (page == NULL) 
   {
      return false;  // 실패 : 유효하지 않은 페이지 요청
   }

   /* 3. 실제 페이지 클레임 수행*/
   return vm_do_claim_page(page);
}

/* PAGE를 요구하고 mmu를 설정합니다.*/
static bool
vm_do_claim_page(struct page *page)
{  
   // 1. 물리 프레임 할당
   struct frame *frame = vm_get_frame();

   /* Set links */
   /* 2. 양방향 링크 설정 (페이지 ↔ 프레임 연결)
      가상 페이지와 물리 프레임 간의 관계를 설정
   */
   frame->page = page;
   page->frame = frame;

   /* TODO: Insert page table entry to map page's VA to frame's PA. */
   // 3. 페이지 테이블 엔트리 설정(MMU 매핑)
   if (!pml4_set_page(thread_current()->pml4,   // 현재 스레드 페이지 테이블
       page->va,                                // 가상 주소
       frame->kva,                              // 물리 주소 (커널 가상 주소)
       page->writable))                         // 쓰기 권한
   {                        
   /* 매핑 실패 처리
      실패 case : 
      - 메모리 부족
      - 이미 매핑된 주소
      - 잘못된 권한 
      등등 더 자세한 설명은 생략한다. 디버깅해서 찾아보슈
   */
      return false;
   }

   // 4. 실제 페이지 내용 로딩
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

/*
 * supplemental_page_table_copy - 보조 페이지 테이블(dst)에 src 페이지 테이블을 복사합니다.
 * 
 * src의 모든 페이지를 순회하면서 페이지 타입에 따라 적절히 복사 및 초기화 작업을 수행합니다.
 * 
 * 파라미터:
 * - dst: 복사 대상 보조 페이지 테이블
 * - src: 복사할 원본 보조 페이지 테이블
 * 
 * 반환값:
 * - 모든 페이지 복사 성공 시 true 반환
 * - 중간에 실패 발생 시 false 반환
 */
bool supplemental_page_table_copy(struct supplemental_page_table *dst UNUSED, struct supplemental_page_table *src UNUSED)
{
   struct hash_iterator i;
   // src의 해시 테이블 첫 번째 요소로 iterator 초기화
   hash_first(&i, &src->SPT_hash_list);
   struct thread *cur = thread_current();

   // src의 모든 페이지 엔트리를 순회
   while (hash_next(&i))
   {
      // src_page 정보
      struct SPT_entry *src_entry = hash_entry(hash_cur(&i), struct SPT_entry, elem);
      struct page *src_page = src_entry->page;
      enum vm_type type = src_page->operations->type;    // 페이지 타입 확인
      void *upage = src_page->va;                        // 가상 주소
      bool writable = src_page->writable;                // 쓰기 가능 여부

      /* 1) type이 uninit이면(초기화되지 않은 페이지) */
      if (type == VM_UNINIT)
      {  // 원본 페이지의 초기화 함수와 aux 정보를 복제하여 새로운 페이지 생성
         vm_initializer *init = src_page->uninit.init;
         void *aux = duplicate_aux(src_page);

         // dst에 익명(anon) 페이지를 초기화 함수와 aux와 함께 할당
         vm_alloc_page_with_initializer(VM_ANON, upage, writable, init, aux);
         continue;
      }

      /* 2) type이 file-backed이면(파일 기반 페이지) */
      if (type == VM_FILE)
      {
         // 원본 페이지의 파일 관련 정보 가져오기
         struct file_page *src_info = &src_page->file;
         // 파일 핸들을 복사하고 읽을 위치 및 바이트 수 정보를 포함한 lazy_load_info 생성
         struct lazy_load_info *info = make_info(file_reopen(src_info->file), src_info->offset, src_info->read_byte);
         // mmap 관련 정보 생성(복사할 때 매핑 카운트 등 포함)
         struct mmap_info *mmap_info = make_mmap_info(info, src_info->mapping_count);
         void *aux = mmap_info;

         // 부모 프로세스(src)에서 이미 초기화된 페이지 내용을 명시적으로 복사
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
      // if (!vm_claim_page(upage))
      // return false;
      /* Project 3 extra : COW */ 
      if (!vm_copy_claim_page(upage, src_page, dst))
         return false;

      // 매핑된 프레임(실제 메모리)에 src 페이지 내용 복사
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