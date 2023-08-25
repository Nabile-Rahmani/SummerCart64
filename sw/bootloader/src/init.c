#include "error.h"
#include "exception.h"
#include "io.h"
#include "sc64.h"
#include "test.h"


void init (void) {
    sc64_error_t error;

    uint32_t pifram = si_io_read((io32_t *) (PIFRAM_STATUS));
    si_io_write((io32_t *) (PIFRAM_STATUS), pifram | PIFRAM_TERMINATE_BOOT);

    exception_install();

    sc64_unlock();

    if (!sc64_check_presence()) {
        error_display("SC64 hardware not detected");
    }

    exception_enable_watchdog();
    exception_enable_interrupts();

    if ((error = sc64_set_config(CFG_ID_BOOTLOADER_SWITCH, false)) != SC64_OK) {
        error_display("Command SET_CONFIG [BOOTLOADER_SWITCH] failed: %d", error);
    }

    if (test_check()) {
        exception_disable_watchdog();
        test_execute();
    }
}

void deinit (void) {
    sc64_lock();
    exception_disable_interrupts();
    exception_disable_watchdog();
}
