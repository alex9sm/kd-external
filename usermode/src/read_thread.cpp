#include "read_thread.h"

#include <cstdint>
#include <cstdio>
#include <vector>

#include "driver_client.h"

#include "../vendor/offsets/offsets.hpp"
#include "../vendor/offsets/client_dll.hpp"

namespace off = exitscam::offsets::client_dll;
namespace sch = exitscam::schemas::client_dll;

constexpr uint64_t OFF_LOCAL_PLAYER = static_cast<uint64_t>(off::dwLocalPlayerPawn);
constexpr uint64_t OFF_ENTITY_LIST  = static_cast<uint64_t>(off::dwEntityList);
constexpr uint64_t OFF_VIEW_MATRIX  = static_cast<uint64_t>(off::dwViewMatrix);
constexpr uint64_t OFF_TEAM         = static_cast<uint64_t>(sch::C_BaseEntity::m_iTeamNum);
constexpr uint64_t OFF_LIFE_STATE   = static_cast<uint64_t>(sch::C_BaseEntity::m_lifeState);
constexpr uint64_t OFF_PLAYER_PAWN  = static_cast<uint64_t>(sch::CCSPlayerController::m_hPlayerPawn);
constexpr uint64_t OFF_V_OLD_ORIGIN = static_cast<uint64_t>(sch::C_BasePlayerPawn::m_vOldOrigin);
constexpr uint64_t OFF_HEALTH       = static_cast<uint64_t>(sch::C_BaseEntity::m_iHealth);

DWORD get_pid() {
	HWND handle = FindWindowA("SDL_app", "Counter-Strike 2");
	if (handle == nullptr) {
		puts("CS2 not running.\n");
		return 0;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(handle, &pid);
	return pid;
}

void read_thread_func(SharedState& state) {
	DWORD pid = get_pid();
	if (!pid) return;

	HANDLE driver_handle = CreateFile(L"\\\\.\\io-driver", GENERIC_READ, 0, nullptr,
									  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (driver_handle == INVALID_HANDLE_VALUE) {
		puts("failed to get driver handle.\n");
		return;
	}

	if (!driver::attach_to_process(driver_handle, pid)) {
		puts("attach failed.\n");
		CloseHandle(driver_handle);
		return;
	}

	uint64_t base = driver::get_module_base(driver_handle, L"client.dll");
	if (!base) {
		puts("get_module_base(client.dll) failed.\n");
		CloseHandle(driver_handle);
		return;
	}

	constexpr uint64_t PAWN_READ_START = OFF_HEALTH;
	constexpr size_t   PAWN_BUF_SIZE   = (OFF_V_OLD_ORIGIN + sizeof(vec3)) - PAWN_READ_START;

	std::vector<EnemyData> frame_enemies;
	uint8_t pawn_buf[PAWN_BUF_SIZE];

	while (state.running) {
		uint64_t entity_list = driver::read_memory<uint64_t>(driver_handle, base + OFF_ENTITY_LIST);
		uint64_t local_pawn = driver::read_memory<uint64_t>(driver_handle, base + OFF_LOCAL_PLAYER);
		view_matrix_t vm = driver::read_memory<view_matrix_t>(driver_handle, base + OFF_VIEW_MATRIX);

		frame_enemies.clear();

		if (entity_list && local_pawn) {
			int local_team = driver::read_memory<int>(driver_handle, local_pawn + OFF_TEAM);

			uint64_t chunk_cache[4] = {};
			bool     chunk_valid[4] = {};

			auto get_chunk = [&](uint32_t ci) -> uint64_t {
				if (ci >= 4) return 0;
				if (!chunk_valid[ci]) {
					chunk_cache[ci] = driver::read_memory<uint64_t>(driver_handle, entity_list + 8 * ci + 16);
					chunk_valid[ci] = true;
				}
				return chunk_cache[ci];
			};

			auto get_entity = [&](uint32_t idx) -> uint64_t {
				uint64_t chunk = get_chunk(idx >> 9);
				if (!chunk) return 0;
				return driver::read_memory<uint64_t>(driver_handle, chunk + 0x70 * (idx & 0x1FF));
			};

			for (int i = 1; i <= 64; i++) {
				uint64_t controller = get_entity(i);
				if (!controller) continue;

				uint32_t pawn_handle = driver::read_memory<uint32_t>(driver_handle, controller + OFF_PLAYER_PAWN);
				uint64_t pawn = get_entity(pawn_handle & 0x7FFF);
				if (!pawn || pawn == local_pawn) continue;

				if (!driver::read_raw(driver_handle, pawn + PAWN_READ_START, pawn_buf, PAWN_BUF_SIZE))
					continue;

				int team = *(int*)(pawn_buf + OFF_TEAM - PAWN_READ_START);
				if (team == local_team) continue;

				uint8_t life = *(uint8_t*)(pawn_buf + OFF_LIFE_STATE - PAWN_READ_START);
				if (life != 0) continue;

				int hp = *(int*)(pawn_buf + OFF_HEALTH - PAWN_READ_START);
				if (hp <= 0) continue;

				vec3 feet = *(vec3*)(pawn_buf + OFF_V_OLD_ORIGIN - PAWN_READ_START);
				vec3 head = { feet.x, feet.y, feet.z + 72.0f };
				frame_enemies.push_back({ feet, head, hp });
			}
		}

		{
			std::lock_guard<std::mutex> lock(state.mtx);
			std::swap(state.enemies, frame_enemies);
			state.view_matrix = vm;
		}
	}

	CloseHandle(driver_handle);
}
