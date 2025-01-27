#include "boot.h"
#include "error.h"
#include "init.h"
#include "io.h"
#include "menu.h"
#include "sc64.h"


void main (void) {
    boot_info_t boot_info;
    sc64_boot_info_t sc64_boot_info;

    sc64_get_boot_info(&sc64_boot_info);

    switch (sc64_boot_info.boot_mode) {
        case BOOT_MODE_MENU:
            menu_load_and_run();
            break;

        case BOOT_MODE_ROM:
            boot_info.device_type = BOOT_DEVICE_TYPE_ROM;
            break;

        case BOOT_MODE_DDIPL:
            boot_info.device_type = BOOT_DEVICE_TYPE_DD;
            break;

        default:
            error_display("Unknown boot mode selected [%d]\n", sc64_boot_info.boot_mode);
            break;
    }

    bool detect_tv_type = (sc64_boot_info.tv_type == TV_TYPE_UNKNOWN);
    bool detect_cic_seed = (sc64_boot_info.cic_seed == CIC_SEED_UNKNOWN);

    boot_info.reset_type = OS_INFO->reset_type;
    boot_info.tv_type = sc64_boot_info.tv_type;
    boot_info.cic_seed = (sc64_boot_info.cic_seed & 0xFF);

    deinit();

    boot(&boot_info, detect_tv_type, detect_cic_seed);
}
