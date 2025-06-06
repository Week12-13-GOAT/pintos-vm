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
	/*	스왑 영역으로 사용할 디스크를 가져옵니다.
		디스크 번호(1, 1)은 PintOS에서 일반적으로 스왑 디스크로 설정된 위치입니다.
	*/
	swap_disk = disk_get(1, 1);

	// 예외 처리 : swap_disk가 없을 경우 커널 패닉 발생
	if (swap_disk == NULL)
	{
		PANIC("CAN'T FIND SWAP DISK!");
	}

	/** TODO: bitmap 자료구조로 스왑 테이블 만들기
		스왑 테이블은 각 스왑 슬록(swap slot)의 사용 여부를 추적하기 위한 비트맵
		- 스왑 슬롯 : 메모리의 한 페이지를 디스크에 저장할 수 있는 최소 단위(1 page = PGSIZE)
		- 각 스왑 슬롯은 여러 개의 디스크 섹터(sector)로 구성

		따라서 스왑 슬롯의 개수 = 전체 디스크 섹터 수 / 한 페이지를 구성하는 섹터 수
		- 디스크 전체 섹터 수 : disk_size(swap_disk)
		- 한 페이지당 섹터 수 : PGSIZE / DISK_SECTOR_SIZE

		이 값을 기반으로 비트맵을 생성
		- bitmap의 각 비트는 하나의 스왑 슬롯을 의미하며, 0이면 비어있고 1이면 사용중
	 */
	swap_table = bitmap_create(disk_size(swap_disk) / (PGSIZE / DISK_SECTOR_SIZE));
}

/* Initialize the file mapping */
bool anon_initializer(struct page *page, enum vm_type type, void *kva)
{
	// 예외 처리 → 들어온 page가 null일 경우
	if (page == NULL)
	{
		return false;
	}

	/*	이페이지는 anonymous 페이지이므로, anon_ops로 설정
		anon_ops는 스왑 인/아웃 등을 포함한 함수 포인터 구조체
	*/
	page->operations = &anon_ops;

	/* uninit을 anon으로 변환 */
	struct anon_page *anon_page = &page->anon; // page->anon은 포인터가 아니라 구조체 자체여서 항상 유효한 주소를 반환함

	/* swap index 초기화 
		-1은 아직 스왑 아웃된 적이 없다는 것을 의미
	*/
	anon_page->swap_idx = -1;

	/*	페이지의 물리 주소(kva)가 유효하며,
		해당 페이지의 frame이 1개 이하로만 참조되고 있을 경우
		새로 할당된 물리 페이지라고 간주하고 초기화함
	*/
	if (kva != NULL && page->frame->ref_cnt <= 1)
	{
		// 물리 페이지 전체를 0으로 초기화(보안 및 예측 가능한 동작 보장)
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

/* 
 * anon_destroy - 익명(anonymous) 페이지를 소멸시킵니다.
 * 
 * 이 함수는 주어진 페이지가 메모리에서 제거될 때 호출되며,
 * 해당 페이지가 스왑 공간을 사용 중이었다면 해당 스왑 슬롯을 해제합니다.
 * 
 * 매개변수:
 * - page: 제거할 대상 페이지 (호출자가 page 자체 메모리 해제는 수행함)
 */
static void
anon_destroy(struct page *page)
{
	// 페이지의 anon_page 구조체 접근 (swap 관련 정보 포함)
	struct anon_page *anon_page = &page->anon;

	// 현재 스레드의 pml4에서 이 페이지에 대한 매핑을 제거 (VA -> PA 연결 해제)
	pml4_clear_page(thread_current()->pml4, page->va);

	// 스왑 아웃된 적이 없거나, 이미 스왑에서 복구되어 유효하지 않은 스왑 슬롯이면 아무 작업도 하지 않음
	// swap_idx < 0이면 해당 페이지는 스왑 슬롯을 점유하고 있지 않음
	if (anon_page->swap_idx < 0)
	{
		return;  // 추가 작업 없이 종료
	}

	// 스왑 테이블에서 해당 스왑 슬롯을 비어있는 상태로 표시
	// 즉, 해당 슬롯은 이제 다른 페이지가 사용 가능하도록 반환됨
	bitmap_reset(swap_table, anon_page->swap_idx);
}