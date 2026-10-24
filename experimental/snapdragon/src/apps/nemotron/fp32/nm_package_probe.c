#include "../nemotron_windows.h"

__declspec(dllimport) void *LoadLibraryExW(const u16 *, void *, u32);
__declspec(dllimport) int MultiByteToWideChar(u32, u32, const char *, int, u16 *, int);
__declspec(dllimport) u32 GetLastError(void);
static char arguments[8][4096], backend_path[4096], host_path[4096];
static void *load(const char *path) {
    static u16 wide[32768];
    void *module;
    if (!MultiByteToWideChar(65001, 8, path, -1, wide, 32768)) return 0;
    module = LoadLibraryExW(wide, 0, 0x1100);
    if (!module) {
        nm_text("LoadLibraryExW error="); nm_number(GetLastError()); nm_text("\n");
    }
    return module;
}
void mainCRTStartup(void) {
    QnnInterfaceGetProviders get;
    const QnnInterfacePrefix **providers = 0;
    QnnInterfaceV2 api = {0};
    QnnBackendHandle backend = 0;
    QnnContextHandle context = 0;
    void *module = 0, *host = 0;
    const char *target;
    u64 status;
    u32 count = 0, i;
    int ok = 0;
    nm_debug_init();
    if (nm_arguments(arguments) != 2 ||
        (rt_strcmp(arguments[1], "CPU") && rt_strcmp(arguments[1], "DEFAULT"))) goto done;
    target = !rt_strcmp(arguments[1], "DEFAULT") ? 0 : "CPU";
    if (nm_package_path("NM_FP32_LOADER_BACKEND", "NM_FP32_LOADER_BACKEND_SHA256", backend_path) != 1 ||
        nm_package_path("NM_FP32_HOST_PACKAGE", "NM_FP32_HOST_SHA256", host_path) != 1) goto done;
    module = load(backend_path);
    if (!module) goto done;
    for (i = 0; i < 6; ++i) {
        static const char *symbols[] = {
            "qhpi_register_ops_v1", "qhpi_tensor_shape", "qhpi_tensor_type",
            "qhpi_tensor_raw_data", "qhpi_num_slices", "qhpi_slice_number"
        };
        nm_text("backend export "); nm_text(symbols[i]); nm_text(" present=");
        nm_number(nm_symbol(module, symbols[i]) != 0); nm_text("\n");
    }
    host = load(host_path);
    nm_text("Own host DLL LoadLibrary="); nm_number(host != 0); nm_text("\n");
    if (!host) goto done;
    nm_text("Own host provider present="); nm_number(nm_symbol(host, "NmFp32InterfaceProvider") != 0); nm_text("\n");
    nm_text("QHPI entry present="); nm_number(nm_symbol(host, "qhpi_init") != 0);
    nm_text(" legacy entry present="); nm_number(nm_symbol(host, "op_pkg_init") != 0); nm_text("\n");
    get = (QnnInterfaceGetProviders)nm_symbol(module, "QnnInterface_getProviders");
    if (!get || get(&providers, &count) || !providers || count > 64) goto done;
    for (i = 0; i < count; ++i) {
        const QnnInterfacePrefix *p = providers[i];
        if (p && p->api_version.core_api_version.major == 2 && p->api_version.core_api_version.minor == 39) {
            api = ((const QnnInterfaceProviderV2 *)p)->api; break;
        }
    }
    if (i == count || !api.backend_create || !api.backend_register_op_package ||
        !api.backend_free || !api.context_create || !api.context_free) goto done;
    nm_boundary("backendCreate", backend_path, "loader diagnostic only");
    status = api.backend_create(0, 0, &backend);
    nm_text("backendCreate status="); nm_number(status); nm_text("\n");
    if (status) goto done;
    nm_boundary("backendRegisterOpPackage", host_path, target ? target : "NULL/default");
    status = api.backend_register_op_package(backend, host_path, "NmFp32InterfaceProvider", target);
    nm_text("register target="); nm_text(target ? target : "NULL"); nm_text(" status="); nm_number(status); nm_text("\n");
    {
        const volatile u32 *reached = nm_symbol(host, "NmFp32LegacyEntryReached");
        if (reached) { nm_text("Legacy SDK entry reached="); nm_number(*reached); nm_text("\n"); }
    }
    if (status) goto done;
    nm_boundary("contextCreate", host_path, "force deferred package loading; no graph execution");
    status = api.context_create(backend, 0, 0, &context);
    nm_text("contextCreate status="); nm_number(status); nm_text("\n");
    ok = !status;
done:
    if (context) api.context_free(context, 0);
    if (backend) api.backend_free(backend);
    nm_unload(host); nm_unload(module);
    nm_text("No graph/model execution attempted\n");
    ExitProcess(ok ? 0 : 1);
}
