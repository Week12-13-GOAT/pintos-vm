/* uninit.c: 미초기화(uninitialized) 페이지의 구현.
 *
 * 모든 페이지는 처음에 uninit 페이지로 생성됩니다. 첫 번째 페이지 폴트가 발생하면,
 * 핸들러 체인이 uninit_initialize(page->operations.swap_in)를 호출합니다.
 * uninit_initialize 함수는 페이지 객체를 초기화하여 해당 페이지를
 * 특정 페이지 객체(anon, file, page_cache)로 변환(transmute)하고,
 * vm_alloc_page_with_initializer 함수에서 전달된 초기화 콜백을 호출합니다.
 * */

#include "vm/vm.h"
#include "vm/uninit.h"

static bool uninit_initialize(struct page *page, void *kva);
static void uninit_destroy(struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations uninit_ops = {
	.swap_in = uninit_initialize,
	.swap_out = NULL,
	.destroy = uninit_destroy,
	.type = VM_UNINIT,
};

/* DO NOT MODIFY this function */
void uninit_new(struct page *page, void *va, vm_initializer *init,
				enum vm_type type, void *aux,
				bool (*initializer)(struct page *, enum vm_type, void *))
{
	ASSERT(page != NULL);

	*page = (struct page){
		.operations = &uninit_ops,
		.va = va,
		.frame = NULL, /* no frame for now */
		.uninit = (struct uninit_page){
			.init = init,
			.type = type,
			.aux = aux,
			.page_initializer = initializer,
		}};
}

/* 첫 번째 page fault 발생 시 해당 페이지를 초기화하는 함수 */
static bool
uninit_initialize(struct page *page, void *kva)
{	
	/*	페이지 구조체의 uninit 멤버를 가져옴
		이 구조체에는 페이지 타입, 초기화 함수, aux 데이터가 저장되어 있음
	*/ 
	struct uninit_page *uninit = &page->uninit;

	/*	page_initialize에서 사용할 수 있도록 먼저 init 함수를 가져옴
		나중에 page_initializer가 이 값을 덮어쓸 수 있음
	*/
	vm_initializer *init = uninit->init;

	// 페이지 초기화에 필요한 부가 정보를 담고 있는 aux 포인터를 가져옴
	void *aux = uninit->aux;

	/*	만약 페이지 타입이 mmap 또는 file-backed (VM_MMAP or VM_FILE)인 경우
		aux에는 mmap_info 구조체가 들어 있으므로, 그 안의 info 필드를 사용함 
	*/
	if (uninit->type == VM_MMAP || uninit->type == VM_FILE)
	{
		struct mmap_info *mmap_info = (struct mmap_info *)aux;
		aux = mmap_info->info;
	}

	/* TODO: 이 함수를 수정해야 할 수도 있습니다. */
	return uninit->page_initializer(page, uninit->type, kva) &&
		   (init ? init(page, aux) : true);
}

/* 
 * uninit_destroy - 초기화되지 않은(uninit) 페이지가 제거될 때 호출되는 함수입니다.
 * 
 * 대부분의 uninit 페이지는 실제로 페이지 폴트가 발생하면서 
 * 익명(anon) 페이지나 파일(file) 페이지로 전환되어 사용됩니다.
 * 
 * 그러나 프로세스 실행 중 참조되지 않은 uninit 페이지는 실제 메모리에 로드되지 않고,
 * 종료 시점에까지 초기화되지 않은 상태로 남아있을 수 있습니다.
 * 
 * 이 함수는 그러한 경우에 대비해, uninit 페이지가 가지고 있는
 * 부가 정보(auxiliary data)를 정리합니다.
 * 
 * 매개변수:
 * - page: 제거 대상 페이지. page 객체 자체는 호출자가 해제함.
 */
static void
uninit_destroy(struct page *page)
{
	// uninit 페이지 구조체를 가져옵니다. (현재는 사용되지 않음)
	struct uninit_page *uninit UNUSED = &page->uninit;

	/* auxiliary data 해제
	 * aux는 lazy loading 등 페이지 초기화를 위해 미리 저장해둔 데이터입니다.
	 * 예: lazy_load_segment에서 사용하는 lazy_load_info 구조체 등.
	 * 
	 * 초기화되지 않고 남은 페이지라면 이 aux도 사용되지 않았기 때문에,
	 * 지금 해제해줘야 메모리 누수가 발생하지 않습니다.
	 */
	free(uninit->aux);
}
