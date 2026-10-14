#ifndef SOMA_LANG_VM_H
#define SOMA_LANG_VM_H

#include <stdint.h>
#include <stddef.h>

enum soma_opcode {
    OP_HALT = 0,
    OP_PUSH_INT,
    OP_PUSH_STR,
    OP_POP,
    OP_DUP,
    OP_SWAP,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_GT,
    OP_AND,
    OP_OR,
    OP_NOT,
    OP_JUMP,
    OP_JUMP_IF_ZERO,
    OP_JUMP_IF_NZ,
    OP_STORE_VAR,
    OP_LOAD_VAR,
    OP_SYS_PRINT,
    OP_SYS_INPUT,
    OP_SYS_RAM,
    OP_SYS_UPTIME,
    OP_SYS_RAND,
    OP_SYS_CLEAR,
    OP_SYS_VFS_READ,
    OP_SYS_VFS_WRITE,
    OP_SYS_VFS_EXISTS,
    OP_SYS_AGENT_ASK,
    OP_SYS_MEM_QUERY,
    OP_LE,
    OP_GE,
    OP_SYS_RANDRANGE,
    OP_CALL,
    OP_RET,
    OP_ENTER,
    OP_LOAD_LOCAL,
    OP_STORE_LOCAL,
    OP_NEW_ARRAY,
    OP_ARRAY_GET,
    OP_ARRAY_SET,
    OP_ARRAY_LEN,
    OP_ARRAY_PUSH,
    OP_ARRAY_POP,
    OP_SYS_FB_IS_ACTIVE,
    OP_SYS_FB_CLEAR,
    OP_SYS_FB_RECT,
    OP_SYS_FB_PIXEL,
    OP_SYS_FB_LINE,
    OP_SYS_FB_CIRCLE,
    OP_SYS_FB_TEXT,
    OP_SYS_FB_PRESENT,
    OP_SYS_SLEEP,
    OP_SYS_KEY,
    OP_SYS_MILLIS,
    OP_STR_TO_INT,
    OP_STR_SUB,
    OP_DUP2,
    OP_NEW_TABLE,
    OP_TABLE_LEN,
    OP_TABLE_HAS,
    OP_TABLE_KEYS,
    OP_SYS_WIN_CREATE,
    OP_SYS_WIN_CLEAR,
    OP_SYS_WIN_RECT,
    OP_SYS_WIN_TEXT,
    OP_SYS_WIN_LINE,
    OP_SYS_WIN_CIRCLE,
    OP_SYS_WIN_PRESENT,
    OP_SYS_WIN_CLOSE,
    OP_SYS_WIN_ACTIVE,
    OP_SYS_WIN_MOUSE
};

/* Capacidades escaladas para aplicaciones complejas */
#define VM_STACK_CAPACITY    256
#define VM_VARS_CAPACITY     128
#define VM_LOCALS_CAPACITY   384
#define VM_FRAMES_CAPACITY   128
#define VM_ARRAYS_POOL_CAP   48
#define VM_ARRAY_ITEMS_CAP   48
#define VM_MAX_STEPS         200000000ULL

#define VM_STR_SLOTS         24
#define VM_STR_SLOT_CAP      16384

#define VM_TABLES_POOL_CAP   48
#define VM_TABLE_ITEMS_CAP   48

enum vm_val_type {
    VAL_NIL = 0,
    VAL_INT,
    VAL_STR,
    VAL_ARRAY,
    VAL_TABLE
};

struct vm_val {
    uint8_t  type;
    int64_t  i;
    char     s[128];
    uint16_t arr_id;
    uint16_t tab_id;
};

struct vm_array_obj {
    uint16_t      len;
    uint8_t       used;
    uint8_t       mark;
    struct vm_val items[VM_ARRAY_ITEMS_CAP];
};

struct vm_table_entry {
    struct vm_val key;
    struct vm_val val;
};

struct vm_table_obj {
    uint16_t              len;
    uint8_t               used;
    uint8_t               mark;
    struct vm_table_entry entries[VM_TABLE_ITEMS_CAP];
};

struct vm_window {
    int      active;
    int      x;
    int      y;
    int      w;
    int      h;
    char     title[64];
    uint32_t bg;
};

struct vm_frame {
    uint32_t ret_ip;
    int      fp;
};

struct soma_vm {
    struct vm_val       stack[VM_STACK_CAPACITY];
    int                 sp;
    struct vm_val       vars[VM_VARS_CAPACITY];
    struct vm_val       locals[VM_LOCALS_CAPACITY];
    struct vm_frame     frames[VM_FRAMES_CAPACITY];
    struct vm_array_obj array_pool[VM_ARRAYS_POOL_CAP];
    int                 array_pool_count;
    int                 lp;
    int                 fp;
    int                 fsp;
    const uint8_t      *code;
    uint32_t            code_len;
    uint32_t            ip;
    int                 halted;
    char               *str_pool;
    uint8_t             str_used[VM_STR_SLOTS];
    uint8_t             str_mark[VM_STR_SLOTS];
    uint32_t            str_len[VM_STR_SLOTS];
    struct vm_val       tmp_roots[4];
    struct vm_table_obj *table_pool;
    int                  table_pool_count;
    struct vm_window     win;
};

void vm_init(struct soma_vm *vm);
int  vm_run(struct soma_vm *vm, const uint8_t *bytecode, uint32_t len);
int  vm_test_self(void);
void vm_release(struct soma_vm *vm);

#endif
