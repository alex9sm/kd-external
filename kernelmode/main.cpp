#include <ntifs.h>

#include "../shared/protocol.h"

extern "C" {
	NTKERNELAPI NTSTATUS IoCreateDriver(PUNICODE_STRING DriverName,
										PDRIVER_INITIALIZE InitializationFunction);

	NTKERNELAPI NTSTATUS MmCopyVirtualMemory(PEPROCESS SourceProcess, PVOID SourceAddress,
											PEPROCESS TargetProcess, PVOID TargetAddress,
											SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode,
											PSIZE_T ReturnSize);

	NTKERNELAPI PVOID NTAPI PsGetProcessPeb(PEPROCESS Process);
}

struct ldr_data_table_entry_min {
	LIST_ENTRY InLoadOrderLinks;
	LIST_ENTRY InMemoryOrderLinks;
	LIST_ENTRY InInitializationOrderLinks;
	PVOID DllBase;
	PVOID EntryPoint;
	ULONG SizeOfImage;
	UNICODE_STRING FullDllName;
	UNICODE_STRING BaseDllName;
};

struct peb_ldr_data_min {
	ULONG Length;
	UCHAR Initialized;
	PVOID SsHandle;
	LIST_ENTRY InLoadOrderModuleList;
};

struct peb_min {
	UCHAR InheritedAddressSpace;
	UCHAR ReadImageFileExecOptions;
	UCHAR BeingDebugged;
	UCHAR BitField;
	PVOID Mutant;
	PVOID ImageBaseAddress;
	peb_ldr_data_min* Ldr;
};

void debug_print(PCSTR str) {
#if DBG
	KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, str));
#else
	UNREFERENCED_PARAMETER(str);
#endif
}

namespace driver {
	static PEPROCESS target_process = nullptr;

	NTSTATUS create(PDEVICE_OBJECT device_obj, PIRP irp) {
		UNREFERENCED_PARAMETER(device_obj);
		irp->IoStatus.Status = STATUS_SUCCESS;
		irp->IoStatus.Information = 0;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return STATUS_SUCCESS;
	}

	NTSTATUS close(PDEVICE_OBJECT device_obj, PIRP irp) {
		UNREFERENCED_PARAMETER(device_obj);
		irp->IoStatus.Status = STATUS_SUCCESS;
		irp->IoStatus.Information = 0;
		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return STATUS_SUCCESS;
	}

	NTSTATUS device_control(PDEVICE_OBJECT device_obj, PIRP irp) {
		UNREFERENCED_PARAMETER(device_obj);

		debug_print("device control called.\n");

		NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
		PIO_STACK_LOCATION stack_irp = IoGetCurrentIrpStackLocation(irp);

		if (stack_irp == nullptr) {
			irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
			irp->IoStatus.Information = 0;
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			return STATUS_INVALID_PARAMETER;
		}

		const ULONG in_len = stack_irp->Parameters.DeviceIoControl.InputBufferLength;
		const ULONG out_len = stack_irp->Parameters.DeviceIoControl.OutputBufferLength;
		auto request = reinterpret_cast<Request*>(irp->AssociatedIrp.SystemBuffer);

		if (request == nullptr || in_len < sizeof(Request) || out_len < sizeof(Request)) {
			irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
			irp->IoStatus.Information = 0;
			IoCompleteRequest(irp, IO_NO_INCREMENT);
			return STATUS_INVALID_PARAMETER;
		}

		const ULONG control_code = stack_irp->Parameters.DeviceIoControl.IoControlCode;

		switch (control_code) {
			case codes::attach: {
				PEPROCESS new_process = nullptr;
				status = PsLookupProcessByProcessId(request->process_id, &new_process);
				if (NT_SUCCESS(status)) {
					if (target_process != nullptr) {
						ObDereferenceObject(target_process);
					}
					target_process = new_process;
				}
				break;
			}

			case codes::read:
				if (target_process == nullptr) {
					status = STATUS_DEVICE_NOT_READY;
				} else {
					status = MmCopyVirtualMemory(target_process, request->target,
												 PsGetCurrentProcess(), request->buffer, request->size,
												 KernelMode, &request->return_size);
				}
				break;

			case codes::write:
				if (target_process == nullptr) {
					status = STATUS_DEVICE_NOT_READY;
				} else {
					status = MmCopyVirtualMemory(PsGetCurrentProcess(), request->buffer,
												 target_process, request->target, request->size,
												 KernelMode, &request->return_size);
				}
				break;

			case codes::get_module_base: {
				if (target_process == nullptr) {
					status = STATUS_DEVICE_NOT_READY;
					break;
				}

				// Force null termination — usermode could send a name that
				// filled the whole buffer with no terminator.
				request->module_name[kMaxModuleNameChars - 1] = L'\0';

				UNICODE_STRING wanted = {};
				RtlInitUnicodeString(&wanted, request->module_name);

				PVOID found_base = nullptr;
				NTSTATUS walk_status = STATUS_NOT_FOUND;

				KAPC_STATE apc_state = {};
				KeStackAttachProcess(target_process, &apc_state);

				__try {
					auto peb = reinterpret_cast<peb_min*>(PsGetProcessPeb(target_process));
					if (peb == nullptr || peb->Ldr == nullptr) {
						walk_status = STATUS_NOT_FOUND;
					} else {
						PLIST_ENTRY head = &peb->Ldr->InLoadOrderModuleList;
						for (PLIST_ENTRY cur = head->Flink; cur != head; cur = cur->Flink) {
							auto entry = CONTAINING_RECORD(cur, ldr_data_table_entry_min, InLoadOrderLinks);
							if (entry->BaseDllName.Buffer != nullptr &&
								RtlEqualUnicodeString(&entry->BaseDllName, &wanted, TRUE)) {
								found_base = entry->DllBase;
								walk_status = STATUS_SUCCESS;
								break;
							}
						}
					}
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					walk_status = STATUS_UNSUCCESSFUL;
				}

				KeUnstackDetachProcess(&apc_state);

				request->module_base = found_base;
				status = walk_status;
				break;
			}

			default:
				status = STATUS_INVALID_DEVICE_REQUEST;
				break;
		}

		irp->IoStatus.Status = status;
		irp->IoStatus.Information = NT_SUCCESS(status) ? sizeof(Request) : 0;

		IoCompleteRequest(irp, IO_NO_INCREMENT);
		return status;
	}

	void driver_unload(PDRIVER_OBJECT driver_obj) {
		if (target_process != nullptr) {
			ObDereferenceObject(target_process);
			target_process = nullptr;
		}

		UNICODE_STRING link = {};
		RtlInitUnicodeString(&link, L"\\DosDevices\\io-driver");
		IoDeleteSymbolicLink(&link);

		if (driver_obj->DeviceObject != nullptr) {
			IoDeleteDevice(driver_obj->DeviceObject);
		}

		debug_print("driver unloaded.\n");
	}
}

NTSTATUS driver_main(PDRIVER_OBJECT driverobj, PUNICODE_STRING path) {
	UNREFERENCED_PARAMETER(path);
	UNICODE_STRING device_name = {};
	RtlInitUnicodeString(&device_name, L"\\Device\\io-driver");
	PDEVICE_OBJECT device_obj = nullptr;
	NTSTATUS status = IoCreateDevice(driverobj, 0, &device_name, FILE_DEVICE_UNKNOWN,
									FILE_DEVICE_SECURE_OPEN, FALSE, &device_obj);
	if (status != STATUS_SUCCESS) {
		debug_print("failed to create driver device.\n");
		return status;
	}

	debug_print("driver device successfully created.\n");

	UNICODE_STRING link = {};
	RtlInitUnicodeString(&link, L"\\DosDevices\\io-driver");

	status = IoCreateSymbolicLink(&link, &device_name);
	if (status != STATUS_SUCCESS) {
		debug_print("failed to create symbolic link.\n");
		IoDeleteDevice(device_obj);
		return status;
	}

	debug_print("symbolic link successfully created.\n");

	SetFlag(device_obj->Flags, DO_BUFFERED_IO);
	driverobj->MajorFunction[IRP_MJ_CREATE] = driver::create;
	driverobj->MajorFunction[IRP_MJ_CLOSE] = driver::close;
	driverobj->MajorFunction[IRP_MJ_DEVICE_CONTROL] = driver::device_control;
	driverobj->DriverUnload = driver::driver_unload;
	ClearFlag(device_obj->Flags, DO_DEVICE_INITIALIZING);

	debug_print("driver successfully initialized.\n");

	return status;
}

NTSTATUS DriverEntry() {
	debug_print("Hello world from the kernel.\n");

	UNICODE_STRING driver_name = {};
	RtlInitUnicodeString(&driver_name, L"\\Driver\\io-driver");

	return IoCreateDriver(&driver_name, &driver_main);
}