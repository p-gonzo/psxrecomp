/* The SPU also has direct native callers (mods, snapshots and host pumping).
 * Publish preceding CD/device input before it catches its mixer up. During
 * device catch-up, callbacks must keep the intermediate event clock and must
 * not recursively synchronize. The maintained mixer body stays shared. */
#include "psx_cycles.h"
/* A snapshot restore reads the restored clock, not an observation: the device
 * deadlines still belong to the pre-load timeline until the restore resyncs
 * them, so observing here would replay every scheduled event in between (a
 * load ~7000 frames ahead ran ~7000 vblanks of catch-up inside the restore). */
static uint64_t psx_spu_restore_cycle_count(void) {
    return psx_get_cycle_count();
}
#define PSX_SPU_RESTORE_CYCLE_COUNT psx_spu_restore_cycle_count
static uint64_t psx_spu_observation_cycle_count(void) {
    if (!psx_in_device_service) psx_devices_observe();
    return psx_get_cycle_count();
}
#define psx_get_cycle_count psx_spu_observation_cycle_count
#include "spu.c"
