#pragma once

#include "shared_data.h"

namespace overlay {
	bool init();
	void run(SharedState& state);
	void shutdown();
}
