#pragma once

#include <windows.h>

#include "shared_data.h"

DWORD get_pid();

void read_thread_func(SharedState& state);
