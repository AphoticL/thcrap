/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Direct memory patching and Import Address Table detouring.
  */

#pragma once

// IsBadReadPointer without the flawed implementation.
// Returns TRUE if [ptr] points to at least [len] bytes of valid memory in the
// address space of the current process.
BOOL VirtualCheckRegion(const void *ptr, const size_t len);
BOOL VirtualCheckCode(const void *ptr);

// Writes [len] bytes from [new] to [ptr] in the address space of the current
// or another process if the current value in [ptr] equals [prev].
int PatchRegion(void *ptr, const void *Prev, const void *New, size_t len);
int PatchRegionEx(HANDLE hProcess, void *ptr, const void *Prev, const void *New, size_t len);

/// Import Address Table patching
/// =============================

// Information about a single function to detour
typedef struct {
	const char *old_func;
	const void *old_ptr;
	const void *new_ptr;
} iat_detour_t;

/// Low-level
/// ---------
// Replaces the function pointer of [pThunk] with [new_ptr]
int func_detour(PIMAGE_THUNK_DATA pThunk, const void *new_ptr);

// Sets up [detour] by name or pointer.
// Returns 1 if the function was found and detoured, 0 if it wasn't.
int func_detour_by_name(HMODULE hMod, PIMAGE_THUNK_DATA pOrigFirstThunk, PIMAGE_THUNK_DATA pImpFirstThunk, const iat_detour_t *detour);
int func_detour_by_ptr(PIMAGE_THUNK_DATA pImpFirstThunk, const iat_detour_t *detour);
/// ---------

/// High-level
/// ----------
// Sets up [detour] using the most appropriate low-level detouring function.
int iat_detour_func(HMODULE hMod, PIMAGE_IMPORT_DESCRIPTOR pImpDesc, const iat_detour_t *detour);
/// ----------

/// Detour chaining
/// ---------------
/**
  * This system allows the support of an arbitrary number of detour hooks
  * for any given API function.
  *
  * Prior to this, detours weren't really reliably extensible by plugins.
  * Since they mostly need to call the original function they hook, they used
  * to imply a certain hierarchy and required knowledge of the entire
  * detour sequence (with the win32_utf8 functions mostly being on the lowest
  * level) in order to work as intended. This could have led to cases where
  * newer detours could have potentially override existing functionality
  * (see https://bitbucket.org/nmlgc/thpatch-bugs/issue/18).
  *
  * It took a while to come up with a proper solution to this problem that
  * wouldn't impose any restrictions on what detour functions are able to do.
  * After all, they may need to call the original function multiple times,
  * or pass locally constructed data as a parameter.
  *
  * After some further thought, it then became clear that, for most reasonable
  * cases where detours only modify function parameters or return values, the
  * order in which they are called doesn't actually matter. Sure, win32_utf8
  * must be at the bottom, but as an integral part of the engine, it can
  * always be treated separately.
  *
  * This realization allows us to build a "detour chaining" mechanism, similar
  * to the interrupt chaining used in MS-DOS TSR applications. Instead of
  * patching the IAT directly with some function pointer, modules first
  * register sequences of detours for each API function in advance, using
  * detour_chain().
  * For every detoured function, detour_chain() records the pointer to the
  * last hook registered. Adding a new hook by calling detour_chain() again
  * for the same DLL and function then returns this pointer, before replacing
  * it with the new one.
  *
  * Detours then use their returned chaining pointer to invoke the original
  * functionality anywhere they need it. As a result, this calls all
  * subsequent hooks in the chain recursively.
  * By initalizing its chaining pointer to point to the original API function,
  * every link in the chain also defaults to well-defined, reliable end point.
  *
  * To apply the chain, we then simply patch the IAT with the last recorded
  * function pointers, once we have a module handle.
  */

// Defines a function pointer used to continue a detour chain. It is
// initialized to [func], which should the default function to be called
// if this is the last link in the chain. The pointer will be of the type
// "[func]_type", which must have been typedef'd before.
#define DETOUR_CHAIN_DEF(func) \
	static func##_type *chain_##func = func;

// DETOUR_CHAIN_DEF for wrappers defined in win32_utf8.
#define W32U8_DETOUR_CHAIN_DEF(func) \
	static func##A_type *chain_##func##U = func##U;

/**
  * Inserts a new hook for a number of functions in [dll_name] at the
  * beginning of their respective detour chains. For each function,
  * 3 or 2 (if return_old_ptrs == 0) additional parameters of the form
  *
  *	"exported name", new_func_ptr, (&old_func_ptr,)
  *	"exported name", new_func_ptr, (&old_func_ptr,)
  *	...,
  *
  * are consumed.
  * If [return_old_ptrs] is nonzero, [old_func_ptr] is set to the next
  * function pointer in the chain, or left unchanged if no hook was
  * registered before.
  */
int detour_chain(const char *dll_name, int return_old_ptrs, ...);
/**
  * detour_chain() for the function pair list of a single DLL returned by
  * win32_utf8. Does obviously not return any pointers to functions that may
  * have been part of the respective chains before.
  */
int detour_chain_w32u8(const w32u8_dll_t *dll);

// Returns a pointer to the first function in a specific detour chain, or
// [fallback] if no hook has been registered so far.
FARPROC detour_top(const char *dll_name, const char *func_name, FARPROC fallback);

// Applies the cached detours to [hMod].
int iat_detour_apply(HMODULE hMod);

// *Not* a module function because we want to call it manually after
// everything else has been cleaned up.
void detour_exit(void);
/// ---------------

/// =============================
