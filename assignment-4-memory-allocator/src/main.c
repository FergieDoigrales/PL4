#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

#include "mem.h"
#include "mem_internals.h"

static struct block_header *get_header(void *contents) {
    return (struct block_header *) (((uint8_t *) contents) - offsetof(struct block_header, contents));
}

void test1() {
    void *heap = heap_init(4096);
    assert(heap);
    void *block = _malloc(1024);
    assert(block);
    _free(block);
    heap_term();
    printf("Test №1: Passed\n\n");
}

void test2() {
    void *heap = heap_init(4096);
    assert(heap != NULL);
    debug_heap(stdout, heap);
    void *block = _malloc(64);
    assert(block != NULL);
    debug_heap(stdout, heap);
    _free(block);
    debug_heap(stdout, heap);
    heap_term();
    printf("Test №2: Passed\n\n");
}

void test3() {
    void *heap = heap_init(0);
    debug_heap(stdout, heap);
    void *block1 = _malloc(256);
    void *block2 = _malloc(256);
    void *block3 = _malloc(256);
    debug_heap(stdout, heap);
    if (block1 != NULL && block2 != NULL && block3 != NULL) {
        printf("Allocated successfully.\n");
    } else {
        printf("Allocation failed.\n");
    }

    _free(block1);
    _free(block2);
    _free(block3);

    debug_heap(stdout, heap);
    heap_term();
    printf("Test №3: Passed\n\n");

}

void test4() {
    heap_init(REGION_MIN_SIZE);
    void *ptr1 = _malloc(REGION_MIN_SIZE / 2);
    void *ptr2 = _malloc(REGION_MIN_SIZE);
    void *ptr3 = _malloc(REGION_MIN_SIZE / 2);
    assert(ptr1 != NULL);
    assert(ptr2 != NULL);
    assert(ptr3 != NULL);
    debug_heap(stdout, HEAP_START);
    _free(ptr2);
    assert(get_header(ptr2)->is_free);
    _free(ptr1);
    assert(get_header(ptr1)->is_free);
    debug_heap(stdout, HEAP_START);
    void *ptr4 = _malloc(REGION_MIN_SIZE);
    debug_heap(stdout, HEAP_START);
    _free(ptr3);
    assert(get_header(ptr3)->is_free);
    _free(ptr4);
    assert(get_header(ptr4)->is_free);
    debug_heap(stdout, HEAP_START);
    printf("Test №4: Passed\n\n");
}

void test5() {
    void *mem1 = _malloc(100000);
    void *mem2 = _malloc(100000);
    assert(mem1 != NULL && mem2 != NULL);
    _free(mem1);
    void *mem3 = _malloc(200000);
    assert(mem3 != NULL);
    _free(mem2);
    _free(mem3);
    printf("Test №5: Passed\n\n");
}

int main() {
    test1();
    test2();
    test3();
    test4();
    test5();

    return 0;
}