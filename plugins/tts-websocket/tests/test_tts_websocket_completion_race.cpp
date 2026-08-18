#include "tts_websocket_completion.h"

#include <cassert>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

static void test_mpf_and_watchdog_race_is_exactly_once()
{
	tts_websocket_completion_t completion;
	std::mutex mutex;
	std::vector<std::thread> threads;
	uint64_t generation;
	int claim_count = 0;

	tts_websocket_completion_init(&completion);
	generation = tts_websocket_completion_begin(&completion);
	for(int i = 0; i < 32; ++i) {
		threads.emplace_back([&, i]() {
			std::lock_guard<std::mutex> lock(mutex);
			auto owner = i % 2
				? TTS_WEBSOCKET_COMPLETION_OWNER_MPF
				: TTS_WEBSOCKET_COMPLETION_OWNER_WATCHDOG;
			if(tts_websocket_completion_claim(&completion, generation, owner)) {
				++claim_count;
			}
		});
	}
	for(auto &thread : threads) {
		thread.join();
	}
	assert(claim_count == 1);
}

int main()
{
	test_mpf_and_watchdog_race_is_exactly_once();
	std::cout << "tts_websocket_completion_race tests passed\n";
	return 0;
}
