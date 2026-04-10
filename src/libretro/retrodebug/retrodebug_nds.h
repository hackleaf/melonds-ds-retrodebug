/*
 * retrodebug_nds.h: NDS-specific retrodebug data structures
 *
 * Shared between NDS libretro cores and the frontend.
 * Passed via rd_MiscBreakpointEvent.data for RTC register events.
 */

#ifndef RETRODEBUG_NDS_H
#define RETRODEBUG_NDS_H

#include <stdint.h>

/*
 * RTC register access event.
 * Fired when the ARM7 reads or writes an RTC register via the SPI protocol.
 *
 * For reads: the handler may modify 'value' to provide a custom response.
 * For writes: 'value' contains the byte written by the ARM7.
 *
 * MiscBreakpoint description: "RTC_REG"
 */
typedef struct rd_nds_rtc_reg_event {
    uint8_t  reg;        /* RTC register command byte (e.g. 0x6D = read reg 112) */
    uint8_t  is_read;    /* 1 = read (output), 0 = write (input) */
    uint8_t  value;      /* the byte value; writable for reads */
    uint8_t  handled;    /* set to 1 by the handler to indicate value was provided */
} rd_nds_rtc_reg_event;

#define RD_NDS_MISC_RTC_REG "RTC_REG"

/* Forward declarations for retrodebug NDS integration */
#ifdef __cplusplus
struct rd_DebuggerIf;

void rd_nds_set_debugger(rd_DebuggerIf *dif);
void rd_nds_init(void *nds_ptr);
void rd_nds_frame_start(void);
void rd_nds_deinit(void);
#endif

#endif /* RETRODEBUG_NDS_H */
