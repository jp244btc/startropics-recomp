/*
 * gpu_sync.c - Windows display synchronisation helpers.
 *
 * Frame pacing is phase-locked to the real vertical blank of the monitor the
 * window is on (IDXGIOutput::WaitForVBlank) rather than to Present() blocking,
 * which is unreliable in a window (the compositor, occlusion, 120 Hz panels).
 * The presentation queue is also limited to one frame so a vsynced Present
 * cannot run several frames ahead of the display.
 */
#include "gpu_sync.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <timeapi.h>
#include <SDL_system.h>
#include <SDL_syswm.h>

static IDXGIOutput *vblank_output;

int gpu_sync_init(SDL_Renderer *r) {
    ID3D11Device *dev = SDL_RenderGetD3D11Device(r);
    if (!dev) return 0;
    IDXGIDevice1 *dxgi = NULL;
    int ok = 0;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&dxgi)) && dxgi) {
        ok = SUCCEEDED(IDXGIDevice1_SetMaximumFrameLatency(dxgi, 1));
        IDXGIDevice1_Release(dxgi);
    }
    ID3D11Device_Release(dev);
    return ok;
}

int gpu_sync_vblank_init(SDL_Window *win) {
    if (vblank_output) { IDXGIOutput_Release(vblank_output); vblank_output = NULL; }
    HMONITOR mon = NULL;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (win && SDL_GetWindowWMInfo(win, &info)) mon = MonitorFromWindow(info.info.win.window, MONITOR_DEFAULTTONEAREST);

    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)) || !factory) return 0;
    IDXGIOutput *first = NULL;
    for (UINT a = 0; !vblank_output; a++) {
        IDXGIAdapter1 *adapter = NULL;
        if (IDXGIFactory1_EnumAdapters1(factory, a, &adapter) != S_OK) break;
        for (UINT o = 0; ; o++) {
            IDXGIOutput *out = NULL;
            if (IDXGIAdapter1_EnumOutputs(adapter, o, &out) != S_OK) break;
            DXGI_OUTPUT_DESC desc;
            if (mon && SUCCEEDED(IDXGIOutput_GetDesc(out, &desc)) && desc.Monitor == mon) { vblank_output = out; break; }
            if (!first) first = out; else IDXGIOutput_Release(out);
        }
        IDXGIAdapter1_Release(adapter);
    }
    IDXGIFactory1_Release(factory);
    if (!vblank_output) vblank_output = first;          /* no monitor match: use the first output */
    else if (first && first != vblank_output) IDXGIOutput_Release(first);
    return vblank_output != NULL;
}

int gpu_sync_wait_vblank(void) {
    if (!vblank_output) return 0;
    return SUCCEEDED(IDXGIOutput_WaitForVBlank(vblank_output));
}

void gpu_sync_timer_begin(void) { timeBeginPeriod(1); }
void gpu_sync_timer_end(void) { timeEndPeriod(1); }

#else
int gpu_sync_init(SDL_Renderer *r) { (void)r; return 0; }
int gpu_sync_vblank_init(SDL_Window *win) { (void)win; return 0; }
int gpu_sync_wait_vblank(void) { return 0; }
void gpu_sync_timer_begin(void) {}
void gpu_sync_timer_end(void) {}
#endif
