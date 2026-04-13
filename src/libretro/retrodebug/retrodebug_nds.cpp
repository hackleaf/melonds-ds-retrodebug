/*
 * retrodebug_nds.cpp: NDS retrodebug integration for melonDS DS
 *
 * Exposes the ARM9 and ARM7 CPUs and their memory buses via the
 * retrodebug API so that arret-debugger can set breakpoints,
 * read/write registers, and inspect memory.
 */

#include "retrodebug.h"

/* Local struct definition — layout must match schema below.
 *
 * The schema uses natural alignment: u8 fields are 1-byte, u32 is
 * 4-byte aligned. We pad explicitly so the C layout matches. */
struct rd_nds_rtc_reg_event {
    uint8_t  reg;
    uint8_t  is_read;
    uint8_t  value;
    uint8_t  handled;
    uint32_t raise_irq;  /* mask of ARM7 IF bits to raise if handled */
    uint8_t  byte_index; /* output byte index for multi-byte reads */
    uint8_t  _pad[3];
};

enum {
    RD_SCHEMA_NDS_RTC_REG = 0,
};

static const char *rd_schemata[] = {
    "rd_nds_rtc_reg:"
        "u8 reg;"
        "u8 is_read;"
        "u8! value;"
        "u8! handled;"
        "u32! raise_irq;"
        "u8 byte_index;"
        "p[3]",
    nullptr,
};

#include <cstring>
#include <cstdio>
#include <vector>
#include <NDS.h>
#include <ARM.h>
#include <RTC.h>

using namespace melonDS;

/* ======================================================================== */
/* State                                                                     */
/* ======================================================================== */

static NDS *g_nds = nullptr;
static rd_DebuggerIf *g_dif = nullptr;

/* Subscription tracking */
struct ExecSub {
    rd_SubscriptionID id;
    int cpu_index;      /* 0 = arm9, 1 = arm7, -1 = any */
    uint64_t address;   /* breakpoint address */
};
struct StepSub {
    rd_SubscriptionID id;
    int cpu_index;
    rd_StepMode mode;
    bool fired;         /* set after first instruction (for STEP_INTO) */
};
static std::vector<ExecSub> g_exec_subs;
static std::vector<StepSub> g_step_subs;
static rd_SubscriptionID g_next_sub_id = 1;

/* ======================================================================== */
/* Memory: ARM9 bus                                                          */
/* ======================================================================== */

static uint64_t arm9_mem_peek(rd_Memory const *, uint64_t addr, uint64_t size,
                              uint8_t *buf, bool)
{
    if (!g_nds) return 0;
    for (uint64_t i = 0; i < size; i++)
        buf[i] = g_nds->ARM9Read8((u32)(addr + i));
    return size;
}

static uint64_t arm9_mem_poke(rd_Memory const *, uint64_t addr, uint64_t size,
                              const uint8_t *buf)
{
    if (!g_nds) return 0;
    for (uint64_t i = 0; i < size; i++)
        g_nds->ARM9Write8((u32)(addr + i), buf[i]);
    return size;
}

static const rd_Memory rd_mem_arm9 = { .v1 = {
    .id = "arm9",
    .description = "ARM9 Address Space",
    .alignment = 1,
    .size = 0x100000000ULL,
    .break_points = nullptr,
    .peek = arm9_mem_peek,
    .poke = arm9_mem_poke,
    .get_memory_map_count = nullptr,
    .get_memory_map = nullptr,
    .get_bank_address = nullptr,
    .cache_probe = nullptr,
}};

/* ======================================================================== */
/* Memory: ARM7 bus                                                          */
/* ======================================================================== */

static uint64_t arm7_mem_peek(rd_Memory const *, uint64_t addr, uint64_t size,
                              uint8_t *buf, bool)
{
    if (!g_nds) return 0;
    for (uint64_t i = 0; i < size; i++)
        buf[i] = g_nds->ARM7Read8((u32)(addr + i));
    return size;
}

static uint64_t arm7_mem_poke(rd_Memory const *, uint64_t addr, uint64_t size,
                              const uint8_t *buf)
{
    if (!g_nds) return 0;
    for (uint64_t i = 0; i < size; i++)
        g_nds->ARM7Write8((u32)(addr + i), buf[i]);
    return size;
}

static const rd_Memory rd_mem_arm7 = { .v1 = {
    .id = "arm7",
    .description = "ARM7 Address Space",
    .alignment = 1,
    .size = 0x100000000ULL,
    .break_points = nullptr,
    .peek = arm7_mem_peek,
    .poke = arm7_mem_poke,
    .get_memory_map_count = nullptr,
    .get_memory_map = nullptr,
    .get_bank_address = nullptr,
    .cache_probe = nullptr,
}};

/* ======================================================================== */
/* Memory: Main RAM                                                          */
/* ======================================================================== */

static uint64_t mainram_peek(rd_Memory const *, uint64_t addr, uint64_t size,
                             uint8_t *buf, bool)
{
    if (!g_nds || !g_nds->MainRAM) return 0;
    uint32_t mask = g_nds->MainRAMMask;
    for (uint64_t i = 0; i < size; i++)
        buf[i] = g_nds->MainRAM[(addr + i) & mask];
    return size;
}

static uint64_t mainram_poke(rd_Memory const *, uint64_t addr, uint64_t size,
                             const uint8_t *buf)
{
    if (!g_nds || !g_nds->MainRAM) return 0;
    uint32_t mask = g_nds->MainRAMMask;
    for (uint64_t i = 0; i < size; i++)
        g_nds->MainRAM[(addr + i) & mask] = buf[i];
    return size;
}

static rd_Memory rd_mem_mainram = { .v1 = {
    .id = "mainram",
    .description = "Main RAM",
    .alignment = 1,
    .size = 0,
    .break_points = nullptr,
    .peek = mainram_peek,
    .poke = mainram_poke,
    .get_memory_map_count = nullptr,
    .get_memory_map = nullptr,
    .get_bank_address = nullptr,
    .cache_probe = nullptr,
}};

/* ======================================================================== */
/* Registers                                                                 */
/* ======================================================================== */

static uint64_t arm9_get_register(rd_Cpu const *, unsigned reg)
{
    if (!g_nds) return 0;
    if (reg <= RD_ARM_R12) return g_nds->ARM9.R[reg];
    if (reg == RD_ARM_SP) return g_nds->ARM9.R[13];
    if (reg == RD_ARM_LR) return g_nds->ARM9.R[14];
    if (reg == RD_ARM_PC) return g_nds->ARM9.R[15];
    if (reg == RD_ARM_CPSR) return g_nds->ARM9.CPSR;
    return 0;
}

static int arm9_set_register(rd_Cpu const *, unsigned reg, uint64_t value)
{
    if (!g_nds) return 0;
    if (reg <= RD_ARM_R12) { g_nds->ARM9.R[reg] = (u32)value; return 1; }
    if (reg == RD_ARM_SP) { g_nds->ARM9.R[13] = (u32)value; return 1; }
    if (reg == RD_ARM_LR) { g_nds->ARM9.R[14] = (u32)value; return 1; }
    if (reg == RD_ARM_PC) { g_nds->ARM9.R[15] = (u32)value; return 1; }
    if (reg == RD_ARM_CPSR) { g_nds->ARM9.CPSR = (u32)value; return 1; }
    return 0;
}

static uint64_t arm7_get_register(rd_Cpu const *, unsigned reg)
{
    if (!g_nds) return 0;
    if (reg <= RD_ARM_R12) return g_nds->ARM7.R[reg];
    if (reg == RD_ARM_SP) return g_nds->ARM7.R[13];
    if (reg == RD_ARM_LR) return g_nds->ARM7.R[14];
    if (reg == RD_ARM_PC) return g_nds->ARM7.R[15];
    if (reg == RD_ARM_CPSR) return g_nds->ARM7.CPSR;
    return 0;
}

static int arm7_set_register(rd_Cpu const *, unsigned reg, uint64_t value)
{
    if (!g_nds) return 0;
    if (reg <= RD_ARM_R12) { g_nds->ARM7.R[reg] = (u32)value; return 1; }
    if (reg == RD_ARM_SP) { g_nds->ARM7.R[13] = (u32)value; return 1; }
    if (reg == RD_ARM_LR) { g_nds->ARM7.R[14] = (u32)value; return 1; }
    if (reg == RD_ARM_PC) { g_nds->ARM7.R[15] = (u32)value; return 1; }
    if (reg == RD_ARM_CPSR) { g_nds->ARM7.CPSR = (u32)value; return 1; }
    return 0;
}

/* ======================================================================== */
/* Pipeline                                                                  */
/* ======================================================================== */

/*
 * ARM's PC register (R15) reads as current_instruction + 8 (ARM mode)
 * or current_instruction + 4 (Thumb mode).  This is architecturally
 * defined, not a microarchitectural detail.
 *
 * In melonDS's interpreter, R[15] is incremented per-stage.  At the
 * point where the debug hook fires (and when halted), R[15] is 2
 * instructions ahead: +4 in ARM mode, +2 in Thumb16 mode.  The
 * execution hook patch (arm-retrodebug-hook.patch) already subtracts
 * this to produce the instruction address for breakpoint matching.
 *
 * delay=0: the address of the instruction being executed
 */
static bool arm9_pipeline_get_delay_pc(rd_Cpu const *, unsigned delay, uint64_t *out_pc)
{
    if (!g_nds || !out_pc || delay > 0) return false;
    u32 pc = g_nds->ARM9.R[15];
    u32 offset = (g_nds->ARM9.CPSR & 0x20) ? 2 : 4;
    *out_pc = pc - offset;
    return true;
}

static bool arm7_pipeline_get_delay_pc(rd_Cpu const *, unsigned delay, uint64_t *out_pc)
{
    if (!g_nds || !out_pc || delay > 0) return false;
    u32 pc = g_nds->ARM7.R[15];
    u32 offset = (g_nds->ARM7.CPSR & 0x20) ? 2 : 4;
    *out_pc = pc - offset;
    return true;
}

/* ======================================================================== */
/* CPU descriptors                                                           */
/* ======================================================================== */

#define ARM9_CONFIG ( \
    5 | RD_ARM_CFG_ARM | RD_ARM_CFG_THUMB | \
    RD_ARM_CFG_MUL | RD_ARM_CFG_LONG_MUL | RD_ARM_CFG_DSP)

#define ARM7_CONFIG ( \
    4 | RD_ARM_CFG_ARM | RD_ARM_CFG_THUMB | \
    RD_ARM_CFG_MUL | RD_ARM_CFG_LONG_MUL)

static const rd_Cpu rd_cpu_arm9 = { .v1 = {
    .id = "arm9",
    .description = "ARM946E-S",
    .type = RD_CPU_ARM,
    .config = ARM9_CONFIG,
    .memory_region = &rd_mem_arm9,
    .break_points = nullptr,
    .get_register = arm9_get_register,
    .set_register = arm9_set_register,
    .pipeline_get_delay_pc = arm9_pipeline_get_delay_pc,
}};

static const rd_Cpu rd_cpu_arm7 = { .v1 = {
    .id = "arm7",
    .description = "ARM7TDMI",
    .type = RD_CPU_ARM,
    .config = ARM7_CONFIG,
    .memory_region = &rd_mem_arm7,
    .break_points = nullptr,
    .get_register = arm7_get_register,
    .set_register = arm7_set_register,
    .pipeline_get_delay_pc = arm7_pipeline_get_delay_pc,
}};

/* ======================================================================== */
/* RTC register misc breakpoint                                              */
/* ======================================================================== */

static const rd_MiscBreakpoint rd_misc_rtc_reg = {
    .v1 = {
        .id = "rtc_reg",
        .description = "RTC register access",
    },
};

static const rd_MiscBreakpoint *rd_misc_bps[] = { &rd_misc_rtc_reg, nullptr };

struct MiscSub {
    rd_SubscriptionID id;
    const rd_MiscBreakpoint *bp;
};
static std::vector<MiscSub> g_misc_subs;

/* Controls whether every RTC byte is logged to stderr. Very noisy —
 * enable only when debugging the RTCom handshake itself. */
static bool g_rtc_trace = false;

/* 8-bit reverse lookup. NDS RTC SPI is bit-banged MSB-first by the sender
 * but melonDS's accumulator stores LSB-first, so every byte our hook gets
 * is the bit-reversal of the logical opcode (e.g., what the protocol docs
 * call 0x6E arrives as 0x76). Reverse once here so subscribers work with
 * the documented opcode number. */
static inline uint8_t rtc_bitrev8(uint8_t b)
{
    b = (b >> 4) | (b << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

/* RTC callback — called from melonDS RTC::ByteIn/CmdRead/CmdWrite.
 * Gives subscribers a chance to intercept RTC SPI byte traffic.
 * On a handled read, the substituted value is written back through *value.
 * On any handled access, if rtc_ev.raise_irq is non-zero, those bits are
 * OR'd into ARM7's IF so the next ARM dispatch services them. */
static bool rtc_reg_access(void *, u8 cmd, bool is_read, u8 *value)
{
    /* NDS RTC SPI is bit-banged MSB-first on the wire, but melonDS's
     * input accumulator and output shifter both use LSB-first bit order
     * (they mirror the wire), so every byte (both the cmd byte and the
     * data bytes) arrives at this hook bit-reversed relative to what the
     * protocol docs / client code call it. Reverse both directions so
     * subscribers see the logical values, and reverse the substituted
     * read value back on the way out. */
    u8 logical_cmd = rtc_bitrev8(cmd);
    u8 logical_val = value ? rtc_bitrev8(*value) : 0;

    if (g_rtc_trace) {
        fprintf(stderr, "[melonDS-rd] rtc cmd=0x%02X %s val=0x%02X (wire cmd=0x%02X val=0x%02X) subs=%zu\n",
                logical_cmd, is_read ? "R" : "W", logical_val,
                cmd, value ? *value : 0, g_misc_subs.size());
    }

    if (!g_dif || !g_dif->v1.handle_event) return false;

    /* Check if anyone subscribed to rtc_reg */
    for (auto &sub : g_misc_subs) {
        if (sub.bp != &rd_misc_rtc_reg) continue;

        rd_nds_rtc_reg_event rtc_ev = {};
        rtc_ev.reg = logical_cmd;  /* logical opcode */
        rtc_ev.is_read = is_read ? 1 : 0;
        rtc_ev.value = logical_val;  /* logical byte value */
        rtc_ev.handled = 0;
        rtc_ev.raise_irq = 0;
        rtc_ev.byte_index = 0;

        rd_Event event = {};
        event.type = RD_EVENT_MISC;
        event.can_halt = false;
        event.misc.breakpoint = &rd_misc_rtc_reg;
        event.misc.data = &rtc_ev;
        event.misc.data_size = sizeof(rtc_ev);
        event.misc.schema_id = RD_SCHEMA_NDS_RTC_REG;

        g_dif->v1.handle_event(nullptr, sub.id, &event);

        if (!rtc_ev.handled) continue;

        if (is_read && value)
            *value = rtc_bitrev8(rtc_ev.value);

        /* Multi-byte reads: the core only invokes our hook once per RTC
         * transaction (at the cmd-byte phase). For opcodes that produce
         * more than one output byte (e.g. 0x6B alarm-time-2 = 2 bytes,
         * 0x71 counter-ext = 3 bytes), populate Output[1..] directly
         * via the core's SetHookOutputByte helper by re-invoking the
         * subscriber with byte_index=1,2,... until it returns handled=0
         * or we hit the 8-byte Output buffer cap. */
        if (is_read && g_nds) {
            for (unsigned i = 1; i < 8; i++) {
                rd_nds_rtc_reg_event ev_n = {};
                ev_n.reg = logical_cmd;
                ev_n.is_read = 1;
                ev_n.value = 0;
                ev_n.handled = 0;
                ev_n.raise_irq = 0;
                ev_n.byte_index = (uint8_t)i;

                rd_Event e_n = {};
                e_n.type = RD_EVENT_MISC;
                e_n.can_halt = false;
                e_n.misc.breakpoint = &rd_misc_rtc_reg;
                e_n.misc.data = &ev_n;
                e_n.misc.data_size = sizeof(ev_n);
                e_n.misc.schema_id = RD_SCHEMA_NDS_RTC_REG;

                g_dif->v1.handle_event(nullptr, sub.id, &e_n);
                if (!ev_n.handled) break;
                g_nds->RTC.SetHookOutputByte((u8)i, rtc_bitrev8(ev_n.value));
            }
        }

        /* Honour IRQ request. raise_irq is a mask of ARM7 IF bits
         * (e.g. 0x01000000 = IRQ_NETWORK). Walk set bits and call
         * SetIRQ(cpu=1, bit_index), which is the core's API for
         * raising a hardware IRQ line. */
        if (rtc_ev.raise_irq && g_nds) {
            if (g_rtc_trace) {
                fprintf(stderr, "[melonDS-rd] raise_irq mask=0x%08X\n",
                        rtc_ev.raise_irq);
            }
            uint32_t mask = rtc_ev.raise_irq;
            while (mask) {
                int bit = __builtin_ctz(mask);
                mask &= mask - 1;
                g_nds->SetIRQ(1, bit);
            }
        }

        return true;
    }
    return false;
}

/* ======================================================================== */
/* System descriptor                                                         */
/* ======================================================================== */

static const rd_Cpu *rd_cpus[] = { &rd_cpu_arm9, &rd_cpu_arm7, nullptr };
static const rd_Memory *rd_extra_mem[] = { &rd_mem_mainram, nullptr };

static const rd_System rd_system = { .v1 = {
    .id = "nds",
    .cpus = rd_cpus,
    .memory_regions = rd_extra_mem,
    .filesystems = nullptr,
    .break_points = rd_misc_bps,
    .schemata = rd_schemata,
}};

/* ======================================================================== */
/* ARM execution hook — called from patched ARM.cpp                          */
/* ======================================================================== */

static bool arm_exec_hook(void *userdata, u32 addr, bool thumb)
{
    (void)thumb;
    int cpu_index = (int)(intptr_t)userdata;  /* 0 = arm9, 1 = arm7 */

    if (!g_dif || !g_dif->v1.handle_event) return false;

    /* If no subscriptions active, nothing to do */
    if (g_step_subs.empty() && g_exec_subs.empty()) return false;

    rd_Cpu const *cpu = (cpu_index == 0) ? &rd_cpu_arm9 : &rd_cpu_arm7;

    /* Check step subscriptions first */
    for (auto it = g_step_subs.begin(); it != g_step_subs.end(); ) {
        if (it->cpu_index >= 0 && it->cpu_index != cpu_index) { ++it; continue; }

        if (it->mode == RD_STEP_INTO || it->mode == RD_STEP_INTO_SKIP_IRQ) {
            rd_Event event = {};
            event.type = RD_EVENT_STEP;
            event.can_halt = false;
            event.step.cpu = cpu;
            event.step.address = addr;

            rd_SubscriptionID sid = it->id;

            /* handle_event may block this thread (can_halt=false) or
             * return immediately if the step is suppressed (e.g. skip address).
             * Don't erase the sub here — arret will unsubscribe it when done. */
            g_dif->v1.handle_event(nullptr, sid, &event);
            return false;  /* continue execution after resume/suppress */
        }
        ++it;
    }

    /* Check breakpoint subscriptions */
    for (auto &sub : g_exec_subs) {
        if (sub.address == (uint64_t)addr &&
            (sub.cpu_index < 0 || sub.cpu_index == cpu_index))
        {
            rd_Event event = {};
            event.type = RD_EVENT_BREAKPOINT;
            event.can_halt = false;
            event.breakpoint.cpu = cpu;
            event.breakpoint.address = addr;

            /* handle_event blocks this thread until frontend resumes */
            g_dif->v1.handle_event(nullptr, sub.id, &event);
            return false;
        }
    }
    return false;
}

/* ======================================================================== */
/* Subscribe / Unsubscribe                                                   */
/* ======================================================================== */

static rd_SubscriptionID nds_subscribe(rd_Subscription const *sub)
{
    if (!sub) return -1;

    if (sub->type == RD_EVENT_BREAKPOINT) {
        ExecSub es;
        es.id = g_next_sub_id++;
        es.address = sub->breakpoint.address;

        es.cpu_index = -1;
        if (sub->breakpoint.cpu == &rd_cpu_arm9) es.cpu_index = 0;
        else if (sub->breakpoint.cpu == &rd_cpu_arm7) es.cpu_index = 1;

        g_exec_subs.push_back(es);
        fprintf(stderr, "[melonDS-rd] subscribed exec bp %ld at 0x%08lX (cpu %d)\n",
                (long)es.id, (unsigned long)es.address, es.cpu_index);
        return es.id;
    }

    if (sub->type == RD_EVENT_MISC) {
        /* Match by breakpoint pointer */
        if (sub->misc.breakpoint == &rd_misc_rtc_reg) {
            MiscSub ms;
            ms.id = g_next_sub_id++;
            ms.bp = &rd_misc_rtc_reg;
            g_misc_subs.push_back(ms);
            fprintf(stderr, "[melonDS-rd] subscribed RTC_REG misc bp %ld\n", (long)ms.id);
            return ms.id;
        }
        return -1;
    }

    if (sub->type == RD_EVENT_STEP) {
        StepSub ss;
        ss.id = g_next_sub_id++;
        ss.mode = sub->step.mode;
        ss.fired = false;

        ss.cpu_index = -1;
        if (sub->step.cpu == &rd_cpu_arm9) ss.cpu_index = 0;
        else if (sub->step.cpu == &rd_cpu_arm7) ss.cpu_index = 1;

        g_step_subs.push_back(ss);
        fprintf(stderr, "[melonDS-rd] subscribed step %ld (cpu %d, mode %d)\n",
                (long)ss.id, ss.cpu_index, (int)ss.mode);
        return ss.id;
    }

    return -1;
}

static void nds_unsubscribe(rd_SubscriptionID id)
{
    for (auto it = g_exec_subs.begin(); it != g_exec_subs.end(); ++it) {
        if (it->id == id) { g_exec_subs.erase(it); return; }
    }
    for (auto it = g_step_subs.begin(); it != g_step_subs.end(); ++it) {
        if (it->id == id) { g_step_subs.erase(it); return; }
    }
    for (auto it = g_misc_subs.begin(); it != g_misc_subs.end(); ++it) {
        if (it->id == id) { g_misc_subs.erase(it); return; }
    }
}

/* ======================================================================== */
/* Public API                                                                */
/* ======================================================================== */

void rd_nds_set_debugger(rd_DebuggerIf *dif)
{
    g_dif = dif;
    if (!dif) return;

    dif->v1.system = &rd_system;
    dif->v1.subscribe = nds_subscribe;
    dif->v1.unsubscribe = nds_unsubscribe;
}

void rd_nds_init(void *nds_ptr)
{
    g_nds = static_cast<NDS *>(nds_ptr);

    rd_mem_mainram.v1.size = g_nds ? (g_nds->MainRAMMask + 1) : 0;

    /* Install execution hooks on both CPUs */
    if (g_nds) {
        g_nds->ARM9.RetroDebugHook = arm_exec_hook;
        g_nds->ARM9.RetroDebugUserData = (void *)(intptr_t)0;
        g_nds->ARM9.RetroDebugHalt = false;

        g_nds->ARM7.RetroDebugHook = arm_exec_hook;
        g_nds->ARM7.RetroDebugUserData = (void *)(intptr_t)1;
        g_nds->ARM7.RetroDebugHalt = false;

        /* Install RTC register access hook */
        g_nds->RTC.OnRegAccess = rtc_reg_access;
        g_nds->RTC.OnRegAccessUserData = nullptr;

        fprintf(stderr, "[melonDS-rd] execution hooks and RTC hook installed\n");
    }
}

void rd_nds_frame_start(void)
{
    if (g_nds) {
        g_nds->ARM9.RetroDebugHalt = false;
        g_nds->ARM7.RetroDebugHalt = false;
    }
}

void rd_nds_deinit(void)
{
    if (g_nds) {
        g_nds->ARM9.RetroDebugHook = nullptr;
        g_nds->ARM7.RetroDebugHook = nullptr;
    }
    if (g_nds) {
        g_nds->ARM9.RetroDebugHook = nullptr;
        g_nds->ARM7.RetroDebugHook = nullptr;
        g_nds->RTC.OnRegAccess = nullptr;
    }
    g_nds = nullptr;
    g_dif = nullptr;
    g_exec_subs.clear();
    g_step_subs.clear();
    g_misc_subs.clear();
    g_next_sub_id = 1;
}
