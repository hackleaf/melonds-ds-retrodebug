/*
 * retrodebug.h
 * based on hcdebug.h by leiradel
 *
 * Everything starts at rd_DebuggerIf, so please see that struct first.
*/

#ifndef RETRO_DEBUG__
#define RETRO_DEBUG__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define RD_API_VERSION 1

/* Watchpoint operations */
#define RD_MEMORY_READ (1 << 0)
#define RD_MEMORY_WRITE (1 << 1)

/* Event types */
typedef enum {
    RD_EVENT_BREAKPOINT = 0,
    RD_EVENT_STEP = 1,
    RD_EVENT_INTERRUPT = 2,
    RD_EVENT_MEMORY = 3,
    RD_EVENT_REG = 4,
    RD_EVENT_MISC = 5
}
rd_EventType;

/* Subscription ID. Helps identify subscriber, and also allows unsubscribing from an event. A negative ID indicates an error.
   IDs are not necessarily consecutive, and an ID may be re-used only after unsubscribing. Otherwise, IDs are unique, even
   between different event types. (The core might implement this by using some bits of the event ID to indicate the event type.) */
typedef int64_t rd_SubscriptionID;

/* Stepping modes for RD_EVENT_STEP subscriptions */
typedef enum {
    /* Report every instruction */
    RD_STEP_INTO,

    /* As above, but if an interrupt occurs, temporarily disable until returned from interrupt */
    RD_STEP_INTO_SKIP_IRQ,

    /* As above, but if a subroutine is invoked, temporarily disable until returned from subroutine */
    RD_STEP_OVER,

    /* Initially disabled; only enabled after returning from the current subroutine. */
    RD_STEP_OUT,
}
rd_StepMode;

typedef struct rd_MiscBreakpoint {
    struct {
        /* e.g. "rtc_reg" */
        char const* id;
        
        /* Human-readable description */
        char const* description;
    }
    v1;
}
rd_MiscBreakpoint;

typedef struct rd_Memory rd_Memory;

/*
 * Describes one region of a memory map. Entries must be consecutive: each
 * entry's base_addr equals the previous entry's base_addr + size.
 */
typedef struct rd_MemoryMap {
    // lowest address in this region
    uint64_t base_addr;
    
    // cannot be 0.
    uint64_t size;
    
    // optional, use if memory mapped region refers to another rd_Memory.
    rd_Memory const* source;
    
    // memory at base_addr coincides with source's source_base_addr
    uint64_t source_base_addr;
    
    // optional; negative if unused/undefined.
    int64_t  bank;

    // optional flags
    uint32_t flags;
}
rd_MemoryMap;

#define RD_MEMMAP_CACHED        (1 << 0)  // Region may be cached; peek may differ from source
#define RD_MEMMAP_MISCREADBLOCK (1 << 1)  // Reads via this mapping may differ from direct source
                                          // reads for reasons other than caching (e.g. hardware
                                          // I/O arbitration, read-sensitive registers, bus muxing).

struct rd_Memory {
    struct {
        /* Matches regex: ([a-z_][a-z0-9_]*)
         * e.g. "ram", "wram", "rom"
        */
        char const* id;
        char const* description; /* e.g. "Work-RAM" */
        unsigned alignment; /* typically 1, 2, 4, or 8 */
        uint64_t size;

        /* Supported breakpoints not covered by specific functions.
         * Null-terminated pointer array. */
        rd_MiscBreakpoint const* const* break_points;

        /* Reads size bytes starting at address into outbuff.
         * If side_effects is false, the core must guarantee no side effects
         * (e.g. from IO register access) occur. If a side-effect-free read
         * is not possible at a given address, the core should write 0 for
         * that byte.
         * Returns the number of bytes successfully read.
         * peek must never cause memory subscriptions to fire. */
        uint64_t (*peek)(struct rd_Memory const* self, uint64_t address, uint64_t size, uint8_t* outbuff, bool side_effects);

        /* Writes size bytes from buff starting at address.
         * poke can be null for read-only memory but all memory should be
         * writeable to allow patching. Can fail.
         * Returns the number of bytes successfully written.
         * poke must never cause memory subscriptions to fire. */
        uint64_t (*poke)(struct rd_Memory const* self, uint64_t address, uint64_t size, uint8_t const* buff);

        /*
         * Optional memory map. Both pointers must be non-NULL or both NULL.
         * The caller allocates the array using the count returned by
         * get_memory_map_count(), then passes it to get_memory_map() to fill.
         */
        unsigned (*get_memory_map_count)(struct rd_Memory const* self);
        void (*get_memory_map)(struct rd_Memory const* self, rd_MemoryMap *out);

        /* Optional. Returns true if there is banking possible at the given address
         * and bank is a valid bank number. Returns false if the address is not
         * banked or if bank is out of range. If bank N is valid, all banks
         * less than N must be valid too.
         *
         * If returns true, populates the given rd_MemoryMap struct (if not NULL)
         * to specify where the given address would point if the given bank were
         * loaded.
         */
        bool (*get_bank_address)(struct rd_Memory const* self, uint64_t address, int64_t bank, rd_MemoryMap* out);

        /* Optional. Probe whether a byte is present in the CPU's cache.
         * Returns:
         *   <0  no cache / uncacheable at this address
         *    0  cacheable but not currently cached (miss)
         *  >=1  cached (hit)
         * NULL means no cache probe support (treat as if every call returned <0). */
        int (*cache_probe)(struct rd_Memory const* self, uint64_t address);
    }
    v1;
};

typedef struct rd_Cpu {
    struct {
        /* Matches regex: ([a-z_][a-z0-9_]*)
         * use "cpu" if only one cpu; otherwise refer to form or function
         * e.g. "z80", "m68k", "cpu1", "cpu2", "co", etc.
        */
        char const* id;
        char const* description;
        unsigned type;

        /* CPU-type-specific configuration.  Interpretation depends on the
         * CPU type.  For example, ARM uses the lower 8 bits for the
         * architecture version and upper bits for capability flags.
         * Set to 0 if not applicable. */
        uint32_t config;

        /* Memory region that is CPU addressable */
        rd_Memory const* memory_region;

        /* Supported breakpoints not covered by specific functions.
         * Null-terminated pointer array. */
        rd_MiscBreakpoint const* const* break_points;

        /* CPU Registers; return 1 on set_register to signal a successful write */
        uint64_t (*get_register)(struct rd_Cpu const* self, unsigned reg);
        int (*set_register)(struct rd_Cpu const* self, unsigned reg, uint64_t value);

        /* Optional; used for pipelined CPUs like MIPS.
           Returns where the PC is expected to be after n delay slots.
           if delay=0, should generally return the current PC value.
           Returns false if failed to calculate or slot out of range. */
        bool (*pipeline_get_delay_pc)(struct rd_Cpu const* self, unsigned delay, uint64_t* out_pc);
    }
    v1;
}
rd_Cpu;

/* ---- Filesystem ---- */

typedef enum {
    RD_FS_OTHER     = 0,
    RD_FS_FILE      = 1,
    RD_FS_DIRECTORY = 2,
} rd_FsFileType;

#define RD_FS_READABLE  (1 << 0)
#define RD_FS_WRITABLE  (1 << 1)

typedef struct rd_FsStat {
    uint64_t size;
    rd_FsFileType type;
    
    /* RD_FS_READABLE, RD_FS_WRITABLE */
    uint32_t flags;
    
    /* Optional, for disc filesystems */
    uint64_t sector;
} rd_FsStat;

typedef struct rd_Filesystem {
    struct {
        /* A slug identifying the filesystem.
         *
         * e.g. "iso", "bu0", "bu1", "cdda", "cdsys", "C".
         *
         * Use "fs" if nothing else suitable.
         * Should match regex [a-zA-Z_][a-zA-Z0-9_]*
         */
        char const *scheme;
        
        /* e.g. "ISO 9660 Filesystem", "Memory Card Slot A" */
        char const *description;
        
        /* path separator: typically '/', '\\', or '\0' for flat filesystems. */
        char separator;

        /* Required. List filenames in a directory. Calls cb once per entry with the
         * entry's basename (never includes the path separator).
         * Returns the number of entries, or negative on error (e.g. if path
         * does not point to a directory). 
         *
         * 'path' is absolute (always starts with path separator).
         */
        int (*list)(struct rd_Filesystem const *self, const char *path, void *user_data,
                         void (*cb)(const char *name, void *user_data));

        /* Required. Get metadata for a path. Returns true on success. 
         * 'path' is absolute (always starts with path separator).
         */
        bool (*stat)(struct rd_Filesystem const *self, const char *path,
                     rd_FsStat *out);

        /* Optional. Read file data starting at offset. Returns bytes read. */
        uint64_t (*read)(struct rd_Filesystem const *self, const char *path,
                         uint64_t offset, uint8_t *buf, uint64_t size);

        /* Optional. Write file data starting at offset. Returns bytes written. */
        uint64_t (*write)(struct rd_Filesystem const *self, const char *path,
                          uint64_t offset, uint8_t const *buf, uint64_t size);
    } v1;
} rd_Filesystem;

typedef struct rd_System {
    struct {
        /* Common system id, lower case -- e.g. "nes", "gb", "gbc", "megadrive". Allows the front-end to identify the system type, so if there are other cores that implement the same system, follow their lead. */
        char const* id;

        /* CPUs available in the system. Null-terminated pointer array.
           The first CPU (cpus[0]) is the main/primary CPU. */
        rd_Cpu const* const* cpus;

        /* Memory regions that aren't addressable by any of the CPUs on the system.
         * Null-terminated pointer array. */
        rd_Memory const* const* memory_regions;

        /* Filesystems exposed by the core (CD-ROM, memory cards, etc.).
         * Null-terminated pointer array. */
        rd_Filesystem const* const* filesystems;

        /* Supported breakpoints not covered by specific functions.
         * Null-terminated pointer array. */
        rd_MiscBreakpoint const* const* break_points;
        
        /* ======================================================================== */
        /* Schema Format                                                             */
        /* ======================================================================== */
        /*
         * Schemata describe the binary layout of C structs passed through the
         * retrodebug interface (misc event data, proc-address output, etc.).
         * They let the frontend interpret void* payloads without compile-time
         * knowledge of system-specific types, and let scripting languages
         * (e.g. Lua) marshal data automatically.
         *
         * rd_System.v1.schemata is a null-terminated array of schema strings.
         * A schema's ID is its 0-based index in the array.  The array must be
         * fully populated before retro_load_game() returns.
         *
         *
         * STRING FORMAT
         * -------------
         *
         *     name:field;field;...
         *
         * "name" is a valid C identifier.  Fields are semicolon-separated.
         * A trailing semicolon is permitted.  Whitespace around ; and :
         * is ignored.
         *
         *
         * FIELD FORMAT
         * ------------
         *
         *     TYPE NAME ["description"]          data field
         *     p[N]                               N bytes of explicit padding
         *
         * TYPE is:
         *
         *     BaseType ([!] [*] [ArraySpec])*
         *
         * BASE TYPES
         * ----------
         *
         *   u8   u16   u32   u64                 unsigned integers
         *   s8   s16   s32   s64                 signed integers
         *
         * Append "be" or "le" for explicit byte order (e.g. u16be, s32le).
         * Without a suffix the native byte order is assumed.
         *
         * ALIASES
         *
         *   str       s8[|0]*                    immutable C string pointer
         *
         * CUSTOM TYPES
         *
         * A schema name may appear as a base type.  Without a pointer modifier
         * the referenced schema must have a lower index (so its size is known
         * at parse time).  Pointer-to-custom-type has no such restriction.
         *
         * POINTER (*)
         * -----------
         *
         * A native-width pointer (typically 4 or 8 bytes depending on host).
         *
         *   u32*               pointer to u32
         *   rd_foo*            pointer to custom type rd_foo
         *
         * MUTABILITY (!)
         * --------------
         *
         * ! marks a field as mutable:
         *
         *   T!   value         mutable scalar -- handler may modify this field
         *   T!*  buf           pointer to mutable data -- handler may write
         *                      through the pointer; pointer itself is immutable
         *   T*!  ptr           mutable pointer -- handler may change the pointer
         *                      value; pointed-to data is immutable
         *   T!*! ptr           mutable pointer to mutable data -- handler may
         *                      change both the pointer and the pointed-to data
         *
         * Fields without ! should not be modified by the frontend.
         *
         *
         * ARRAY SPECIFICATIONS
         * --------------------
         *   [N]                fixed-length array of N elements
         *   [N|sym]            N slots allocated, actual count given by field "sym"
         *   [N|0]              N slots allocated, null/zero-terminated
         *   []                 variable-length (must be last field in struct)
         *   [|sym]             variable-length, count given by field "sym" (must be last)
         *   [|0]               variable-length, null/zero-terminated (must be last)
         *
         * EXAMPLES
         * --------
         *
         * u32le![2]*[16]*    pointer to 16 consecutive pointers to a pair of mutable little-endian 32-bit unsigned values.
         *
         * PSX GPU command post:
         *   "rd_psx_gpu_post:"
         *       "u8 port \"0=GP0 1=GP1\";"
         *       "u8 source \"0=CPU 2=DMA ch2\";"
         *       "u16 word_count;"
         *       "u32 pc;"
         *       "u32[16|word_count] words"
         *
         * PSX DMA transfer:
         *   "rd_psx_dma_transfer:"
         *       "u8 channel \"0=MDEC_IN 1=MDEC_OUT 2=GPU 3=CDC 4=SPU 5=CH5 6=OT\";"
         *       "u8 direction \"0=DEV->RAM 1=RAM->DEV\";"
         *       "p[2];"
         *       "u32 base_addr;"
         *       "u32 word_count;"
         *       "u32 chcr"
         *
         * PSX GTE (COP2) operation (post-op snapshot):
         *   "rd_psx_gte_op:"
         *       "u8 op \"GTE cmd: 0x01=RTPS 0x06=NCLIP 0x12=MVMVA 0x13=NCDS 0x30=RTPT\";"
         *       "u8 sf \"result shift (0 or 12)\";"
         *       "u8 lm \"clamp IR to >=0\";"
         *       "p[1];"
         *       "u32 instr \"full COP2 instruction word\";"
         *       "u32 pc;"
         *       "s16[8] sxy \"XY FIFO: x0,y0,x1,y1,x2,y2,x3,y3\";"
         *       "u16[4] sz \"Z FIFO\";"
         *       "s32[4] mac \"MAC0..3\";"
         *       "s16[4] ir \"IR0..3\""
         *
         * Mega Drive VDP register write:
         *   "rd_md_vdp_reg:u8 reg;u8 value"
         *
         * Mega Drive DMA:
         *   "rd_md_dma:"
         *       "u8 type \"0=68K->VRAM 1=Fill 2=Copy\";"
         *       "p[3];"
         *       "u32 source;"
         *       "u32 dest;"
         *       "u32 length"
         *
         * Saturn DMA transfer:
         *   "rd_saturn_dma_transfer:"
         *       "u8 level;"
         *       "u8 indirect \"0=direct 1=indirect\";"
         *       "p[2];"
         *       "u32 src_addr;"
         *       "u32 dst_addr;"
         *       "u32 byte_count;"
         *       "u8 read_add;"
         *       "u8 write_add \"0=none 1=+4 2=+2\";"
         *       "u8 src_bus \"0=A 1=B 2=C\";"
         *       "u8 dst_bus \"0=A 1=B 2=C\""
         *
         * Saturn CDB event:
         *   "rd_saturn_cdb_event:"
         *       "u8 event \"0=command 1=sector 2=DT_start 3=DT_end\";"
         *       "u8 cmd;"
         *       "p[2];"
         *       "u32 p0;"
         *       "u32 p1;"
         *       "u32 p2;"
         *       "u32 p3"
         *
         * Saturn SCSP slot state (used by proc-address, not events):
         *   "rd_ss_scsp_slot_state:"
         *       "u16 current_addr;"
         *       "u16 env_level \"0=loud 0x3FF=silent\";"
         *       "u8 env_phase \"0=Atk 1=Dec1 2=Dec2 3=Rel\";"
         *       "u8 in_loop;"
         *       "p[2]"
         *
         * Saturn SCSP key-on/off:
         *   "rd_ss_scsp_key:"
         *       "u8 slot;"
         *       "u8 key_on;"
         *       "u8 source_ctrl \"SSCTL: 0=Mem 1=Noise 2=Zero\";"
         *       "u8 wf_8bit;"
         *       "u8 loop_mode \"LPCTL: 0=Off 1=Normal 2=Rev 3=Alt\";"
         *       "p[3];"
         *       "u32 start_addr;"
         *       "u16 loop_start;"
         *       "u16 loop_end;"
         *       "u8 octave;"
         *       "p[1];"
         *       "u16 freq_num;"
         *       "u8 total_level \"0=loudest 0xFF=silent\";"
         *       "p[3]"
         *
         * NDS RTC register access (mutable fields for handler response):
         *   "rd_nds_rtc_reg:"
         *       "u8 reg;"
         *       "u8 is_read;"
         *       "u8! value;"
         *       "u8! handled"
         */

        /* Optional data-structure schemata for misc events and other untyped data.
         * Null-terminated array of schema definition strings (see "Schema
         * Format" above).  A schema's ID is its 0-based index.  Must be
         * fully populated before retro_load_game() returns.
         * (NULL if the core defines no schemata.) */
        char const* const* schemata;

        /* Write human-readable info about the loaded content (cartridge header,
           mapper, game title, checksum, etc.) into outbuff.
           outbuff may be NULL and outsize may be 0.
           Returns the number of chars that would be written for the complete output. */
        int (*get_content_info)(char* outbuff, int outsize);
    }
    v1;
}
rd_System;

/* Informs the front-end that a breakpoint was hit at the given address */
typedef struct rd_BreakpointEvent {
    rd_Cpu const* cpu;
    uint64_t address;
}
rd_BreakpointEvent;

/* Informs the front-end that a step event occurred at the given address */
typedef struct rd_StepEvent {
    rd_Cpu const* cpu;
    uint64_t address;
}
rd_StepEvent;

/* Informs the front-end that an interrupt was served */
typedef struct rd_InterruptEvent {
    rd_Cpu const* cpu;
    
    /* Identifies the type of interrupt. Meaning depends on CPU model */
    unsigned kind;
    
    /* Address of the next instruction to be executed when returning from interrupt */
    uint64_t return_address;
    
    /* New value of the program counter (in general, the start of the interrupt vector) */
    uint64_t vector_address;
}
rd_InterruptEvent;

/* Informs the front-end that a memory location is about to be written to,
   or has just been read from. */
typedef struct rd_MemoryWatchpointEvent {
    rd_Memory const* memory;
    uint64_t address;
    
    // The value about to be written, or the value just read. Endianness is that of
    // host system.
    uint64_t value;
    
    uint8_t operation; /* RD_MEMORY_READ or RD_MEMORY_WRITE */

    uint8_t width; /* typically 1, 2, 4, or 8 */
    
    // Indicates that the 'value' field can be altered by the breakpoint
    bool can_edit;
    
    // Optional. Can point to e.g. the CPU that caused this access, if applicable.
    void* accessor;
}
rd_MemoryWatchpointEvent;

/* Informs the front-end that a register is about to have its value changed */
typedef struct rd_RegisterWatchpointEvent {
    rd_Cpu const* cpu;
    unsigned reg;
    uint64_t new_value;
}
rd_RegisterWatchpointEvent;

/* Informs the front-end that a misc breakpoint was hit */
typedef struct rd_MiscBreakpointEvent {
    rd_MiscBreakpoint const* breakpoint;
    void* data;
    size_t data_size;
    int schema_id; /* index into rd_System.v1.schemata; negative = no schema */
}
rd_MiscBreakpointEvent;

/* Tagged union over all hc Event types */
typedef struct rd_Event {
    rd_EventType type;

    /* True if the core can halt execution and return from retro_run()
     * immediately. When true, the frontend may return true from
     * handle_event to request that the core break its run loop and
     * return from retro_run(); remaining events should be postponed
     * to the next retro_run() invocation.
     * When false, the core cannot break cleanly at this point, and
     * the frontend must fall back to blocking the core's thread
     * within this handler if it wishes to pause execution. */
    bool can_halt;

    union {
        rd_BreakpointEvent breakpoint;
        rd_StepEvent step;
        rd_InterruptEvent interrupt;
        rd_MemoryWatchpointEvent memory;
        rd_RegisterWatchpointEvent reg;
        rd_MiscBreakpointEvent misc;
    };
}
rd_Event;

/* Tells the core to fire RD_EVENT_BREAKPOINT when the CPU is about to execute
 * the instruction at the given address. */
typedef struct rd_BreakpointSubscription {
    rd_Cpu const* cpu;
    uint64_t address;
}
rd_BreakpointSubscription;

/* Tells the core to fire RD_EVENT_STEP on each instruction, filtered by mode.
 * The core implicitly captures the current stack depth at subscribe time
 * for modes that need it (RD_STEP_OVER, RD_STEP_OUT). */
typedef struct rd_StepSubscription {
    rd_Cpu const* cpu;
    rd_StepMode mode;
}
rd_StepSubscription;

/* Tells the core to report certain interrupt events */
typedef struct rd_InterruptSubscription {
    rd_Cpu const* cpu;
    unsigned kind;
}
rd_InterruptSubscription;

/* Tells the core to report certain memory access events */
typedef struct rd_MemoryWatchpointSubscription {
    rd_Memory const* memory;
    uint64_t address_range_begin;
    uint64_t address_range_end;
    uint8_t operation;
}
rd_MemoryWatchpointSubscription;

/* Tells the core to report certain register change events */
typedef struct rd_RegisterWatchpointSubscription {
    rd_Cpu const* cpu;
    unsigned reg;
}
rd_RegisterWatchpointSubscription;

/* Tells the core to report a misc breakpoint event */
typedef struct rd_MiscBreakpointSubscription {
    rd_MiscBreakpoint const* breakpoint;
}
rd_MiscBreakpointSubscription;

/* Informs the core that a particular type of event should be reported (via handle_event()) */
typedef struct rd_Subscription {
    rd_EventType type;
    
    union {
        rd_BreakpointSubscription breakpoint;
        rd_StepSubscription step;
        rd_InterruptSubscription interrupt;
        rd_MemoryWatchpointSubscription memory;
        rd_RegisterWatchpointSubscription reg;
        rd_MiscBreakpointSubscription misc;
    };
}
rd_Subscription;

/*
 * Debug interface. Shared between the core and frontend.
 * Members which are const are initialized by the frontend.
 *
 * The frontend allocates rd_DebuggerIf, fills in the const fields
 * (frontend_api_version, user_data, handle_event), and passes it to
 * rd_set_debugger().
 *
 * During rd_set_debugger() the core fills in:
 *   - core_api_version
 *   - v1.system   (pointer to the core's rd_System)
 *   - v1.subscribe / v1.unsubscribe
 *
 * The rd_System pointer itself is set during rd_set_debugger(), but its
 * contents (CPUs, memory regions, sizes) may continue to be updated by
 * the core until retro_load_game() returns. For example, memory region
 * sizes and the set of available regions may depend on the loaded
 * content (ROM header, MBC type, CGB mode, etc.).
 *
 * The frontend must not cache or act on system topology (region lists,
 * sizes, memory map entries) until after retro_load_game() has returned
 * successfully. For cores that do not load content, initialization should be immediate.
 */
typedef struct rd_DebuggerIf {
    unsigned const frontend_api_version;
    unsigned core_api_version;

    struct {
        /* The emulated system */
        rd_System const* system;

        /* A front-end user-defined data */
        void* const user_data;

        /* Handles an event from the core.
         * Return true to request that the core halt execution.
         * The return value is only meaningful when can_halt is true;
         * the core must ignore it when can_halt is false.
         * If can_halt is true and the handler returns true, the core
         * will break its run loop and return from retro_run() cleanly.
         * If can_halt is false, the core cannot break at this point
         * and the frontend must block the calling thread within this
         * handler if it wishes to pause execution.
         *
         * The frontend must NOT save or load state during this handler.
         * State operations are only safe after retro_run() has returned.
         *
         * The core should be prepared for the program counter to have been
         * modified when this handler returns.
         *
         * The frontend may call subscribe() and unsubscribe() from within
         * this handler (e.g. to add or remove breakpoints, or to clean up
         * temporary subscriptions). Core implementations must ensure that
         * modifying the subscription set during event dispatch is safe. */
        bool (* const handle_event)(void* frontend_user_data, rd_SubscriptionID subscription_id, rd_Event* event);

        /* Tells the core to report certain events. Returns negative value if not supported or if an error occurred.
         * Must be safe to call from within handle_event (see above). */
        rd_SubscriptionID (* subscribe)(rd_Subscription const* subscription);
        /* Must be safe to call from within handle_event (see above). */
        void (* unsubscribe)(rd_SubscriptionID subscription_id);
    }
    v1;
}
rd_DebuggerIf;

typedef void (*rd_Set)(rd_DebuggerIf* const debugger_if);

#define RD_MAKE_CPU_TYPE(id, version) ((id) << 16 | (version))
#define RD_CPU_API_VERSION(type) ((type) & 0xffffU)

/* Supported CPUs in API version 1 */
#define RD_CPU_Z80 RD_MAKE_CPU_TYPE(0, 1)

#define RD_Z80_A 0
#define RD_Z80_F 1
#define RD_Z80_BC 2
#define RD_Z80_DE 3
#define RD_Z80_HL 4
#define RD_Z80_IX 5
#define RD_Z80_IY 6
#define RD_Z80_AF2 7
#define RD_Z80_BC2 8
#define RD_Z80_DE2 9
#define RD_Z80_HL2 10
#define RD_Z80_I 11
#define RD_Z80_R 12
#define RD_Z80_SP 13
#define RD_Z80_PC 14
#define RD_Z80_IFF 15
#define RD_Z80_IM 16
#define RD_Z80_WZ 17

#define RD_Z80_NUM_REGISTERS 18

#define RD_Z80_INT 0
#define RD_Z80_NMI 1

#define RD_CPU_6502 RD_MAKE_CPU_TYPE(1, 1)

#define RD_6502_A 0
#define RD_6502_X 1
#define RD_6502_Y 2
#define RD_6502_S 3
#define RD_6502_PC 4
#define RD_6502_P 5

#define RD_6502_NUM_REGISTERS 6

#define RD_6502_NMI 0
#define RD_6502_IRQ 1

/* 6502 config flags (rd_Cpu.v1.config) */
#define RD_6502_CFG_NO_BCD  (1 << 0)  /* BCD mode disabled (e.g. NES/Famicom 2A03) */
#define RD_6502_CFG_CMOS    (1 << 1)  /* 65C02 extra instructions */

#define RD_CPU_65816 RD_MAKE_CPU_TYPE(2, 1)

#define RD_65816_A 0
#define RD_65816_X 1
#define RD_65816_Y 2
#define RD_65816_S 3
#define RD_65816_PC 4
#define RD_65816_P 5
#define RD_65816_DB 6
#define RD_65816_D 7
#define RD_65816_PB 8
#define RD_65816_EMU 9 /* 'hidden' 1-bit register, set to 1 in emulation mode, 0 in native mode */

#define RD_65816_NUM_REGISTERS 10

#define RD_CPU_R3000A RD_MAKE_CPU_TYPE(3, 1)

#define RD_R3000A_R0 0
#define RD_R3000A_AT 1
#define RD_R3000A_V0 2
#define RD_R3000A_V1 3
#define RD_R3000A_A0 4
#define RD_R3000A_A1 5
#define RD_R3000A_A2 6
#define RD_R3000A_A3 7
#define RD_R3000A_T0 8
#define RD_R3000A_T1 9
#define RD_R3000A_T2 10
#define RD_R3000A_T3 11
#define RD_R3000A_T4 12
#define RD_R3000A_T5 13
#define RD_R3000A_T6 14
#define RD_R3000A_T7 15
#define RD_R3000A_S0 16
#define RD_R3000A_S1 17
#define RD_R3000A_S2 18
#define RD_R3000A_S3 19
#define RD_R3000A_S4 20
#define RD_R3000A_S5 21
#define RD_R3000A_S6 22
#define RD_R3000A_S7 23
#define RD_R3000A_T8 24
#define RD_R3000A_T9 25
#define RD_R3000A_K0 26
#define RD_R3000A_K1 27
#define RD_R3000A_GP 28
#define RD_R3000A_SP 29
#define RD_R3000A_FP 30
#define RD_R3000A_RA 31
#define RD_R3000A_PC 32
#define RD_R3000A_LO 33
#define RD_R3000A_HI 34

#define RD_R3000A_NUM_REGISTERS 35

/* Motorola 68000 */
#define RD_CPU_M68K RD_MAKE_CPU_TYPE(4, 1)

#define RD_M68K_D0 0
#define RD_M68K_D1 1
#define RD_M68K_D2 2
#define RD_M68K_D3 3
#define RD_M68K_D4 4
#define RD_M68K_D5 5
#define RD_M68K_D6 6
#define RD_M68K_D7 7
#define RD_M68K_A0 8
#define RD_M68K_A1 9
#define RD_M68K_A2 10
#define RD_M68K_A3 11
#define RD_M68K_A4 12
#define RD_M68K_A5 13
#define RD_M68K_A6 14
#define RD_M68K_A7 15
#define RD_M68K_PC 16
#define RD_M68K_SR 17
#define RD_M68K_SSP 18
#define RD_M68K_USP 19

#define RD_M68K_NUM_REGISTERS 20

/* LR35902 (Game Boy CPU) - Sharp SM83-based CPU used in Game Boy/Color */
#define RD_CPU_LR35902 RD_MAKE_CPU_TYPE(5, 1)

/* LR35902 registers - 8-bit registers can be combined into 16-bit pairs */
#define RD_LR35902_A 0   /* Accumulator */
#define RD_LR35902_F 1   /* Flags: Z N H C (bits 7-4), bits 3-0 always 0 */
#define RD_LR35902_B 2
#define RD_LR35902_C 3
#define RD_LR35902_D 4
#define RD_LR35902_E 5
#define RD_LR35902_H 6
#define RD_LR35902_L 7
#define RD_LR35902_SP 8  /* Stack Pointer (16-bit) */
#define RD_LR35902_PC 9  /* Program Counter (16-bit) */
/* Combined 16-bit register pairs for convenience */
#define RD_LR35902_AF 10
#define RD_LR35902_BC 11
#define RD_LR35902_DE 12
#define RD_LR35902_HL 13
/* Interrupt state */
#define RD_LR35902_IME 14 /* Interrupt Master Enable */

#define RD_LR35902_NUM_REGISTERS 15

/* LR35902 interrupt types */
#define RD_LR35902_INT_VBLANK 0  /* V-Blank interrupt (INT 0x40) */
#define RD_LR35902_INT_STAT 1    /* LCD STAT interrupt (INT 0x48) */
#define RD_LR35902_INT_TIMER 2   /* Timer interrupt (INT 0x50) */
#define RD_LR35902_INT_SERIAL 3  /* Serial interrupt (INT 0x58) */
#define RD_LR35902_INT_JOYPAD 4  /* Joypad interrupt (INT 0x60) */

/* LR35902 Flag bits (in F register) */
#define RD_LR35902_FLAG_Z 0x80  /* Zero flag */
#define RD_LR35902_FLAG_N 0x40  /* Subtract flag */
#define RD_LR35902_FLAG_H 0x20  /* Half-carry flag */
#define RD_LR35902_FLAG_C 0x10  /* Carry flag */

/* SH-2 (Hitachi SuperH-2) - 32-bit RISC CPU used in Sega Saturn */
#define RD_CPU_SH2 RD_MAKE_CPU_TYPE(6, 1)

#define RD_SH2_R0  0
#define RD_SH2_R1  1
#define RD_SH2_R2  2
#define RD_SH2_R3  3
#define RD_SH2_R4  4
#define RD_SH2_R5  5
#define RD_SH2_R6  6
#define RD_SH2_R7  7
#define RD_SH2_R8  8
#define RD_SH2_R9  9
#define RD_SH2_R10 10
#define RD_SH2_R11 11
#define RD_SH2_R12 12
#define RD_SH2_R13 13
#define RD_SH2_R14 14
#define RD_SH2_R15 15  /* Stack Pointer */
#define RD_SH2_PC  16
#define RD_SH2_SR  17  /* Status Register */
#define RD_SH2_GBR 18  /* Global Base Register */
#define RD_SH2_VBR 19  /* Vector Base Register */
#define RD_SH2_MACH 20
#define RD_SH2_MACL 21
#define RD_SH2_PR  22  /* Procedure Register (return address) */

#define RD_SH2_NUM_REGISTERS 23

/* ARM (32-bit, ARMv4T through ARMv7) — covers ARM7TDMI (GBA, DS ARM7),
 * ARM946E-S (DS ARM9), ARM11 (3DS), etc.
 *
 * NOTE: AArch64 (ARMv8 64-bit) has a completely different register file
 * (X0-X30, SP, PSTATE) and instruction encoding.  It should be defined as
 * a separate RD_CPU_AARCH64 type, not reuse this one. */
#define RD_CPU_ARM RD_MAKE_CPU_TYPE(7, 1)

#define RD_ARM_R0   0
#define RD_ARM_R1   1
#define RD_ARM_R2   2
#define RD_ARM_R3   3
#define RD_ARM_R4   4
#define RD_ARM_R5   5
#define RD_ARM_R6   6
#define RD_ARM_R7   7
#define RD_ARM_R8   8
#define RD_ARM_R9   9
#define RD_ARM_R10  10
#define RD_ARM_R11  11  /* FP in some ABIs */
#define RD_ARM_R12  12  /* IP (intra-procedure scratch) */
#define RD_ARM_SP   13  /* Stack Pointer (R13) */
#define RD_ARM_LR   14  /* Link Register (R14) */
#define RD_ARM_PC   15  /* Program Counter (R15) */
#define RD_ARM_CPSR 16  /* Current Program Status Register */

#define RD_ARM_NUM_REGISTERS 17

/* ARM config (rd_Cpu.v1.config)
 *
 * Bits 0-7: architecture version (4=ARMv4, 5=ARMv5, 6=ARMv6, 7=ARMv7).
 * Bits 8+:  capability flags.
 *
 * Examples:
 *   ARM7TDMI  (GBA, DS ARM7):  4 | ARM | THUMB | MUL | LONG_MUL
 *   ARM946E-S (DS ARM9):       5 | ARM | THUMB | MUL | LONG_MUL | DSP
 *   ARM11     (3DS):           6 | ARM | THUMB | THUMB2 | MUL | LONG_MUL | DSP | VFP
 *   Cortex-M0 (v6-M):         6 | THUMB | MUL
 *   Cortex-M7 (v7-M):         7 | THUMB | THUMB2 | MUL | LONG_MUL | DSP | DIV
 *   Cortex-A7 (v7-A):         7 | ARM | THUMB | THUMB2 | MUL | LONG_MUL | DSP | DIV | VFP | NEON
 */
#define RD_ARM_CFG_VER_MASK   0xFF      /* Architecture version in bits 0-7 */
#define RD_ARM_CFG_ARM        (1 << 8)  /* ARM (32-bit) instructions */
#define RD_ARM_CFG_THUMB      (1 << 9)  /* Thumb (16-bit) instructions */
#define RD_ARM_CFG_THUMB2     (1 << 10) /* Thumb-2 (mixed 16/32-bit Thumb; requires THUMB) */
#define RD_ARM_CFG_MUL        (1 << 11) /* MUL/MLA instructions */
#define RD_ARM_CFG_LONG_MUL   (1 << 12) /* SMULL/UMULL/SMLAL/UMLAL (requires MUL) */
#define RD_ARM_CFG_DSP        (1 << 13) /* ARMv5TE DSP extensions (QADD, SMLA*, etc.) */
#define RD_ARM_CFG_DIV        (1 << 14) /* SDIV/UDIV */
#define RD_ARM_CFG_NVIC       (1 << 15) /* M-profile exception model (NVIC + hw frame push) */
#define RD_ARM_CFG_VFP        (1 << 16) /* VFP floating-point registers */
#define RD_ARM_CFG_NEON       (1 << 17) /* NEON SIMD (requires VFP) */

/* HuC6280 — the PC Engine / TurboGrafx-16 CPU.  A WDC 65C02 superset (all the
 * 65C02 RMB/SMB/BBR/BBS/STZ/etc. instructions) plus PCE-specific opcodes:
 * block transfers (TAI/TIA/TII/TDD/TIN), bank-register ops (TAM/TMA), the
 * speed switch (CSL/CSH), TST, and the VDC port writes (ST0/ST1/ST2).
 *
 * The CPU sees a 16-bit logical address space (eight 8 KB pages); each page's
 * physical bank is selected by one of the eight MPR (Memory Page Register)
 * bank registers, yielding a 21-bit (2 MB) physical space.  The base register
 * layout (A,X,Y,S,PC,P at indices 0-5) matches RD_CPU_6502; the MPR bank
 * registers and the speed/IRQ-mask/timer state follow. */
#define RD_CPU_HUC6280 RD_MAKE_CPU_TYPE(8, 1)

#define RD_HUC6280_A    0
#define RD_HUC6280_X    1
#define RD_HUC6280_Y    2
#define RD_HUC6280_S    3   /* Stack Pointer */
#define RD_HUC6280_PC   4
#define RD_HUC6280_P    5   /* Processor status */
#define RD_HUC6280_MPR0 6   /* Bank register for logical $0000-$1FFF */
#define RD_HUC6280_MPR1 7   /* ...                    $2000-$3FFF     */
#define RD_HUC6280_MPR2 8   /* ...                    $4000-$5FFF     */
#define RD_HUC6280_MPR3 9   /* ...                    $6000-$7FFF     */
#define RD_HUC6280_MPR4 10  /* ...                    $8000-$9FFF     */
#define RD_HUC6280_MPR5 11  /* ...                    $A000-$BFFF     */
#define RD_HUC6280_MPR6 12  /* ...                    $C000-$DFFF     */
#define RD_HUC6280_MPR7 13  /* ...                    $E000-$FFFF     */
#define RD_HUC6280_SPD  14  /* Speed (0=1.79MHz, 1=7.16MHz) */
#define RD_HUC6280_IRQM 15  /* IRQ mask */
#define RD_HUC6280_TIMS 16  /* Timer status (enabled) */
#define RD_HUC6280_TIMV 17  /* Timer value */
#define RD_HUC6280_TIML 18  /* Timer reload */
#define RD_HUC6280_TIMD 19  /* Timer divider */

#define RD_HUC6280_NUM_REGISTERS 20

/* HuC6280 interrupt kinds (vectors: IRQ1=$FFF8, IRQ2=$FFF6, TIMER=$FFFA) */
#define RD_HUC6280_IRQ2  0
#define RD_HUC6280_IRQ1  1
#define RD_HUC6280_TIMER 2

/* HuC6280 P (status) flag bits */
#define RD_HUC6280_FLAG_N 0x80  /* Negative */
#define RD_HUC6280_FLAG_V 0x40  /* Overflow */
#define RD_HUC6280_FLAG_T 0x20  /* Memory operation (HuC6280-specific) */
#define RD_HUC6280_FLAG_B 0x10  /* Break */
#define RD_HUC6280_FLAG_D 0x08  /* Decimal */
#define RD_HUC6280_FLAG_I 0x04  /* IRQ disable */
#define RD_HUC6280_FLAG_Z 0x02  /* Zero */
#define RD_HUC6280_FLAG_C 0x01  /* Carry */

#endif /* RETRO_DEBUG__ */

