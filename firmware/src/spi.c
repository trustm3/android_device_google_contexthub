#include <errno.h>
#include <heap.h>
#include <string.h>

#include <cpu.h>
#include <spi.h>
#include <spi_priv.h>

struct SpiDeviceState {
    struct SpiDevice dev;

    void **rxBuf;
    const void **txBuf;
    size_t *size;
    size_t n;
    size_t currentBuf;
    struct SpiMode mode;

    SpiCbkF rxTxCallback;
    void *rxTxCookie;

    SpiCbkF finishCallback;
    void *finishCookie;

    int err;
};
#define SPI_DEVICE_TO_STATE(p) ((struct SpiDeviceState *)p)

static void spiMasterNext(struct SpiDeviceState *state);
static void spiMasterStop(struct SpiDeviceState *state);
static void spiMasterDone(struct SpiDeviceState *state, int err);

static void spiSlaveNext(struct SpiDeviceState *state);
static void spiSlaveIdle(struct SpiDeviceState *state, int err);
static void spiSlaveDone(struct SpiDeviceState *state);

static void spiBufsFree(struct SpiDeviceState *state);

static int spiMasterStart(struct SpiDeviceState *state,
        spi_cs_t cs, const struct SpiMode *mode)
{
    struct SpiDevice *dev = &state->dev;

    if (dev->ops->masterStartAsync)
        return dev->ops->masterStartAsync(dev, cs, mode);

    if (dev->ops->masterStartSync) {
        int err = dev->ops->masterStartSync(dev, cs, mode);
        if (err < 0)
            return err;
    }

    return dev->ops->masterRxTx(dev, state->rxBuf[0], state->txBuf[0],
            state->size[0], mode);
}

void spi_masterStartAsync_done(struct SpiDevice *dev, int err)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);
    if (err)
        spiMasterDone(state, err);
    else
        spiMasterNext(state);
}

static void spiMasterNext(struct SpiDeviceState *state)
{
    struct SpiDevice *dev = &state->dev;

    if (state->currentBuf == state->n) {
        spiMasterStop(state);
        return;
    }

    size_t i = state->currentBuf;
    void *rxBuf = state->rxBuf[i];
    const void *txBuf = state->txBuf[i];
    size_t size = state->size[i];
    const struct SpiMode *mode = &state->mode;

    int err = dev->ops->masterRxTx(dev, rxBuf, txBuf, size, mode);
    if (err)
        spiMasterDone(state, err);
}

void spiMasterRxTxDone(struct SpiDevice *dev, int err)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);
    if (err) {
        spiMasterDone(state, err);
    } else {
        state->currentBuf++;
        spiMasterNext(state);
    }
}

static void spiMasterStop(struct SpiDeviceState *state)
{
    struct SpiDevice *dev = &state->dev;

    if (dev->ops->masterStopSync) {
        int err = dev->ops->masterStopSync(dev);
        spiMasterDone(state, err);
    } else if (dev->ops->masterStopAsync) {
        int err = dev->ops->masterStopAsync(dev);
        if (err < 0)
            spiMasterDone(state, err);
    } else {
        spiMasterDone(state, 0);
    }
}

void spiMasterStopAsyncDone(struct SpiDevice *dev, int err)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);
    spiMasterDone(state, err);
}

static void spiMasterDone(struct SpiDeviceState *state, int err)
{
    struct SpiDevice *dev = &state->dev;
    SpiCbkF callback = state->rxTxCallback;
    void *cookie = state->rxTxCookie;

    spiBufsFree(state);
    if (dev->ops->release)
        dev->ops->release(dev);
    heapFree(state);

    callback(cookie, err);
}

static int spiSlaveStart(struct SpiDeviceState *state,
        const struct SpiMode *mode)
{
    struct SpiDevice *dev = &state->dev;

    if (dev->ops->slaveStartAsync)
        return dev->ops->slaveStartAsync(dev, mode);

    if (dev->ops->slaveStartSync) {
        int err = dev->ops->slaveStartSync(dev, mode);
        if (err < 0)
            return err;
    }

    return dev->ops->slaveIdle(dev, mode);
}

void spiSlaveStartAsyncDone(struct SpiDevice *dev, int err)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);

    if (err)
        state->err = err;
    else
        state->err = dev->ops->slaveIdle(dev, &state->mode);
}

void spiSlaveRxTxDone(struct SpiDevice *dev, int err)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);

    if (err) {
        spiSlaveIdle(state, err);
    } else {
        state->currentBuf++;
        spiSlaveNext(state);
    }
}

void spiSlaveCsInactive(struct SpiDevice *dev)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);

    dev->ops->slaveSetCsInterrupt(dev, false);

    if (!state->finishCallback) {
        osLog(LOG_WARN, "%s called without callback\n", __func__);
        return;
    }

    SpiCbkF callback = state->finishCallback;
    void *cookie = state->finishCookie;
    state->finishCallback = NULL;
    state->finishCookie = NULL;

    callback(cookie, 0);
}

static void spiSlaveNext(struct SpiDeviceState *state)
{
    struct SpiDevice *dev = &state->dev;

    if (state->currentBuf == state->n) {
        spiSlaveIdle(state, 0);
        return;
    }

    size_t i = state->currentBuf;
    void *rxBuf = state->rxBuf[i];
    const void *txBuf = state->txBuf[i];
    size_t size = state->size[i];
    const struct SpiMode *mode = &state->mode;

    int err = dev->ops->slaveRxTx(dev, rxBuf, txBuf, size, mode);
    if (err)
        spiSlaveIdle(state, err);
}

static void spiSlaveIdle(struct SpiDeviceState *state, int err)
{
    struct SpiDevice *dev = &state->dev;
    SpiCbkF callback = state->rxTxCallback;
    void *cookie = state->rxTxCookie;

    if (!err)
        err = dev->ops->slaveIdle(dev, &state->mode);

    spiBufsFree(state);
    callback(cookie, err);
}

void spiSlaveStopAsyncDone(struct SpiDevice *dev, int err)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);
    spiSlaveDone(state);
}

static void spiSlaveDone(struct SpiDeviceState *state)
{
    struct SpiDevice *dev = &state->dev;

    if (dev->ops->release)
        dev->ops->release(dev);
    heapFree(state);
}

static int spiSetupRxTx(struct SpiDeviceState *state,
        void *rxBuf[], const void *txBuf[], size_t size[], size_t n,
        SpiCbkF callback, void *cookie)
{
    state->rxBuf = heapAlloc(n * sizeof(*rxBuf));
    state->txBuf = heapAlloc(n * sizeof(*txBuf));
    state->size = heapAlloc(n * sizeof(*size));

    if (!state->rxBuf || !state->txBuf || !state->size) {
        spiBufsFree(state);
        return -ENOMEM;
    }

    memcpy(state->rxBuf, rxBuf, n * sizeof(*rxBuf));
    memcpy(state->txBuf, txBuf, n * sizeof(*txBuf));
    memcpy(state->size, size, n * sizeof(*size));
    state->n = n;
    state->currentBuf = 0;
    state->rxTxCallback = callback;
    state->rxTxCookie = cookie;

    return 0;
}

static void spiBufsFree(struct SpiDeviceState *state)
{
    heapFree(state->rxBuf);
    heapFree(state->txBuf);
    heapFree(state->size);
}

int spiMasterRxTx(uint8_t busId, spi_cs_t cs,
        void *rxBuf[], const void *txBuf[], size_t size[], size_t n,
        const struct SpiMode *mode, SpiCbkF callback,
        void *cookie)
{
    int ret = 0;

    if (!n)
        return -EINVAL;

    struct SpiDeviceState *state = heapAlloc(sizeof(*state));
    if (!state)
        return -ENOMEM;
    struct SpiDevice *dev = &state->dev;

    ret = spiRequest(dev, busId);
    if (ret < 0)
        goto err_request;

    if (!dev->ops->masterRxTx) {
        ret = -EOPNOTSUPP;
        goto err_opsupp;
    }

    ret = spiSetupRxTx(state, rxBuf, txBuf, size, n, callback, cookie);
    if (ret < 0)
        goto err_opsupp;

    state->mode = *mode;

    ret = spiMasterStart(state, cs, mode);
    if (ret < 0)
        goto err_start;

    return 0;

err_start:
    spiBufsFree(state);
err_opsupp:
    if (dev->ops->release)
        dev->ops->release(dev);
err_request:
    heapFree(state);
    return ret;
}

int spiSlaveRequest(uint8_t busId, const struct SpiMode *mode,
        struct SpiDevice **dev_out)
{
    int ret = 0;

    struct SpiDeviceState *state = heapAlloc(sizeof(*state));
    if (!state)
        return -ENOMEM;
    struct SpiDevice *dev = &state->dev;

    ret = spiRequest(dev, busId);
    if (ret < 0)
        goto err_request;

    if (!dev->ops->slaveIdle || !dev->ops->slaveRxTx) {
        ret = -EOPNOTSUPP;
        goto err_opsupp;
    }

    state->mode = *mode;
    state->err = 0;

    ret = spiSlaveStart(state, mode);
    if (ret < 0)
        goto err_opsupp;

    *dev_out = dev;
    return 0;

err_opsupp:
    if (dev->ops->release)
        dev->ops->release(dev);
err_request:
    heapFree(state);
    return ret;
}

int spiSlaveRxTx(struct SpiDevice *dev,
        void *rxBuf[], const void *txBuf[], size_t size[], size_t n,
        SpiCbkF callback, void *cookie)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);

    if (!n)
        return -EINVAL;

    if (state->err)
        return state->err;

    int ret = spiSetupRxTx(state, rxBuf, txBuf, size, n, callback, cookie);
    if (ret < 0)
        return ret;

    return dev->ops->slaveRxTx(dev, state->rxBuf[0], state->txBuf[0],
            state->size[0], &state->mode);
}

int spiSlaveWaitForInactive(struct SpiDevice *dev, SpiCbkF callback,
        void *cookie)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);

    if (!dev->ops->slaveSetCsInterrupt || !dev->ops->slaveCsIsActive)
        return -EOPNOTSUPP;

    state->finishCallback = callback;
    state->finishCookie = cookie;

    uint64_t flags = cpuIntsOff();
    dev->ops->slaveSetCsInterrupt(dev, true);

    /* CS may already be inactive before enabling the interrupt.  In this case
     * roll back and fire the callback immediately.
     *
     * Interrupts must be off while checking for this.  Otherwise there is a
     * (very unlikely) race where the CS interrupt fires between calling
     * slaveSetCsInterrupt(true) and the rollback
     * slaveSetCsInterrupt(false), causing the event to be handled twice.
     *
     * Likewise the check must come after enabling the interrupt.  Otherwise
     * there is an (also unlikely) race where CS goes inactive between reading
     * CS and enabling the interrupt, causing the event to be lost.
     */

    bool cs = dev->ops->slaveCsIsActive(dev);
    if (!cs) {
        dev->ops->slaveSetCsInterrupt(dev, false);
        cpuIntsRestore(flags);

        state->finishCallback = NULL;
        state->finishCookie = NULL;
        callback(cookie, 0);
        return 0;
    }

    cpuIntsRestore(flags);
    return 0;
}

int spiSlaveRelease(struct SpiDevice *dev)
{
    struct SpiDeviceState *state = SPI_DEVICE_TO_STATE(dev);
    int ret;

    if (dev->ops->slaveStopSync) {
        ret = dev->ops->slaveStopSync(dev);
        if (ret < 0)
            return ret;
    } else if (dev->ops->slaveStopAsync) {
        return dev->ops->slaveStopAsync(dev);
    }

    spiSlaveDone(state);
    return 0;
}
