#pragma once

#include <unistd.h>

// Return number of CPUs in computer
int system_ncpu(void);
//void *threads_info_thread(void *ptr);
void threads_info_thread(float limit, pid_t pid);