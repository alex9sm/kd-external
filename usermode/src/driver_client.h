#pragma once

#include <cstdint>
#include <cwchar>
#include <windows.h>

#include "../../shared/protocol.h"

namespace driver {
	inline bool attach_to_process(HANDLE driver_handle, const DWORD pid) {
		Request r;
		r.process_id = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(pid));

		return DeviceIoControl(driver_handle, codes::attach, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);
	}

	template <class T>
	T read_memory(HANDLE driver_handle, const std::uintptr_t addr, bool* ok = nullptr) {
		T temp = {};
		Request r;
		r.target = reinterpret_cast<PVOID>(addr);
		r.buffer = &temp;
		r.size = sizeof(T);

		const BOOL success = DeviceIoControl(driver_handle, codes::read, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);
		if (ok) *ok = (success == TRUE);
		return temp;
	}

	inline bool read_raw(HANDLE driver_handle, const std::uintptr_t addr, void* out, const std::size_t size) {
		Request r = {};
		r.target = reinterpret_cast<PVOID>(addr);
		r.buffer = out;
		r.size = size;
		return DeviceIoControl(driver_handle, codes::read, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr) == TRUE;
	}

	template <class T>
	void write_memory(HANDLE driver_handle, const std::uintptr_t addr, const T& val) {
		Request r;
		r.target = reinterpret_cast<PVOID>(addr);
		r.buffer = (PVOID)&val;
		r.size = sizeof(T);

		DeviceIoControl(driver_handle, codes::write, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr);
	}

	inline std::uintptr_t get_module_base(HANDLE driver_handle, const wchar_t* module_name) {
		Request r = {};
		wcsncpy_s(r.module_name, kMaxModuleNameChars, module_name, _TRUNCATE);

		if (!DeviceIoControl(driver_handle, codes::get_module_base, &r, sizeof(r), &r, sizeof(r), nullptr, nullptr)) {
			return 0;
		}
		return reinterpret_cast<std::uintptr_t>(r.module_base);
	}

	struct ReadEntry {
		std::uintptr_t addr;
		std::size_t size;
	};

	inline bool read_batch(HANDLE driver_handle, const ReadEntry* entries, std::size_t count,
	                        void* out, std::size_t out_size) {
		if (count == 0 || count > kMaxBatchEntries) return false;

		BatchRequest req = {};
		req.count = static_cast<ULONG>(count);
		for (std::size_t i = 0; i < count; i++) {
			req.entries[i].target = reinterpret_cast<PVOID>(entries[i].addr);
			req.entries[i].size = entries[i].size;
		}

		return DeviceIoControl(driver_handle, codes::read_batch,
		                       &req, sizeof(req),
		                       out, static_cast<DWORD>(out_size),
		                       nullptr, nullptr) == TRUE;
	}
}
