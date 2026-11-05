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
	std::vector<uint8_t> bulk_buf;

	while (state.running) {
		// Phase 1: globals (3 reads → 1 IOCTL)
		struct { uint64_t entity_list; uint64_t local_pawn; view_matrix_t vm; } globals = {};
		{
			driver::ReadEntry entries[] = {
				{ base + OFF_ENTITY_LIST, sizeof(uint64_t) },
				{ base + OFF_LOCAL_PLAYER, sizeof(uint64_t) },
				{ base + OFF_VIEW_MATRIX, sizeof(view_matrix_t) },
			};
			driver::read_batch(driver_handle, entries, 3, &globals, sizeof(globals));
		}

		frame_enemies.clear();

		if (globals.entity_list && globals.local_pawn) {
			// Phase 2: chunk pointers + local team (5 reads → 1 IOCTL)
			struct { uint64_t chunks[4]; int local_team; } phase2 = {};
			{
				driver::ReadEntry entries[] = {
					{ globals.entity_list + 8 * 0 + 16, sizeof(uint64_t) },
					{ globals.entity_list + 8 * 1 + 16, sizeof(uint64_t) },
					{ globals.entity_list + 8 * 2 + 16, sizeof(uint64_t) },
					{ globals.entity_list + 8 * 3 + 16, sizeof(uint64_t) },
					{ globals.local_pawn + OFF_TEAM, sizeof(int) },
				};
				driver::read_batch(driver_handle, entries, 5, &phase2, sizeof(phase2));
			}

			// Phase 3: all 64 controller pointers (64 reads → 1 IOCTL)
			uint64_t controllers[64] = {};
			if (phase2.chunks[0]) {
				driver::ReadEntry entries[64];
				for (int i = 0; i < 64; i++)
					entries[i] = { phase2.chunks[0] + 0x70 * (uint64_t)(i + 1), sizeof(uint64_t) };
				driver::read_batch(driver_handle, entries, 64, controllers, sizeof(controllers));
			}

			// Phase 4: pawn handles for valid controllers (≤64 reads → 1 IOCTL)
			uint32_t pawn_handles[64] = {};
			int valid_ctrl[64];
			int valid_ctrl_n = 0;

			for (int i = 0; i < 64; i++) {
				if (controllers[i])
					valid_ctrl[valid_ctrl_n++] = i;
			}

			if (valid_ctrl_n > 0) {
				driver::ReadEntry entries[64];
				for (int i = 0; i < valid_ctrl_n; i++)
					entries[i] = { controllers[valid_ctrl[i]] + OFF_PLAYER_PAWN, sizeof(uint32_t) };

				uint32_t results[64] = {};
				driver::read_batch(driver_handle, entries, valid_ctrl_n, results, valid_ctrl_n * sizeof(uint32_t));

				for (int i = 0; i < valid_ctrl_n; i++)
					pawn_handles[valid_ctrl[i]] = results[i];
			}

			// Phase 5: resolve pawn entity pointers (≤64 reads → 1 IOCTL)
			uint64_t pawns[64] = {};
			int pawn_slots[64];
			int pawn_slot_n = 0;

			for (int i = 0; i < 64; i++) {
				if (!pawn_handles[i]) continue;
				uint32_t idx = pawn_handles[i] & 0x7FFF;
				uint32_t ci = idx >> 9;
				if (ci >= 4 || !phase2.chunks[ci]) continue;
				pawn_slots[pawn_slot_n++] = i;
			}

			if (pawn_slot_n > 0) {
				driver::ReadEntry entries[64];
				for (int i = 0; i < pawn_slot_n; i++) {
					uint32_t idx = pawn_handles[pawn_slots[i]] & 0x7FFF;
					uint32_t ci = idx >> 9;
					entries[i] = { phase2.chunks[ci] + 0x70 * (uint64_t)(idx & 0x1FF), sizeof(uint64_t) };
				}

				uint64_t results[64] = {};
				driver::read_batch(driver_handle, entries, pawn_slot_n, results, pawn_slot_n * sizeof(uint64_t));

				for (int i = 0; i < pawn_slot_n; i++)
					pawns[pawn_slots[i]] = results[i];
			}

			// Phase 6: bulk pawn data for enemy pawns (≤64 reads → 1 IOCTL)
			int enemy_slots[64];
			int enemy_n = 0;

			for (int i = 0; i < 64; i++) {
				if (pawns[i] && pawns[i] != globals.local_pawn)
					enemy_slots[enemy_n++] = i;
			}

			if (enemy_n > 0) {
				driver::ReadEntry entries[64];
				for (int i = 0; i < enemy_n; i++)
					entries[i] = { pawns[enemy_slots[i]] + PAWN_READ_START, PAWN_BUF_SIZE };

				bulk_buf.resize(enemy_n * PAWN_BUF_SIZE);
				driver::read_batch(driver_handle, entries, enemy_n, bulk_buf.data(), bulk_buf.size());

				for (int i = 0; i < enemy_n; i++) {
					const uint8_t* pawn_buf = bulk_buf.data() + i * PAWN_BUF_SIZE;

					int team = *reinterpret_cast<const int*>(pawn_buf + OFF_TEAM - PAWN_READ_START);
					if (team == phase2.local_team) continue;

					uint8_t life = *(pawn_buf + OFF_LIFE_STATE - PAWN_READ_START);
					if (life != 0) continue;

					int hp = *reinterpret_cast<const int*>(pawn_buf + OFF_HEALTH - PAWN_READ_START);
					if (hp <= 0) continue;

					vec3 feet = *reinterpret_cast<const vec3*>(pawn_buf + OFF_V_OLD_ORIGIN - PAWN_READ_START);
					vec3 head = { feet.x, feet.y, feet.z + 72.0f };
					frame_enemies.push_back({ feet, head, hp });
				}
			}
		}

		{
			std::lock_guard<std::mutex> lock(state.mtx);
			std::swap(state.enemies, frame_enemies);
			state.view_matrix = globals.vm;
		}

		Sleep(2);
	}

	CloseHandle(driver_handle);
}
