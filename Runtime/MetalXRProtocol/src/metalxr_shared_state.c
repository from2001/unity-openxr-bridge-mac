#include "MetalXRProtocol/metalxr_shared_state.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t atomic_load_u32(const uint32_t* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static uint64_t atomic_load_u64(const uint64_t* value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void atomic_store_u32(uint32_t* value, uint32_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static void atomic_store_u64(uint64_t* value, uint64_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static uint32_t begin_write(uint32_t* sequence)
{
    uint32_t next = atomic_load_u32(sequence) + 1u;
    if ((next & 1u) == 0u) {
        ++next;
    }
    atomic_store_u32(sequence, next);
    return next + 1u;
}

static void end_write(uint32_t* sequence, uint32_t nextEvenSequence)
{
    atomic_store_u32(sequence, nextEvenSequence);
}

static int read_consistent(
    const uint32_t* sequence,
    const void* source,
    void* destination,
    size_t size,
    uint32_t* outputSequence)
{
    if (sequence == NULL || source == NULL || destination == NULL) {
        return 0;
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t before = atomic_load_u32(sequence);
        if (before == 0u || (before & 1u) != 0u) {
            continue;
        }

        memcpy(destination, source, size);

        const uint32_t after = atomic_load_u32(sequence);
        if (before == after && (after & 1u) == 0u) {
            if (outputSequence != NULL) {
                *outputSequence = after;
            }
            return 1;
        }
    }

    return 0;
}

static void set_error(char* errorMessage, size_t errorMessageSize, const char* message)
{
    if (errorMessage == NULL || errorMessageSize == 0) {
        return;
    }

    snprintf(errorMessage, errorMessageSize, "%s", message != NULL ? message : "unknown shared state error");
}

const char* metalxr_shared_state_default_name(void)
{
    return METALXR_SHARED_STATE_DEFAULT_NAME;
}

static const char* normalize_name(const char* name)
{
    return name != NULL && name[0] != '\0' ? name : METALXR_SHARED_STATE_DEFAULT_NAME;
}

static void initialize_state(MetalXRSharedState* state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->magic = METALXR_SHARED_STATE_MAGIC;
    state->version = METALXR_SHARED_STATE_VERSION;
    state->size = (uint32_t)sizeof(*state);
    state->tracking.hmd.orientation[3] = 1.0f;
    state->tracking.hmd.trackingFlags = METALXR_TRACKING_ORIENTATION_VALID |
                                        METALXR_TRACKING_POSITION_VALID;
    for (size_t hand = 0; hand < 2; ++hand) {
        state->tracking.controllers[hand].hand = (uint32_t)hand;
        state->tracking.controllers[hand].aimOrientation[3] = 1.0f;
        state->tracking.controllers[hand].gripOrientation[3] = 1.0f;
    }
}

int metalxr_shared_state_open(
    MetalXRSharedStateMapping* mapping,
    const char* name,
    int create,
    char* errorMessage,
    size_t errorMessageSize)
{
    if (mapping == NULL) {
        set_error(errorMessage, errorMessageSize, "mapping is null");
        return 0;
    }

    memset(mapping, 0, sizeof(*mapping));
    mapping->fd = -1;
    mapping->size = sizeof(MetalXRSharedState);

    const char* normalizedName = normalize_name(name);
    if (normalizedName[0] != '/') {
        set_error(errorMessage, errorMessageSize, "POSIX shared memory name must start with '/'");
        return 0;
    }
    snprintf(mapping->name, sizeof(mapping->name), "%s", normalizedName);

    const int flags = O_RDWR | (create ? O_CREAT : 0);
    int fd = shm_open(mapping->name, flags, 0600);
    if (fd < 0) {
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "shm_open(%s) failed: %s", mapping->name, strerror(errno));
        set_error(errorMessage, errorMessageSize, buffer);
        return 0;
    }

    if (create && ftruncate(fd, (off_t)mapping->size) != 0) {
        const int firstErrno = errno;
        close(fd);
        fd = -1;

        if (firstErrno == EINVAL) {
            (void)shm_unlink(mapping->name);
            fd = shm_open(mapping->name, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0 && ftruncate(fd, (off_t)mapping->size) == 0) {
                goto mapped_open;
            }
        }

        char buffer[160];
        snprintf(buffer,
                 sizeof(buffer),
                 "ftruncate(%s) failed: %s",
                 mapping->name,
                 strerror(fd >= 0 ? errno : firstErrno));
        if (fd >= 0) {
            close(fd);
        }
        set_error(errorMessage, errorMessageSize, buffer);
        return 0;
    }

mapped_open:
    ;
    void* mapped = mmap(NULL, mapping->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        char buffer[160];
        snprintf(buffer, sizeof(buffer), "mmap(%s) failed: %s", mapping->name, strerror(errno));
        close(fd);
        set_error(errorMessage, errorMessageSize, buffer);
        return 0;
    }

    mapping->fd = fd;
    mapping->state = (MetalXRSharedState*)mapped;

    if (create) {
        initialize_state(mapping->state);
    }

    if (mapping->state->magic != METALXR_SHARED_STATE_MAGIC ||
        mapping->state->version != METALXR_SHARED_STATE_VERSION ||
        mapping->state->size != sizeof(MetalXRSharedState)) {
        metalxr_shared_state_close(mapping);
        set_error(errorMessage, errorMessageSize, "shared state header is incompatible");
        return 0;
    }

    return 1;
}

void metalxr_shared_state_close(MetalXRSharedStateMapping* mapping)
{
    if (mapping == NULL) {
        return;
    }

    if (mapping->state != NULL) {
        munmap(mapping->state, mapping->size);
    }
    if (mapping->fd >= 0) {
        close(mapping->fd);
    }

    memset(mapping, 0, sizeof(*mapping));
    mapping->fd = -1;
}

void metalxr_shared_state_write_host_heartbeat(
    MetalXRSharedStateMapping* mapping,
    uint64_t hostTimestampNs)
{
    if (mapping == NULL || mapping->state == NULL) {
        return;
    }

    atomic_store_u64(&mapping->state->hostHeartbeatNs, hostTimestampNs);
}

uint64_t metalxr_shared_state_host_heartbeat_ns(MetalXRSharedStateMapping* mapping)
{
    if (mapping == NULL || mapping->state == NULL) {
        return 0;
    }

    return atomic_load_u64(&mapping->state->hostHeartbeatNs);
}

void metalxr_shared_state_write_tracking(
    MetalXRSharedStateMapping* mapping,
    const MetalXRSharedTrackingState* tracking)
{
    if (mapping == NULL || mapping->state == NULL || tracking == NULL) {
        return;
    }

    const uint32_t sequence = begin_write(&mapping->state->trackingSequence);
    memcpy(&mapping->state->tracking, tracking, sizeof(*tracking));
    end_write(&mapping->state->trackingSequence, sequence);
}

int metalxr_shared_state_read_tracking(
    MetalXRSharedStateMapping* mapping,
    MetalXRSharedTrackingState* tracking,
    uint32_t* sequence)
{
    if (mapping == NULL || mapping->state == NULL) {
        return 0;
    }

    return read_consistent(&mapping->state->trackingSequence,
                           &mapping->state->tracking,
                           tracking,
                           sizeof(*tracking),
                           sequence);
}

void metalxr_shared_state_write_timing(
    MetalXRSharedStateMapping* mapping,
    const MetalXRSharedTimingState* timing)
{
    if (mapping == NULL || mapping->state == NULL || timing == NULL) {
        return;
    }

    const uint32_t sequence = begin_write(&mapping->state->timingSequence);
    memcpy(&mapping->state->timing, timing, sizeof(*timing));
    end_write(&mapping->state->timingSequence, sequence);
}

int metalxr_shared_state_read_timing(
    MetalXRSharedStateMapping* mapping,
    MetalXRSharedTimingState* timing,
    uint32_t* sequence)
{
    if (mapping == NULL || mapping->state == NULL) {
        return 0;
    }

    return read_consistent(&mapping->state->timingSequence,
                           &mapping->state->timing,
                           timing,
                           sizeof(*timing),
                           sequence);
}

void metalxr_shared_state_write_haptic(
    MetalXRSharedStateMapping* mapping,
    const MetalXRHapticCommandPayload* haptic)
{
    if (mapping == NULL || mapping->state == NULL || haptic == NULL) {
        return;
    }

    const uint32_t sequence = begin_write(&mapping->state->hapticSequence);
    memcpy(&mapping->state->haptic, haptic, sizeof(*haptic));
    end_write(&mapping->state->hapticSequence, sequence);
}

int metalxr_shared_state_read_haptic(
    MetalXRSharedStateMapping* mapping,
    MetalXRHapticCommandPayload* haptic,
    uint32_t* sequence)
{
    if (mapping == NULL || mapping->state == NULL) {
        return 0;
    }

    return read_consistent(&mapping->state->hapticSequence,
                           &mapping->state->haptic,
                           haptic,
                           sizeof(*haptic),
                           sequence);
}
