/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * DLL injection.
  */

#include "thcrap.h"

/// Detour chains
/// -------------
typedef BOOL WINAPI CreateProcessW_type(
	LPCWSTR lpApplicationName,
	LPWSTR lpCommandLine,
	LPSECURITY_ATTRIBUTES lpProcessAttributes,
	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	BOOL bInheritHandles,
	DWORD dwCreationFlags,
	LPVOID lpEnvironment,
	LPCWSTR lpCurrentDirectory,
	LPSTARTUPINFOW lpStartupInfo,
	LPPROCESS_INFORMATION lpProcessInformation
);

typedef HANDLE WINAPI CreateRemoteThread_type(
	HANDLE hProcess,
	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	SIZE_T dwStackSize,
	LPTHREAD_START_ROUTINE lpStartAddress,
	LPVOID lpParameter,
	DWORD dwCreationFlags,
	LPDWORD lpThreadId
);

typedef BOOL WINAPI FreeLibrary_type(
	HMODULE hLibModule
);

typedef HMODULE WINAPI LoadLibraryW_type(
	LPCWSTR lpLibFileName
);

W32U8_DETOUR_CHAIN_DEF(CreateProcess);
DETOUR_CHAIN_DEF(CreateProcessW);
DETOUR_CHAIN_DEF(CreateRemoteThread);
DETOUR_CHAIN_DEF(FreeLibrary);
W32U8_DETOUR_CHAIN_DEF(LoadLibrary);
DETOUR_CHAIN_DEF(LoadLibraryW);
/// -------------

/// DLL reference counting
/// ----------------------
// Ensures that thcrap_detour() is only called once for every DLL loaded.
// Keeping our own reference count is certainly more stable in the long run
// than relying on the values from the PEB structure.

json_t *modules_detoured;

#define addr_key_len 2 + (sizeof(void*) * 2) + 1

// Both inject_incref() and inject_decref() return the new reference count
// of the given module.

json_int_t inject_refcount_update(HMODULE hMod, int delta)
{
	char key_str[addr_key_len];
	json_int_t val;

	snprintf(key_str, sizeof(key_str), "%x", hMod);

	val = json_integer_value(json_object_get(modules_detoured, key_str));
	val += delta;
	if(val >= 1) {
		json_object_set_new(modules_detoured, key_str, json_integer(val));
	} else if(val == 0) {
		json_object_del(modules_detoured, key_str);
	}
	return val;
}

json_int_t inject_refcount_inc(HMODULE hMod)
{
	if(!modules_detoured) {
		modules_detoured = json_object();
	}
	return inject_refcount_update(hMod, +1);
}

json_int_t inject_refcount_dec(HMODULE hMod)
{
	if(!modules_detoured) {
		return -1;
	}
	return inject_refcount_update(hMod, -1);
}
/// ----------------------

/**
  * A more complete DLL injection solution.
  * Adapted from http://www.codeproject.com/Articles/20084/completeinject.
  *
  * Parameters
  * ----------
  *	HANDLE hProcess
  *		The handle to the process to inject the DLL into.
  *
  *	const char *dll_dir
  *		Directory of the DLL to inject. (optional)
  *		During injection, the target application's directory can be temporarily
  *		switched to this directory. This may be necessary if the injected DLL
  *		depends on other DLLs stored in the same directory.
  *
  *	const char *dll_fn
  *		File name of the DLL to inject into the process.
  *
  *	const char *func_name
  *		The name of the function to call once the DLL has been injected.
  *
  *	const void *param
  *		Optional parameter to pass to [func_name].
  *		Note that the pointer received in [func_name] points to a copy in the
  *		injection workspace and, thus, is only valid until the function returns.
  *		Don't store it globally in the DLL!
  *
  *	const size_t param_size
  *		Size of [param].
  *
  * Return value
  * ------------
  * Exit code of the injected function:
  *	0 for success
  *	1 if first error was thrown
  *	2 if second error was thrown
  *
  * Description
  * -----------
  * This function will inject a DLL into a process and execute an exported function
  * from the DLL to "initialize" it. The function should be in the format shown below,
  * one parameter and no return type. Do not forget to prefix extern "C" if you are in C++.
  *
  *		__declspec(dllexport) void FunctionName(void*)
  *
  * The function that is called in the injected DLL -MUST- return, the loader
  * waits for the thread to terminate before removing the allocated space and
  * returning control to the loader. This method of DLL injection also adds error
  * handling, so the end user knows if something went wrong.
  */

unsigned char* memcpy_advance_dst(unsigned char *dst, const void *src, size_t num)
{
	memcpy(dst, src, num);
	return dst + num;
}

unsigned char* ptrcpy_advance_dst(unsigned char *dst, const void *src)
{
	*((size_t*)dst) = (size_t)src;
	return dst + sizeof(void*);
}

unsigned char* StringToUTF16_advance_dst(unsigned char *dst, const char *src)
{
	size_t conv_len = StringToUTF16((wchar_t*)dst, src, -1);
	return dst + (conv_len * sizeof(wchar_t));
}

int Inject(HANDLE hProcess, const char *dll_dir, const char *dll_fn, const char *func_name, const void *param, const size_t param_size)
{
	// String constants
	const char *injectError1Format =
		"Could not inject %s.\n"
		"\n"
		"If you're running Windows Vista or 7, make sure that you have installed the KB2533623 update:\n"
		"\n"
		"\thttp://support.microsoft.com/kb/2533623/";
	const char *injectError2Format = "Could not load the function: %s";

//------------------------------------------//
// Function variables.                      //
//------------------------------------------//

	// Main DLL we will need to load
	HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

	// Main functions we will need to import.
	// If [dll_fn] is absolute, LoadLibraryEx() with the LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
	// flag is used to guarantee that the injected DLL and its dependencies really
	// are only loaded from the given directory. Otherwise, LoadLibrary() may load
	// a possible other DLL with the same name from the directory of [hProcess].
	FARPROC getcurrentdirectory = GetProcAddress(kernel32, "GetCurrentDirectoryW");
	FARPROC setcurrentdirectory = GetProcAddress(kernel32, "SetCurrentDirectoryW");
	FARPROC loadlibrary = GetProcAddress(kernel32, "LoadLibraryW");
	FARPROC loadlibraryex = GetProcAddress(kernel32, "LoadLibraryExW");
	FARPROC getprocaddress = GetProcAddress(kernel32, "GetProcAddress");
	FARPROC exitthread = GetProcAddress(kernel32, "ExitThread");
	FARPROC freelibraryandexitthread = GetProcAddress(kernel32, "FreeLibraryAndExitThread");
	int have_kb2533623 = GetProcAddress(kernel32, "SetDefaultDllDirectories") != 0;

	// The workspace we will build the codecave on locally.
	// workspaceSize gets incremented with the final length of the error strings.
	size_t workspaceSize = 2048;
	LPBYTE workspace = NULL;
	LPBYTE p = NULL;

	// The memory in the process we write to
	LPBYTE codecaveAddress = NULL;

	// Strings we have to write into the process
	size_t injectError1_len = _scprintf(injectError1Format, dll_fn) + 1;
	size_t injectError2_len = _scprintf(injectError2Format, func_name) + 1;

	char *injectError0 = "Error";
	VLA(char, injectError1, injectError1_len);
	VLA(char, injectError2, injectError2_len);
	char *user32Name = "user32.dll";
	char *msgboxName = "MessageBoxW";

	// Placeholder addresses to use the strings
	LPBYTE user32NameAddr = 0;
	LPBYTE user32Addr = 0;
	LPBYTE msgboxNameAddr = 0;
	LPBYTE msgboxAddr = 0;
	LPBYTE dllAddr = 0;
	LPBYTE dllDirAddr = 0;
	LPBYTE dllNameAddr = 0;
	LPBYTE funcNameAddr = 0;
	LPBYTE funcParamAddr = 0;
	LPBYTE error0Addr = 0;
	LPBYTE error1Addr = 0;
	LPBYTE error2Addr = 0;

	// Where the codecave execution should begin at
	LPTHREAD_START_ROUTINE codecaveExecAddr = 0;

	// Handle to the thread we create in the process
	HANDLE hThread = NULL;

	// Old protection on page we are writing to in the process and the bytes written
	DWORD oldProtect = 0;
	DWORD byte_ret = 0;

	// Return code of injection function
	DWORD injRet;

//------------------------------------------//
// Variable initialization.                 //
//------------------------------------------//

// This section will cause compiler warnings on VS8,
// you can upgrade the functions or ignore them

	// Build error messages
	sprintf(injectError1, injectError1Format, dll_fn);
	sprintf(injectError2, injectError2Format, func_name);

	workspaceSize += (
		strlen(dll_dir) + 1 + strlen(dll_fn) + 1 + strlen(func_name) + 1 +
		param_size + strlen(injectError1) + 1 + strlen(injectError2) + 1
	) * sizeof(wchar_t);

	// Create the workspace
	workspace = (LPBYTE)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, workspaceSize);
	p = workspace;

	// Allocate space for the codecave in the process
	codecaveAddress = (LPBYTE)VirtualAllocEx(hProcess, 0, workspaceSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

// Note there is no error checking done above for any functions that return a pointer/handle.
// I could have added them, but it'd just add more messiness to the code and not provide any real
// benefit. It's up to you though in your final code if you want it there or not.

//------------------------------------------//
// Data and string writing.                 //
//------------------------------------------//

	// Reserve space for the user32 dll address, the MessageBox address,
	// and the address of the injected DLL's module.
	user32Addr = (p - workspace) + codecaveAddress;
	p += sizeof(LPBYTE);

	msgboxAddr = (p - workspace) + codecaveAddress;
	p += sizeof(LPBYTE);

	dllAddr = (p - workspace) + codecaveAddress;
	p += sizeof(LPBYTE);

	// User32 Dll Name
	user32NameAddr = (p - workspace) + codecaveAddress;
	p = StringToUTF16_advance_dst(p, user32Name);

	// MessageBox name
	msgboxNameAddr = (p - workspace) + codecaveAddress;
	p = memcpy_advance_dst(p, msgboxName, strlen(msgboxName) + 1);

	// Directory name
	if(dll_dir) {
		dllDirAddr = (p - workspace) + codecaveAddress;
		p = StringToUTF16_advance_dst(p, dll_dir);
	}

	// Dll Name
	dllNameAddr = (p - workspace) + codecaveAddress;
	p = StringToUTF16_advance_dst(p, dll_fn);

	// Function Name
	funcNameAddr = (p - workspace) + codecaveAddress;
	p = memcpy_advance_dst(p, func_name, strlen(func_name) + 1);

	// Function Parameter
	funcParamAddr = (p - workspace) + codecaveAddress;
	p = memcpy_advance_dst(p, param, param_size);

	// Error Message 1
	error0Addr = (p - workspace) + codecaveAddress;
	p = StringToUTF16_advance_dst(p, injectError0);

	// Error Message 2
	error1Addr = (p - workspace) + codecaveAddress;
	p = StringToUTF16_advance_dst(p, injectError1);

	// Error Message 3
	error2Addr = (p - workspace) + codecaveAddress;
	p = StringToUTF16_advance_dst(p, injectError2);

	// Pad a few INT3s after string data is written for seperation
	*p++ = 0xCC;
	*p++ = 0xCC;
	*p++ = 0xCC;

	// Store where the codecave execution should begin
	codecaveExecAddr = (LPTHREAD_START_ROUTINE)((p - workspace) + codecaveAddress);

// For debugging - infinite loop, attach onto process and step over
	//*p++ = 0xEB;
	//*p++ = 0xFE;

//------------------------------------------//
// User32.dll loading.                      //
//------------------------------------------//

// User32 DLL Loading
	// PUSH 0x00000000 - Push the address of the DLL name to use in LoadLibrary
	*p++ = 0x68;
	p = ptrcpy_advance_dst(p, user32NameAddr);

	// MOV EAX, ADDRESS - Move the address of LoadLibrary into EAX
	*p++ = 0xB8;
	p = ptrcpy_advance_dst(p, loadlibrary);

	// CALL EAX - Call LoadLibrary
	*p++ = 0xFF;
	*p++ = 0xD0;

// MessageBox Loading
	// PUSH 0x000000 - Push the address of the function name to load
	*p++ = 0x68;
	p = ptrcpy_advance_dst(p, msgboxNameAddr);

	// Push EAX, module to use in GetProcAddress
	*p++ = 0x50;

	// MOV EAX, ADDRESS - Move the address of GetProcAddress into EAX
	*p++ = 0xB8;
	p = ptrcpy_advance_dst(p, getprocaddress);

	// CALL EAX - Call GetProcAddress
	*p++ = 0xFF;
	*p++ = 0xD0;

	// MOV [ADDRESS], EAX - Save the address to our variable
	*p++ = 0xA3;
	p = ptrcpy_advance_dst(p, msgboxAddr);

//------------------------------------------//
// Injected dll loading.                    //
//------------------------------------------//

/*
	// This is the way the following assembly code would look like in C/C++

	// In case the injected DLL depends on other DLLs,
	// we need to change the current directory to the one given as parameter
	if(dll_dir) {
		size_t cur_dir_len = GetCurrentDirectory(0, NULL) + 1;
		VLA(wchar_t, cur_dir, cur_dir_len);
		GetCurrentDirectory(cur_dir, cur_dir_len);
		SetCurrentDirectory(dll_dir);
	}

	// Load the injected DLL into this process
	HMODULE h = LoadLibraryEx(dll_fn, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
	if(!h) {
		MessageBox(0, injectError1, "Error", MB_ICONERROR);
		ExitThread(1);
	}

	if(dll_dir) {
		SetCurrentDirectory(cur_dir);
	}

	// Get the address of the export function
	FARPROC p = GetProcAddress(h, func_name);
	if(!p) {
		MessageBox(0, injectError2, "Error", MB_ICONERROR);
		FreeLibraryAndExitThread(h, 2);
	}

	// So we do not need a function pointer interface
	__asm call p

	// Exit the thread so the loader continues
	ExitThread(0);
*/

// DLL Loading

	if(dllDirAddr) {
		// Registers:

		// ebp: Base stack frame
		// esi: GetCurrentDirectory / SetCurrentDirectory
		// ebx: Current directory of process (on stack)
		// ecx: byte length of string at ebx

		// mov ebp, esp - Save stack frame
		*p++ = 0x89;
		*p++ = 0xe5;

		// Get length for current directory

		// push 0
		// push 0
		*p++ = 0x6a;
		*p++ = 0x00;
		*p++ = 0x6a;
		*p++ = 0x00;
		// mov esi, GetCurrentDirectory
		*p++ = 0xbe;
		p = ptrcpy_advance_dst(p, getcurrentdirectory);

		// call esi
		*p++ = 0xFF;
		*p++ = 0xD6;

		/// Calculate byte size of directory buffer.
		/// Also do some poor man's DWORD boundary alignment
		/// in order to not fuck up the stack

		// mov ecx, eax
		// shl ecx, 1
		// and ecx, fffffff8
		// add ecx, 4

		*p++ = 0x89;
		*p++ = 0xc1;
		*p++ = 0xd1;
		*p++ = 0xe1;
		*p++ = 0x83;
		*p++ = 0xe1;
		*p++ = 0xf8;
		*p++ = 0x83;
		*p++ = 0xc1;
		*p++ = 0x04;

		/// "Allocate" ecx bytes on stack and store buffer pointer to ebx

		// sub esp, ecx
		// mov ebx, esp

		*p++ = 0x29;
		*p++ = 0xcc;
		*p++ = 0x89;
		*p++ = 0xe3;

		/// Call GetCurrentDirectory
		// push ebx
		// push eax
		// call esi
		*p++ = 0x53;
		*p++ = 0x50;
		*p++ = 0xff;
		*p++ = 0xd6;

		/// PUSH 0x00000000 - Push the address of our directory
		*p++ = 0x68;
		p = ptrcpy_advance_dst(p, dllDirAddr);

		// mov esi, SetCurrentDirectory
		*p++ = 0xbe;
		p = ptrcpy_advance_dst(p, setcurrentdirectory);

		// call esi
		*p++ = 0xFF;
		*p++ = 0xD6;
	}

	if(PathIsRelativeA(dll_fn) || !have_kb2533623) {
		// PUSH 0x00 (dwFlags = 0)
		*p++ = 0x6a;
		*p++ = 0x00;
	} else {
		// PUSH 0x00000900 (dwFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)
		*p++ = 0x68;
		*p++ = 0x00;
		*p++ = 0x09;
		*p++ = 0x00;
		*p++ = 0x00;
	}

	// PUSH 0x00 (hFile = NULL)
	*p++ = 0x6a;
	*p++ = 0x00;

	// PUSH 0x00000000 - Push the address of the DLL name to use in LoadLibraryEx
	*p++ = 0x68;
	p = ptrcpy_advance_dst(p, dllNameAddr);

	// MOV EAX, ADDRESS - Move the address of LoadLibraryEx into EAX
	*p++ = 0xB8;
	p = ptrcpy_advance_dst(p, loadlibraryex);

	// CALL EAX - Call LoadLibraryEx
	*p++ = 0xFF;
	*p++ = 0xD0;

	// mov edi, eax - Save return value
	*p++ = 0x89;
	*p++ = 0xc7;

	if(dllDirAddr) {
		/// Reset directory to the original one of the process
		// push ebx
		// call esi
		*p++ = 0x53;
		*p++ = 0xFF;
		*p++ = 0xD6;

		/// Reset stack frame
		// mov esp, ebp
		*p++ = 0x89;
		*p++ = 0xec;
	}

// Error Checking
	// CMP EDI, 0
	*p++ = 0x83;
	*p++ = 0xFF;
	*p++ = 0x00;

// JNZ EIP + 0x1E to skip over eror code
	*p++ = 0x75;
	*p++ = 0x1E;

// Error Code 1
	// MessageBox
		// PUSH 0x10 (MB_ICONHAND)
		*p++ = 0x6A;
		*p++ = 0x10;

		// PUSH 0x000000 - Push the address of the MessageBox title
		*p++ = 0x68;
		p = ptrcpy_advance_dst(p, error0Addr);

		// PUSH 0x000000 - Push the address of the MessageBox message
		*p++ = 0x68;
		p = ptrcpy_advance_dst(p, error1Addr);

		// Push 0
		*p++ = 0x6A;
		*p++ = 0x00;

		// MOV EAX, [ADDRESS] - Move the address of MessageBox into EAX
		*p++ = 0xA1;
		p = ptrcpy_advance_dst(p, msgboxAddr);

		// CALL EAX - Call MessageBoxW
		*p++ = 0xFF;
		*p++ = 0xD0;

	// ExitThread
		// PUSH 1
		*p++ = 0x6A;
		*p++ = 0x01;

		// MOV EAX, ADDRESS - Move the address of ExitThread into EAX
		*p++ = 0xB8;
		p = ptrcpy_advance_dst(p, exitthread);

		// CALL EAX - Call ExitThread
		*p++ = 0xFF;
		*p++ = 0xD0;

//	Now we have the address of the injected DLL, so save the handle

	// MOV [ADDRESS], EAX - Save the address to our variable
	*p++ = 0x89;
	*p++ = 0x3D;
	p = ptrcpy_advance_dst(p, dllAddr);

// Load the initilize function from it

	// PUSH 0x000000 - Push the address of the function name to load
	*p++ = 0x68;
	p = ptrcpy_advance_dst(p, funcNameAddr);

	// Push EDI - module to use in GetProcAddress
	*p++ = 0x57;

	// MOV EAX, ADDRESS - Move the address of GetProcAddress into EAX
	*p++ = 0xB8;
	p = ptrcpy_advance_dst(p, getprocaddress);

	// CALL EAX - Call GetProcAddress
	*p++ = 0xFF;
	*p++ = 0xD0;

// Error Checking
	// CMP EAX, 0
	*p++ = 0x83;
	*p++ = 0xF8;
	*p++ = 0x00;

// JNZ EIP + 0x23 to skip eror code
	*p++ = 0x75;
	*p++ = 0x23;

// Error Code 2
	// MessageBox
		// PUSH 0x10 (MB_ICONHAND)
		*p++ = 0x6A;
		*p++ = 0x10;

		// PUSH 0x000000 - Push the address of the MessageBox title
		*p++ = 0x68;
		p = ptrcpy_advance_dst(p, error0Addr);

		// PUSH 0x000000 - Push the address of the MessageBox message
		*p++ = 0x68;
		p = ptrcpy_advance_dst(p, error2Addr);

		// Push 0
		*p++ = 0x6A;
		*p++ = 0x00;

		// MOV EAX, ADDRESS - Move the address of MessageBox into EAX
		*p++ = 0xA1;
		p = ptrcpy_advance_dst(p, msgboxAddr);

		// CALL EAX - Call MessageBoxA
		*p++ = 0xFF;
		*p++ = 0xD0;

	// FreeLibraryAndExitThread
		// PUSH 2
		*p++ = 0x6A;
		*p++ = 0x02;

		// PUSH 0x000000 - Push the injected DLL's module handle
		*p++ = 0x68;
		p = ptrcpy_advance_dst(p, dllAddr);

		// MOV EAX, ADDRESS - Move the address of FreeLibraryAndExitThread into EAX
		*p++ = 0xB8;
		p = ptrcpy_advance_dst(p, freelibraryandexitthread);

		// CALL EAX - Call ExitThread function
		*p++ = 0xFF;
		*p++ = 0xD0;

	// PUSH 0x000000 - Push the address of the function parameter
	*p++ = 0x68;
	p = ptrcpy_advance_dst(p, funcParamAddr);

	// CALL EAX - Call [func_name]
	*p++ = 0xFF;
	*p++ = 0xD0;

	// If we get here, [func_name] has been called,
	// so it's time to close this thread and optionally unload the DLL.

//------------------------------------------//
// Exiting from the injected dll.           //
//------------------------------------------//

// Call ExitThread to leave the DLL loaded
#if 1
	// PUSH 0 (exit code)
	*p++ = 0x6A;
	*p++ = 0x00;

	// MOV EAX, ADDRESS - Move the address of ExitThread into EAX
	*p++ = 0xB8;
	p = ptrcpy_advance_dst(p, exitthread);

	// CALL EAX - Call ExitThread
	*p++ = 0xFF;
	*p++ = 0xD0;
#endif

// Call FreeLibraryAndExitThread to unload DLL
#if 0
	// Push 0 (exit code)
	*p++ = 0x6A;
	*p++ = 0x00;

	// PUSH [0x000000] - Push the address of the DLL module to unload
	*p++ = 0xFF;
	*p++ = 0x35;
	p = ptrcpy_advance_dst(p, dllAddr);

	// MOV EAX, ADDRESS - Move the address of FreeLibraryAndExitThread into EAX
	*p++ = 0xB8;
	p = ptrcpy_advance_dst(p, freelibraryandexitthread);

	// CALL EAX - Call FreeLibraryAndExitThread
	*p++ = 0xFF;
	*p++ = 0xD0;
#endif

//------------------------------------------//
// Code injection and cleanup.              //
//------------------------------------------//

	// Change page protection so we can write executable code
	VirtualProtectEx(hProcess, codecaveAddress, p - workspace, PAGE_EXECUTE_READWRITE, &oldProtect);

	// Write out the patch
	WriteProcessMemory(hProcess, codecaveAddress, workspace, p - workspace, &byte_ret);

	// Restore page protection
	VirtualProtectEx(hProcess, codecaveAddress, p - workspace, oldProtect, &oldProtect);

	// Make sure our changes are written right away
	FlushInstructionCache(hProcess, codecaveAddress, p - workspace);

	// Free the workspace memory
	HeapFree(GetProcessHeap(), 0, workspace);

	// Execute the thread now and wait for it to exit, note we execute where the code starts, and not the codecave start
	// (since we wrote strings at the start of the codecave)
	hThread = CreateRemoteThread(hProcess, NULL, 0, codecaveExecAddr, 0, 0, NULL);
	WaitForSingleObject(hThread, INFINITE);

	GetExitCodeThread(hThread, &injRet);
	CloseHandle(hThread);

	// Free the memory in the process that we allocated
	VirtualFreeEx(hProcess, codecaveAddress, 0, MEM_RELEASE);

	VLA_FREE(injectError1);
	VLA_FREE(injectError2);
	return injRet;
}

int thcrap_inject_into_running(HANDLE hProcess, const char *run_cfg_fn)
{
	int ret = -1;
	HMODULE inj_mod = NULL;

	if(GetModuleHandleEx(
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		(LPTSTR)thcrap_inject_into_running,
		&inj_mod
	)) {
		size_t cur_dir_len = GetCurrentDirectory(0, NULL) + 1;
		size_t inj_dir_len = GetModuleFileNameU(inj_mod, NULL, 0) + 1;
		VLA(char, inj_dll, inj_dir_len);
		VLA(char, inj_dir, inj_dir_len);

		STRLEN_DEC(run_cfg_fn);
		size_t param_len = cur_dir_len + run_cfg_fn_len;
		VLA(char, abs_run_cfg_fn, param_len);
		const char *param;

		GetModuleFileNameU(inj_mod, inj_dir, inj_dir_len);
		strncpy(inj_dll, inj_dir, inj_dir_len);
		PathRemoveFileSpec(inj_dir);
		PathAddBackslashA(inj_dir);

		// Allow for relative directory names
		if(PathIsRelativeA(run_cfg_fn)) {
			GetCurrentDirectory(cur_dir_len, abs_run_cfg_fn);
			PathAppendA(abs_run_cfg_fn, run_cfg_fn);
			param = abs_run_cfg_fn;
		} else {
			param = run_cfg_fn;
			param_len = run_cfg_fn_len;
		}
		ret = Inject(hProcess, inj_dir, inj_dll, "thcrap_init", param, param_len);
		VLA_FREE(abs_run_cfg_fn);
		VLA_FREE(inj_dir);
		VLA_FREE(inj_dll);
	}
	return ret;
}

BOOL thcrap_inject_into_new(const char *exe_fn, char *args, const char *run_cfg_fn)
{
	int ret = 0;
	json_t *run_cfg = json_load_file_report(run_cfg_fn);
	if(!run_cfg) {
		return 1;
	};
	json_object_set_new(run_cfg, "run_cfg_fn", json_string(run_cfg_fn));
	runconfig_set(run_cfg);
	{
		STRLEN_DEC(exe_fn);
		VLA(char, exe_dir_local, exe_fn_len);
		VLA(char, exe_fn_local, exe_fn_len);
		STARTUPINFOA si = {0};
		PROCESS_INFORMATION pi = {0};
		char *exe_dir = NULL;

		strcpy(exe_fn_local, exe_fn);
		str_slash_normalize_win(exe_fn_local);

		strcpy(exe_dir_local, exe_fn);
		if(PathRemoveFileSpec(exe_dir_local)) {
			exe_dir = exe_dir_local;
		}

		/**
		  * Sure, the alternative would be to set up the entire engine
		  * with all plug-ins and modules to correctly run any additional
		  * detours. While it would indeed be nice to allow those to control
		  * initial startup, it really shouldn't be necessary for now - and
		  * it really does run way too much unnecessary code for my taste.
		  */
		ret = W32_ERR_WRAP(inject_CreateProcessU(
			exe_fn_local, args, NULL, NULL, TRUE, 0, NULL, exe_dir, &si, &pi
		));
		if(ret) {
			char *msg_str = "";

			FormatMessage(
				FORMAT_MESSAGE_FROM_SYSTEM |
				FORMAT_MESSAGE_ALLOCATE_BUFFER |
				FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, ret, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				(LPSTR)&msg_str, 0, NULL
			);

			log_mboxf(NULL, MB_OK | MB_ICONEXCLAMATION,
				"Failed to start %s: %s",
				exe_fn, msg_str
			);
			LocalFree(msg_str);
		}
		VLA_FREE(exe_fn_local);
		VLA_FREE(exe_dir_local);
	}
	return ret;
}

/// Entry point determination
/// -------------------------
void* entry_from_context(HANDLE hThread)
{
	CONTEXT context = {0};
	context.ContextFlags = CONTEXT_FULL;
	if(GetThreadContext(hThread, &context)) {
		return (void*)context.Eax;
	}
	return NULL;
}
/// -------------------------

int ThreadWaitUntil(HANDLE hProcess, HANDLE hThread, void *addr)
{
	CONTEXT context = {0};
	BYTE entry_asm_orig[2];
	const BYTE entry_asm_delay[2] = {0xEB, 0xFE}; // JMP SHORT YADA YADA
	MEMORY_BASIC_INFORMATION mbi;
	DWORD byte_ret;
	DWORD old_prot;

	if(!VirtualQueryEx(hProcess, addr, &mbi, sizeof(mbi))) {
		return 1;
	}
	VirtualProtectEx(hProcess, mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &old_prot);
	ReadProcessMemory(hProcess, addr, entry_asm_orig, sizeof(entry_asm_orig), &byte_ret);
	WriteProcessMemory(hProcess, addr, entry_asm_delay, sizeof(entry_asm_delay), &byte_ret);
	FlushInstructionCache(hProcess, addr, sizeof(entry_asm_delay));

	context.ContextFlags = CONTEXT_CONTROL;
	while(context.Eip != (DWORD)addr) {
		ResumeThread(hThread);
		Sleep(10);
		SuspendThread(hThread);
		GetThreadContext(hThread, &context);
	}

	// Write back the original code
	WriteProcessMemory(hProcess, addr, entry_asm_orig, sizeof(entry_asm_orig), &byte_ret);
	FlushInstructionCache(hProcess, addr, sizeof(entry_asm_orig));

	VirtualProtectEx(hProcess, mbi.BaseAddress, mbi.RegionSize, old_prot, &old_prot);
	return 0;
}

int WaitUntilEntryPoint(HANDLE hProcess, HANDLE hThread, const char *module)
{
	// Try to get the entry point by various means, sorted by both efficiency
	// and probability of them working.
	void *entry_addr = NULL;

	/**
	  * Method 1: Initial value of EAX
	  * After creating a process in suspended state, EAX is guaranteed to contain
	  * the correct address of the entry point, even when the executable has the
	  * DYNAMICBASE flag activated in its header.
	  *
	  * (Works on Windows, but not on Wine)
	  */
	if(!(entry_addr = entry_from_context(hThread))) {
		HMODULE module_base;

		/**
		  * Method 2: EnumProcessModules, then parse the PE header.
		  *
		  * (Works on Wine, but not on Windows immediately after the target process
		  * was created in suspended state.)
		  */
		if(!(module_base = GetRemoteModuleHandle(hProcess, module))) {
			/**
			  * Method 3: Guessing 0x400000
			  * This is the default value in many compilers and should thus work for
			  * most non-ASLR Windows applications.
			  */
			module_base = (HMODULE)0x400000;
		}
		entry_addr = GetRemoteModuleEntryPoint(hProcess, module_base);
	}

	if(entry_addr) {
		return ThreadWaitUntil(hProcess, hThread, entry_addr);
	} else {
		log_mboxf(NULL, MB_OK | MB_ICONEXCLAMATION,
			"Couldn't determine the entry point of %s!\n"
			"\n"
			"Seems as if %s won't work with this game on your system.\n",
			PathFindFileNameA(module), PROJECT_NAME_SHORT()
		);
		return 1;
	}
}

// Injection calls shared between the U and W versions
static void inject_CreateProcess_helper(
	LPPROCESS_INFORMATION lpPI,
	LPCSTR lpAppName,
	DWORD dwCreationFlags
)
{
	if(!WaitUntilEntryPoint(lpPI->hProcess, lpPI->hThread, lpAppName)) {
		const char *run_cfg_fn = json_object_get_string(run_cfg, "run_cfg_fn");
		thcrap_inject_into_running(lpPI->hProcess, run_cfg_fn);
	}
	if(~dwCreationFlags & CREATE_SUSPENDED) {
		ResumeThread(lpPI->hThread);
	}
}

BOOL WINAPI inject_CreateProcessU(
	LPCSTR lpAppName,
	LPSTR lpCmdLine,
	LPSECURITY_ATTRIBUTES lpProcessAttributes,
	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	BOOL bInheritHandles,
	DWORD dwCreationFlags,
	LPVOID lpEnvironment,
	LPCSTR lpCurrentDirectory,
	LPSTARTUPINFOA lpSI,
	LPPROCESS_INFORMATION lpPI
)
{
	BOOL ret = chain_CreateProcessU(
		lpAppName, lpCmdLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles,
		dwCreationFlags | CREATE_SUSPENDED,
		lpEnvironment, lpCurrentDirectory, lpSI, lpPI
	);
	if(ret) {
		inject_CreateProcess_helper(lpPI, lpAppName, dwCreationFlags);
	}
	return ret;
}

BOOL WINAPI inject_CreateProcessW(
	LPCWSTR lpAppName,
	LPWSTR lpCmdLine,
	LPSECURITY_ATTRIBUTES lpProcessAttributes,
	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	BOOL bInheritHandles,
	DWORD dwCreationFlags,
	LPVOID lpEnvironment,
	LPCWSTR lpCurrentDirectory,
	LPSTARTUPINFOW lpSI,
	LPPROCESS_INFORMATION lpPI
)
{
	BOOL ret = chain_CreateProcessW(
		lpAppName, lpCmdLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles,
		dwCreationFlags | CREATE_SUSPENDED,
		lpEnvironment, lpCurrentDirectory, lpSI, lpPI
	);
	if(ret) {
		UTF8_DEC(lpAppName);
		UTF8_CONV(lpAppName);
		inject_CreateProcess_helper(lpPI, lpAppName_utf8, dwCreationFlags);
		UTF8_FREE(lpAppName);
	}
	return ret;
}

LPTHREAD_START_ROUTINE inject_change_start_func(
	HANDLE hProcess,
	LPTHREAD_START_ROUTINE lpStartAddress,
	FARPROC rep_func,
	const char *new_dll,
	const char *new_func
)
{
	LPTHREAD_START_ROUTINE ret = NULL;
	if((FARPROC)lpStartAddress == rep_func) {
		HMODULE hMod = GetRemoteModuleHandle(hProcess, new_dll);
		ret = (LPTHREAD_START_ROUTINE)GetRemoteProcAddress(hProcess, hMod, new_func);
	}
	return ret ? ret : lpStartAddress;
}

HANDLE WINAPI inject_CreateRemoteThread(
	HANDLE hProcess,
	LPSECURITY_ATTRIBUTES lpThreadAttributes,
	SIZE_T dwStackSize,
	LPTHREAD_START_ROUTINE lpFunc,
	LPVOID lpParameter,
	DWORD dwCreationFlags,
	LPDWORD lpThreadId
)
{
#ifdef _DEBUG
	const char *thcrap_dll = "thcrap_d.dll";
#else
	const char *thcrap_dll = "thcrap.dll";
#endif
	HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
	FARPROC kernel32_LoadLibraryA = GetProcAddress(hKernel32, "LoadLibraryA");
	FARPROC kernel32_LoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");

	lpFunc = inject_change_start_func(
		hProcess, lpFunc, kernel32_LoadLibraryA, thcrap_dll, "inject_LoadLibraryU"
	);
	lpFunc = inject_change_start_func(
		hProcess, lpFunc, kernel32_LoadLibraryW, thcrap_dll, "inject_LoadLibraryW"
	);

	return (HANDLE)chain_CreateRemoteThread(
		hProcess, lpThreadAttributes, dwStackSize, lpFunc,
		lpParameter, dwCreationFlags, lpThreadId
	);
}

HMODULE WINAPI inject_LoadLibraryU(
	LPCSTR lpLibFileName
)
{
	HMODULE ret = chain_LoadLibraryU(lpLibFileName);
	if(ret && inject_refcount_inc(ret) == 1) {
		thcrap_detour(ret);
	}
	return ret;
}

HMODULE WINAPI inject_LoadLibraryW(
	LPCWSTR lpLibFileName
)
{
	HMODULE ret = (HMODULE)chain_LoadLibraryW(lpLibFileName);
	if(ret && inject_refcount_inc(ret) == 1) {
		thcrap_detour(ret);
	}
	return ret;
}

BOOL WINAPI inject_FreeLibrary(
	HMODULE hLibModule
)
{
	inject_refcount_dec(hLibModule);
	return (BOOL)chain_FreeLibrary(hLibModule);
}

void inject_mod_detour(void)
{
	detour_chain("kernel32.dll", 1,
		"CreateProcessA", inject_CreateProcessU, &chain_CreateProcessU,
		"CreateProcessW", inject_CreateProcessW, &chain_CreateProcessW,
		"CreateRemoteThread", inject_CreateRemoteThread, &chain_CreateRemoteThread,
		"FreeLibrary", inject_FreeLibrary, &chain_FreeLibrary,
		"LoadLibraryA", inject_LoadLibraryU, &chain_LoadLibraryU,
		"LoadLibraryW", inject_LoadLibraryW, &chain_LoadLibraryW,
		NULL
	);
}

void inject_mod_exit(void)
{
	modules_detoured = json_decref_safe(modules_detoured);
}
