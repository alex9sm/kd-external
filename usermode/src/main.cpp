#include <cstdio>
#include <thread>

#include "gui.h"
#include "read_thread.h"
#include "shared_data.h"

int main() {
	SharedState state;

	std::thread reader(read_thread_func, std::ref(state));

	if (!overlay::init()) {
		puts("overlay init failed");
		state.running = false;
		reader.join();
		return 1;
	}

	overlay::run(state);

	state.running = false;
	reader.join();
	overlay::shutdown();
	return 0;
}
