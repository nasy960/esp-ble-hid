#pragma once
#include <stdint.h>
#include <stdbool.h>
// Host-task only; one managed B connection. Name probe owns the 2s deadline.
void b_auth_gate_begin(uint16_t handle);
void b_auth_gate_cancel(uint16_t handle);
void b_auth_gate_reset(void);
bool b_auth_gate_resume(uint16_t handle);
