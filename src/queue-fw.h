/* SPDX-License-Identifier: MIT */
#ifndef AVD_QUEUE_FW_H
#define AVD_QUEUE_FW_H
void avdq_fw_init(void);
int avdq_fw_mailbox(unsigned int token);
int avdq_fw_event(unsigned int vp, unsigned int event);
int avdq_fw_poll(void);
void avdq_fw_idle(void);
void avdq_fw_full_mask(unsigned int vp, unsigned int enable);
#endif
