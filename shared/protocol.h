#pragma once

// Wire format shared between the kernel driver and the usermode client.
// Include AFTER the platform header for the current TU (ntifs.h in the
// kernel, windows.h in usermode) so HANDLE/PVOID/SIZE_T/ULONG and the
// CTL_CODE / FILE_DEVICE_UNKNOWN / METHOD_BUFFERED / FILE_SPECIAL_ACCESS
// macros are already visible.

namespace driver {
	namespace codes {
		constexpr ULONG attach = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x696, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
		constexpr ULONG read = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x697, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
		constexpr ULONG write = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x698, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
		constexpr ULONG get_module_base = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x699, METHOD_BUFFERED, FILE_SPECIAL_ACCESS);
	}

	constexpr SIZE_T kMaxModuleNameChars = 64;

	struct Request {
		HANDLE process_id;
		PVOID target;
		PVOID buffer;
		SIZE_T size;
		SIZE_T return_size;
		wchar_t module_name[kMaxModuleNameChars];
		PVOID module_base;
	};
}
