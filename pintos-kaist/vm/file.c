/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "string.h"

struct lock filesys_lock;

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void)
{
	/* 전역 자료구조 초기화 */
	/* mmap_list 초기화 */
	list_init(&thread_current()->mmap_list);
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva)
{
	/* Set up the handler */
	page->operations = &file_ops;
	struct mmap_info *mapping_info = (struct mmap_info *)page->uninit.aux;
	struct lazy_load_info *info = (struct lazy_load_info *)mapping_info->info;

	struct file *backup_file = info->file;
	off_t backup_offset = info->offset;
	size_t read_byte = info->readbyte;
	size_t zero_byte = info->zerobyte;
	int mapping_count = mapping_info->mapping_count;

	struct file_page *file_page = &page->file;
	/* swap out을 대비해 저장 */
	file_page->file = backup_file;
	file_page->offset = backup_offset;
	file_page->read_byte = read_byte;
	file_page->zero_byte = zero_byte;
	file_page->mapping_count = mapping_count;

	return true;
}

/* 파일에서 내용을 읽어와 페이지를 스왑인합니다. */
static bool
file_backed_swap_in(struct page *page, void *kva)
{
	// file_page는 file-backed 페이지에 대한 메타데이터를 담고 있는 구조체
	struct file_page *file_page UNUSED = &page->file;
	// swap_in을 위한 버퍼
	// void *buffer[PGSIZE];
	/** TODO: 파일에서 정보를 읽어와 kva에 복사하세요
	 * aux에 저장된 백업 정보를 사용하세요
	 * file_open과 read를 사용하면 될 것 같아요
	 * 파일 시스템 동기화가 필요할수도 있어요
	 * 필요시 file_backed_initializer를 수정하세요
	 */
	// mmap 시 등록된 파일 객체 포인터
	struct file *file = file_page->file;
	// mmap 시 설정된 파일 내 offset(해당 페이지가 파일의 어디서부터 읽어야 하는지 나타냄)
	off_t offset = file_page->offset;
	// 실제로 파일에서 읽어야 할 바이트 수
	size_t read_byte = file_page->read_byte;

	// 파일에서 데이터를 읽어와 kva(페이지가 매핑된 커널 가상 주소)에 저장
	if (file_read_at(file, kva, read_byte, offset) != (off_t)read_byte)
	{
		// 읽은 바이트 수가 기대치와 다르면 오류 처리
		return false;
	}

	// 파일에서 읽어오지 못한 나머지 페이지 영역을 0으로 초기화
	memset(kva + read_byte, 0, page->file.zero_byte);

	return true;
}

/* 페이지의 내용을 파일에 기록(writeback)하여 스왑아웃합니다. */
static bool
file_backed_swap_out(struct page *page)
{
	// file_page는 file-backed 페이지에 대한 메타데이터를 담고 있는 구조체
	struct file_page *file_page UNUSED = &page->file;
	/** TODO: dirty bit 확인해서 write back
	 * pml4_is_dirty를 사용해서 dirty bit를 확인하세요
	 * write back을 할 때는 aux에 저장된 파일 정보를 사용
	 * file_write를 사용하면 될 것 같아요
	 * dirty_bit 초기화 (pml4_set_dirty)
	 */
	struct thread *curr = thread_current();
	bool dirty_bit = pml4_is_dirty(curr->pml4, page->va);

	// dirty bit가 true이면, 즉 메모리에서 수정된 경우
	if (dirty_bit == true)
	{
		// 공유 자원 접근 → 락 걸고 접근
		lock_acquire(&filesys_lock);
		if (file_write_at(file_page->file,		// mmap된 파일 객체
						  page->frame->kva,		// 페이지의 실제 물리 주소
						  file_page->read_byte, // 실제로 파일에 기록할 바이트 수
						  file_page->offset)	// 파일 내 시작 위치
			!= (off_t)file_page->read_byte)
		{
			// write 실패하면 lock 해제 해야겠지?
			lock_release(&filesys_lock);
			return false;
		}
		// 파일 쓰기 완료 후 락 해제
		lock_release(&filesys_lock);

		// 더티 비트 클리어(쓰기 완!)
		pml4_set_dirty(curr->pml4, page->va, false);
	}
	// 초기화는 victim에서
	page->frame->page = NULL;
	page->frame = NULL;

	return true;
}

/* 파일 기반 페이지를 소멸시킵니다. PAGE는 호출자가 해제합니다. */
static void
file_backed_destroy(struct page *page)
{
	// file_page는 file-backed 페이지에 대한 메타데이터를 담고 있는 구조체
	struct file_page *file_page UNUSED = &page->file;
	/** TODO: dirty_bit 확인 후 write_back
	 * pml4_is_dirty를 사용해서 dirty bit 확인
	 * write back을 할 때는 aux에 저장된 파일 정보를 사용
	 * file_write를 사용하면 될 것 같아요
	 */
	struct thread *curr = thread_current();

	// 파일을 스기 가능하게 설정 → read_only로 열렸을 수도 있으므로
	file_allow_write(file_page->file);

	// 페이지가 dirty 상태 → 메모리 상에서 파일 내용이 수정됨
	if (pml4_is_dirty(thread_current()->pml4, page->va))
	{
		lock_acquire(&filesys_lock);
		off_t written = file_write_at(file_page->file,		// mmap으로 매핑된 파일 객체
									  page->frame->kva,		// 물리 메모리 상 해당 페이지의 커널 주소
									  file_page->read_byte, // 실제로 파일에 쓸 바이트 수
									  file_page->offset);	// mmap할 때 저장된 파일 내부의 오프셋 위치
		lock_release(&filesys_lock);
		ASSERT(written == file_page->read_byte);

		// dirty bit를 false로 초기화(더 이상 수정 X)
		pml4_set_dirty(curr->pml4, page->va, false);
	}

	// 해당 페이지가 물리 프레임에 매핑되어 있으면
	if (page->frame != NULL && page->frame->ref_cnt < 1)
	{
		// 물리 페이지를 해제하고, frame 구조체도 동적 메모리 해제
		palloc_free_page(page->frame->kva);
		free(page->frame);
		page->frame = NULL;
	}

	// 최종적으로 사용자 가상 주소 공간에서 해당 페이지 매핑을 제거
	pml4_clear_page(curr->pml4, page->va);
}

struct lazy_load_info *make_info(
	struct file *file, off_t offset, size_t read_byte)
{
	struct lazy_load_info *info = malloc(sizeof(struct lazy_load_info));
	info->file = file;
	info->offset = offset;
	info->readbyte = read_byte;
	info->zerobyte = PGSIZE - read_byte;
	return info;
}

struct mmap_info *make_mmap_info(struct lazy_load_info *info, int mapping_count)
{
	struct mmap_info *mmap_info = malloc(sizeof(struct mmap_info));
	mmap_info->info = info;
	mmap_info->mapping_count = mapping_count;
	return mmap_info;
}

/* Do the mmap */
void *do_mmap(void *addr, size_t length, int writable,
			  struct file *file, off_t offset)
{
	/* 지연 로딩과 스왑 시의 백업 정보 저장 */
	/* mmap 대상 파일의 전체 크기*/
	off_t file_size = file_length(file);

	/* offset 이후 실제로 읽을 수 있는 크기 계산 */
	off_t read_size = file_size - offset;

	/* 실제로 읽을 수 있는 크기가 0미만이면 읽을 수 있는 게 없으므로 0으로 설정 */
	if (read_size < 0) {
		read_size = 0;
	}

	/* 남은 매핑할 파일 길이 */
	size_t remain_length = (size_t)read_size;

	/* 현재 매핑 중인 주소 및 파일 offset */
	void *cur_addr = addr;
	off_t cur_offset = offset;

	/* 	파일은 mmap 동안 읽기 전용으로 reopen 해서 사용됨
		원본 파일은 fd_table에 유지되므로 별도로 reopen 필요
	*/
	struct file *reopen_file = file_reopen(file);
	/*	mmap은 여러 페이지에 걸쳐 매핑될 수 있습니다.
		munmap 시에 어디까지 해제해줄 것인지 판단할 기준이 됩니다 
	*/
	int mapping_count = 0; 

	/* 매핑해야 할 전체 크기를 페이지 단위로 반복하며 처리 */
	while (remain_length > 0)
	{
		/*	한 페이지(4KB) 단위로 매핑 처리
			남은 크기가 한 페이지보다 작다면, 그만큼만 할당
		*/
		size_t allocate_length = remain_length > PGSIZE ? PGSIZE : remain_length;
		
		/*	지연 로딩에 사용할 info 구조체 생성
			reopen된 파일 핸들, 현재 offset, 읽을 크기 등을 저장
		*/
		struct lazy_load_info *info = make_info(reopen_file, cur_offset, allocate_length);
		
		/*	mmap 영역이라는 것을 표시하고,
			해당 매핑에 대한 식별자(mapping_count)를 포함한 구조체 생성
		*/
		struct mmap_info *mmap_info = make_mmap_info(info, mapping_count);
		
		void *aux = mmap_info;

		/* mmap 또한 지연 로딩이 필요합니다 */
		vm_alloc_page_with_initializer(VM_MMAP, cur_addr, writable, lazy_load_segment, aux);
		/* remain_length는 unsigned 형입니다. 따라서 음수가 없기에 미리 break를 해줘야 합니다 */
		if (remain_length < PGSIZE)
			break;
		
		/* 다음 페이지를 위해 주소, offset, 남은 길이 갱신 */
		remain_length -= PGSIZE;
		cur_addr += PGSIZE;
		cur_offset += PGSIZE;
		mapping_count++;
	}
}

static bool is_my_mmap(void *addr, struct file *mmap_file, int mmap_count)
{
	/* 현재 스레드의 보조 테이블 가져오기 */
	struct supplemental_page_table *spt = &thread_current()->spt;
	
	/* addr에 해당하는 페이지를 보조 테이블에서 찾음 */
	struct page *find_page = spt_find_page(spt, addr);
	
	/* 예외 처리 → 해당 주소에 매핑된 페이지가 존재하지 않으면 false */
	if (find_page == NULL)
		return false;

	/* 해당 페이지가 참조하는 있는 파일 객체를 가져옴*/
	struct file *find_file = find_page->file.file;

	/*	파일이 존재하지 않거나, mmap시 사용된 파일과 다르면 false
		→ 이는 다른 mmap의 페이지일 가능성이 높음
	*/
	if (find_file == NULL || find_file != mmap_file)
		return false;

	/*	동일한 파일이라 하더라도, mmap 내 인덱스(mapping_count)가 다르면
		→ 이는 다른 mmap에서 온 페이지일 수 있으므로 false
	*/
	if (find_page->file.mapping_count != mmap_count)
		return false;

	/* 위에 예외 케이스에 안걸린다면 mmap영역의 연속된 페이지임*/
	return true;
}

/* Do the munmap */
void do_munmap(void *addr)
{
	/* 현재 스레드의 보조 테이블을 가져오기 */
	struct supplemental_page_table *spt = &thread_current()->spt;
	/* SPT에 없으면 해제할 수 없습니다 !! */
	/* 항상 mmap 영역의 첫번째 주소를 준다고 합니다 */
	struct page *target_page = spt_find_page(spt, addr);
	if (target_page == NULL)
		return;

	/* 해당 페이지가 mmap에 의해 매핑된 파일인지 확인*/
	struct file_page *file_page = &target_page->file;
	struct file *target_file = file_page->file;			// 넌 어떤 파일이냐...?
	int target_mmap_count = file_page->mapping_count;	// mmap에서 몇 번째 페이지인지

	addr += PGSIZE; // 다음 mmap_file 찾기
	/* while 루프를 통해 연속된 페이지가 같은 mmap 영역인지 확인합니다 */
	while (is_my_mmap(addr, target_file, ++target_mmap_count))
	{
		struct page *remove_page = spt_find_page(spt, addr);
		spt_remove_page(spt, remove_page);
		addr += PGSIZE;
	}
	/* 첫번째 주소를 해제합니다 */
	spt_remove_page(spt, target_page);
}