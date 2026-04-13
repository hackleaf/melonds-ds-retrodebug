/*
 * retrodebug_nds.h: Forward declarations for NDS retrodebug integration
 */

#ifndef RETRODEBUG_NDS_H
#define RETRODEBUG_NDS_H

#ifdef __cplusplus
struct rd_DebuggerIf;

void rd_nds_set_debugger(rd_DebuggerIf *dif);
void rd_nds_init(void *nds_ptr);
void rd_nds_frame_start(void);
void rd_nds_deinit(void);
#endif

#endif /* RETRODEBUG_NDS_H */
