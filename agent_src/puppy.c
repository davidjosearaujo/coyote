#include "frida-gum.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

typedef int (*SystemPropertyGetFunc)(const char *name, char *value);
static SystemPropertyGetFunc orig_system_property_get = NULL;

static int my_system_property_get(const char *name, char *value) {
    if (name != NULL && value != NULL) {
        for (size_t i = 0; i < SYSTEM_PROP_OVERRIDES_COUNT; i++) {
            if (strcmp(name, SYSTEM_PROP_OVERRIDES[i].key) == 0) {
                strcpy(value, SYSTEM_PROP_OVERRIDES[i].value);
                return (int)strlen(SYSTEM_PROP_OVERRIDES[i].value);
            }
        }
    }
    return orig_system_property_get(name, value);
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0);

    char buffer[128] = {0};

    printf("[*] Initializing Puppy Stealth Agent...\n");
    gum_init_embedded();

    GumInterceptor *interceptor = gum_interceptor_obtain();
    gpointer target_address = GSIZE_TO_POINTER(gum_module_find_global_export_by_name("__system_property_get"));

    if (target_address == NULL) {
        printf("[!] Failed to locate __system_property_get in memory.\n");
        gum_deinit_embedded();
        return 1;
    }

    printf("[*] Target symbol located at: %p\n", target_address);

    gum_interceptor_begin_transaction(interceptor);
    gum_interceptor_replace(
        interceptor,
        target_address,
        GSIZE_TO_POINTER(my_system_property_get),
        (gpointer *)&orig_system_property_get,
        NULL
    );
    gum_interceptor_end_transaction(interceptor);

    printf("[+] Interceptor active. Testing system property lookups:\n");

    my_system_property_get("ro.build.display.id", buffer);
    printf("  [>] ro.build.display.id -> %s\n", buffer);

    memset(buffer, 0, sizeof(buffer));
    __system_property_get("ro.build.version.release", buffer);
    printf("  [>] ro.build.version.release (Original) -> %s\n", buffer);

    gum_interceptor_revert(interceptor, target_address);
    g_object_unref(interceptor);
    gum_deinit_embedded();

    printf("[*] Puppy agent execution finished.\n");
    return 0;
}