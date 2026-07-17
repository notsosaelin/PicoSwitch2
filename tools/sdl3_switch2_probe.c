/*
 * Minimal diagnostic harness for Steam's bundled SDL3 Switch 2 HIDAPI driver.
 *
 * This intentionally loads SDL3.dll at runtime so it exercises the exact SDL
 * build Steam ships on the test PC, without requiring SDL headers or import
 * libraries. Run with Steam fully exited and one controller personality
 * connected. The important output is "SDL error immediately after init";
 * SDL's Switch 2 driver otherwise discards its InitUSB error when it falls
 * back to the generic Windows joystick path.
 *
 * Build (MSYS2 UCRT64):
 *   gcc -std=c11 -Wall -Wextra -O2 -o sdl3_switch2_probe.exe \
 *       tools/sdl3_switch2_probe.c
 *
 * Run:
 *   sdl3_switch2_probe.exe C:\\steamgames\\SDL3.dll
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint32_t SDL_JoystickID;

typedef unsigned char (__cdecl *pfn_SDL_Init)(uint32_t flags);
typedef void (__cdecl *pfn_SDL_Quit)(void);
typedef const char *(__cdecl *pfn_SDL_GetError)(void);
typedef void (__cdecl *pfn_SDL_SetLogPriorities)(int priority);
typedef void (__cdecl *pfn_SDL_SetLogOutputFunction)(
    void (__cdecl *callback)(void *userdata, int category, int priority, const char *message),
    void *userdata);
typedef unsigned char (__cdecl *pfn_SDL_SetHint)(const char *name, const char *value);
typedef void (__cdecl *pfn_SDL_Delay)(uint32_t ms);
typedef void (__cdecl *pfn_SDL_UpdateJoysticks)(void);
typedef SDL_JoystickID *(__cdecl *pfn_SDL_GetJoysticks)(int *count);
typedef const char *(__cdecl *pfn_SDL_GetJoystickStringForID)(SDL_JoystickID instance_id);
typedef uint16_t (__cdecl *pfn_SDL_GetJoystickU16ForID)(SDL_JoystickID instance_id);
typedef void (__cdecl *pfn_SDL_free)(void *memory);

static void __cdecl log_message(void *userdata, int category, int priority, const char *message) {
    (void)userdata;
    printf("SDL log category=%d priority=%d: %s\n", category, priority,
           message ? message : "(null)");
    fflush(stdout);
}
static FARPROC require_symbol(HMODULE dll, const char *name) {
    FARPROC symbol = GetProcAddress(dll, name);
    if (!symbol) {
        fprintf(stderr, "Missing SDL export %s (Windows error %lu)\n", name,
                (unsigned long)GetLastError());
        exit(2);
    }
    return symbol;
}

#define LOAD_FN(type, variable, name) type variable = (type)(uintptr_t)require_symbol(dll, name)

int main(int argc, char **argv) {
    const char *dll_path = argc > 1 ? argv[1] : "C:\\steamgames\\SDL3.dll";

    // libusb's own diagnostics go to stderr and can reveal a WinUSB open/claim failure
    // that SDL reduces to a single InitUSB error.
    SetEnvironmentVariableA("LIBUSB_DEBUG", "4");

    HMODULE dll = LoadLibraryExA(dll_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) {
        fprintf(stderr, "Could not load %s (Windows error %lu)\n", dll_path,
                (unsigned long)GetLastError());
        return 2;
    }

    LOAD_FN(pfn_SDL_Init, SDL_Init, "SDL_Init");
    LOAD_FN(pfn_SDL_Quit, SDL_Quit, "SDL_Quit");
    LOAD_FN(pfn_SDL_GetError, SDL_GetError, "SDL_GetError");
    LOAD_FN(pfn_SDL_SetLogPriorities, SDL_SetLogPriorities, "SDL_SetLogPriorities");
    LOAD_FN(pfn_SDL_SetLogOutputFunction, SDL_SetLogOutputFunction, "SDL_SetLogOutputFunction");
    LOAD_FN(pfn_SDL_SetHint, SDL_SetHint, "SDL_SetHint");
    LOAD_FN(pfn_SDL_Delay, SDL_Delay, "SDL_Delay");
    LOAD_FN(pfn_SDL_UpdateJoysticks, SDL_UpdateJoysticks, "SDL_UpdateJoysticks");
    LOAD_FN(pfn_SDL_GetJoysticks, SDL_GetJoysticks, "SDL_GetJoysticks");
    LOAD_FN(pfn_SDL_GetJoystickStringForID, SDL_GetJoystickNameForID, "SDL_GetJoystickNameForID");
    LOAD_FN(pfn_SDL_GetJoystickStringForID, SDL_GetJoystickPathForID, "SDL_GetJoystickPathForID");
    LOAD_FN(pfn_SDL_GetJoystickU16ForID, SDL_GetJoystickVendorForID, "SDL_GetJoystickVendorForID");
    LOAD_FN(pfn_SDL_GetJoystickU16ForID, SDL_GetJoystickProductForID, "SDL_GetJoystickProductForID");
    LOAD_FN(pfn_SDL_GetJoystickU16ForID, SDL_GetJoystickProductVersionForID,
            "SDL_GetJoystickProductVersionForID");
    LOAD_FN(pfn_SDL_free, SDL_free_fn, "SDL_free");

    SDL_SetLogOutputFunction(log_message, NULL);
    SDL_SetLogPriorities(1);  // SDL_LOG_PRIORITY_VERBOSE
    SDL_SetHint("SDL_JOYSTICK_HIDAPI", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_SWITCH2", "1");
    SDL_SetHint("SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS", "0");

    // SDL_INIT_JOYSTICK. HIDAPI setup, including Switch 2 InitUSB, occurs synchronously here.
    unsigned char initialized = SDL_Init(0x00000200u);
    const char *init_error = SDL_GetError();
    printf("SDL_Init returned: %s\n", initialized ? "true" : "false");
    printf("SDL error immediately after init: %s\n",
           (init_error && *init_error) ? init_error : "(empty)");

    SDL_Delay(500);
    SDL_UpdateJoysticks();

    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    printf("Visible joystick count: %d\n", count);
    for (int i = 0; i < count; ++i) {
        SDL_JoystickID id = ids[i];
        printf("  id=%lu vid=%04x pid=%04x version=%04x name=%s path=%s\n",
               (unsigned long)id,
               SDL_GetJoystickVendorForID(id),
               SDL_GetJoystickProductForID(id),
               SDL_GetJoystickProductVersionForID(id),
               SDL_GetJoystickNameForID(id) ? SDL_GetJoystickNameForID(id) : "(null)",
               SDL_GetJoystickPathForID(id) ? SDL_GetJoystickPathForID(id) : "(null)");
    }
    SDL_free_fn(ids);

    const char *final_error = SDL_GetError();
    printf("SDL error after enumeration: %s\n",
           (final_error && *final_error) ? final_error : "(empty)");

    SDL_Quit();
    FreeLibrary(dll);
    return initialized ? 0 : 1;
}
