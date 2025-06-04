/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "lib/kernel/bitmap.h"
#include "devices/disk.h"
#include "threads/mmu.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in(struct page *page, void *kva);
static bool anon_swap_out(struct page *page);
static void anon_destroy(struct page *page);

struct bitmap *swap_table;

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void vm_anon_init(void)
{
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get(1, 1);

	// 예외 처리
	if (swap_disk == NULL)
	{
		PANIC("CAN'T FIND SWAP DISK!");
	}

	/** TODO: bitmap 자료구조로 스왑 테이블 만들기
	 * bitmap_create로 만들기
	 * 스왑 테이블 엔트리에 이 엔트리가 비어있다는 비트 필요
	 * bitmap 공부가 필요할듯
	 */
	swap_table = bitmap_create(disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE));
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	// 들어온 page가 null일 경우
	if (page == NULL)
	{
		return false;
	}
	/* Set up the handler */
	page->operations = &anon_ops;

	/* uninit을 anon으로 변환 */
	struct anon_page *anon_page = &page->anon; // page->anon은 포인터가 아니라 구조체 자체여서 항상 유효한 주소를 반환함

	/* swap index 초기화 */
	anon_page->swap_idx = -1;

	/* 물리 주소 초기화 */
	if (kva != NULL && page->frame->ref_cnt <= 1)
	{
		memset(kva, 0, PGSIZE);
	}

	return true;
}

/* 스왑 디스크에서 내용을 읽어와 페이지를 스왑인합니다. */
static bool
anon_swap_in(struct page *page, void *kva)
{
	struct anon_page *anon_page = &page->anon;
	int swap_idx = anon_page->swap_idx;

	// 예외 처리 → swap_idx가 -1이면 페이지가 스왑아웃된 적이 없거나 이미 복구되었으므로 스왑 인 생략
	if (swap_idx < 0)
	{
		return false;
	}
	// disk_read에서 사용할 버퍼
	// void *buffer[PGSIZE];
	/** TODO: 페이지 스왑 인
	 * disk_read를 데이터를 읽고 kva에 데이터 복사
	 * swap_idx를 -1로 바꿔주어야 함
	 * 프레임 테이블에 해당 프레임 넣어주기
	 * 프레임하고 페이지 매핑해주기
	 */

	// 한 섹터는 512바이트이고, 한 페이지는 4KB(4096바이트)이므로
	// 총 8개의 섹터를 순차적으로 읽어야 전체 페이지 데이터를 복원할 수 있음
	// swap_idx는 스왑 테이블 상의 페이지 단위 인덱스를 의미하며,
	// 실제 섹터 번호는 swap_idx * 8부터 시작함
	for (int i = 0; i < 8; i++)
	{
		disk_read(swap_disk,					 // 스왑 디스크에서 데이터를 읽어옴
				  (swap_idx * 8) + i,			 // 8개의 연속된 섹터에 페이지가 저장되어 있으므로, i를 더해가며 읽음
				  kva + (DISK_SECTOR_SIZE * i)); // 읽어온 데이터를 커널 가상 주소 kva에 512B 단위로 복사
	}

	// 스왑 테이블에서 해당 스왑 슬롯을 비어있다고 표시 (해당 슬롯 재사용 가능하도록)
	bitmap_reset(swap_table, swap_idx);

	// 페이지가 더 이상 스왑 영역에 존재하지 않음을 나타내기 위해 swap_idx를 -1로 초기화
	anon_page->swap_idx = -1;

	return true;
}

/* 페이지의 내용을 스왑 디스크에 기록하여 스왑아웃합니다. */
static bool
anon_swap_out(struct page *page)
{
	// page 인자의 예외 처리
	if (page == NULL)
	{
		return false;
	}
	struct anon_page *anon_page = &page->anon;
	/** TODO: disk_write를 사용하여 disk에 기록
	 * 섹터 크기는 512바이트라 8번 반복해야합니다
	 * 비어있는 스왑 슬롯을 스왑 테이블에서 검색
	 * 검색된 스왑 슬롯 인덱스를 anon_page에 저장
	 * disk_write를 통해 해당 디스크 섹터에 저장
	 */

	size_t swap_idx = bitmap_scan_and_flip(swap_table, 0, 1, false);

	if (swap_idx == BITMAP_ERROR)
	{
		ASSERT(bitmap_test(swap_table, swap_idx) == false);
		return false;
	}

	// swap in에 자세히 주석을 달아 놓았음 잘 살펴 보셈
	for (int i = 0; i < 8; i++)
	{
		disk_write(swap_disk, (swap_idx * 8) + i, page->frame->kva + (DISK_SECTOR_SIZE * i));
	}

	// 페이지와 프레임 간의 연결을 끊음 (프레임은 더 이상 이 페이지를 참조 하지 않음)
	page->frame->page = NULL;
	page->frame = NULL;

	// 스왑 슬록 인덱스를 anon_page에 저장해 나중에 다시 swap_in할 수 있게 함
	anon_page->swap_idx = swap_idx;

	return true;
}

/* 익명 페이지를 소멸시킵니다. PAGE는 호출자가 해제합니다. */
static void
anon_destroy(struct page *page)
{
	struct anon_page *anon_page = &page->anon;
	// swap_idx가 0보다 작을 경우는 페이지가 스왑 아웃이 된 적이 없거나 이미 복구 되어 swap_idx가 -1이면 추가 작업 X, 종료
	pml4_clear_page(thread_current()->pml4, page->va);
	if (anon_page->swap_idx < 0)
	{
		return;
	}

	// 스왑 테이블에서 해당 스왑 슬롯을 비어있는 상태로 표시
	// 즉, 더 이상 해당 스왑 슬롯은 사용되지 않으며, 이후 다른 페이지가 재사용 가능
	bitmap_reset(swap_table, anon_page->swap_idx);
}