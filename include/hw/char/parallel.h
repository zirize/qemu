#ifndef HW_PARALLEL_H
#define HW_PARALLEL_H

#include "system/memory.h"
#include "hw/isa/isa.h"
#include "hw/core/irq.h"
#include "chardev/char-fe.h"
#include "chardev/char.h"

typedef struct ParallelState {
    MemoryRegion iomem;
    uint8_t dataw;
    uint8_t datar;
    uint8_t status;
    uint8_t control;
    qemu_irq irq;
    int irq_pending;
    CharFrontend chr;
    int hw_driver;
    int epp_timeout;
    /*
     * Byte latched on the rising edge of STROBE, waiting to be handed to the
     * back end.  While tx_pending is set the port keeps BUSY asserted, so the
     * guest is told to wait instead of the write blocking the vCPU thread.
     */
    uint8_t tx_data;
    bool tx_pending;
    guint tx_watch;
    uint32_t last_read_offset; /* For debugging */
    /* Memory-mapped interface */
    int it_shift;
} ParallelState;

void parallel_hds_isa_init(ISABus *bus, int n);

bool parallel_mm_init(MemoryRegion *address_space,
                      hwaddr base, int it_shift, qemu_irq irq,
                      Chardev *chr);

#endif
