/*
 * Lab 03 - shared message layout between M55 (listener) and M4
 * (accelerometer monitor). Only one message type ever flows in this lab
 * (M4 -> M55: "the motion state just changed"), so unlike a
 * general-purpose protocol there is no type/command tag here.
 */
#ifndef IPC_COMMON_H_
#define IPC_COMMON_H_

#include <stdint.h>

enum motion_state {
	MOTION_STATE_STILL = 0,
	MOTION_STATE_ACTIVE = 1,
};

struct ipc_motion_event {
	uint32_t seq;          /* event sequence number, starts at 1 */
	uint8_t state;         /* enum motion_state: the state just entered */
	int32_t deviation_mg;  /* |dx|+|dy|+|dz| from the baseline, in
				 * milli-g, at the moment the state changed */
};

#endif /* IPC_COMMON_H_ */
