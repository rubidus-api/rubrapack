// src/pal/win32/entry_win32.c - process entry on Windows: UTF-16 argv becomes UTF-8 here,
// once, and rp_main never sees UTF-16. Built with -municode.

#include "rubrapack/cli.h"
#include "rubrapack/diag.h"
#include "rubrapack/text.h"

#include <stdio.h>
#include <wchar.h>

#include "proven/heap.h"

int wmain(int argc, wchar_t **argv) {
    proven_allocator_t heap = proven_heap_allocator();
    if (argc < 0) return RP_EXIT_USAGE;

    proven_result_mem_mut_t block =
        heap.alloc_fn(heap.ctx, ((size_t)argc + 1) * sizeof(proven_u8str_t), alignof(proven_u8str_t));
    proven_result_mem_mut_t ptrs =
        heap.alloc_fn(heap.ctx, ((size_t)argc + 1) * sizeof(char *), alignof(char *));
    if (block.err != PROVEN_OK || ptrs.err != PROVEN_OK) {
        rp_diag_error(RP_DIAG_NOMEM, "out of memory");
        return RP_EXIT_IO;
    }
    proven_u8str_t *strs = (proven_u8str_t *)(void *)block.value.ptr;
    char **args = (char **)(void *)ptrs.value.ptr;

    int made = 0;
    int status = RP_EXIT_OK;
    for (int i = 0; i < argc; ++i) {
        proven_u16str_view_t view = { .ptr = (const proven_u16 *)argv[i], .size = wcslen(argv[i]) };
        rp_text_result_t r = rp_utf16_to_u8str(heap, view, &strs[i]);
        if (r.err != PROVEN_OK) {
            rp_diag_error(RP_DIAG_BAD_ARG_TEXT, "argument %d is not valid Unicode (UTF-16 unit %zu)",
                          i, r.offset);
            status = RP_EXIT_USAGE;
            break;
        }
        args[i] = (char *)proven_u8str_as_cstr(&strs[i]);
        made = i + 1;
    }
    args[argc] = NULL;

    if (status == RP_EXIT_OK) status = rp_main(argc, args);

    for (int i = 0; i < made; ++i) proven_u8str_destroy(heap, &strs[i]);
    heap.free_fn(heap.ctx, ptrs.value.ptr);
    heap.free_fn(heap.ctx, block.value.ptr);
    return status;
}
