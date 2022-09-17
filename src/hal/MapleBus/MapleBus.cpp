#include "MapleBus.hpp"
#include "pico/stdlib.h"
#include "hardware/structs/systick.h"
#include "hardware/regs/m0plus.h"
#include "hardware/irq.h"
#include "configuration.h"
#include "maple.pio.h"
#include "string.h"
#include "utils.h"

MapleBus* mapleWriteIsr[4] = {};
MapleBus* mapleReadIsr[4] = {};

extern "C"
{
void maple_write_isr0(void)
{
    if (MAPLE_OUT_PIO->irq & (0x01))
    {
        mapleWriteIsr[0]->writeIsr();
        hw_set_bits(&MAPLE_OUT_PIO->irq, 0x01);
    }
    if (MAPLE_OUT_PIO->irq & (0x04))
    {
        mapleWriteIsr[2]->writeIsr();
        hw_set_bits(&MAPLE_OUT_PIO->irq, 0x04);
    }
}
void maple_write_isr1(void)
{
    if (MAPLE_OUT_PIO->irq & (0x02))
    {
        mapleWriteIsr[1]->writeIsr();
        hw_set_bits(&MAPLE_OUT_PIO->irq, 0x02);
    }
    if (MAPLE_OUT_PIO->irq & (0x08))
    {
        mapleWriteIsr[3]->writeIsr();
        hw_set_bits(&MAPLE_OUT_PIO->irq, 0x08);
    }
}
void maple_read_isr0(void)
{
    if (MAPLE_IN_PIO->irq & (0x01))
    {
        mapleReadIsr[0]->readIsr();
        hw_set_bits(&MAPLE_IN_PIO->irq, 0x01);
    }
    if (MAPLE_IN_PIO->irq & (0x04))
    {
        mapleReadIsr[2]->readIsr();
        hw_set_bits(&MAPLE_IN_PIO->irq, 0x04);
    }
}
void maple_read_isr1(void)
{
    if (MAPLE_IN_PIO->irq & (0x02))
    {
        mapleReadIsr[1]->readIsr();
        hw_set_bits(&MAPLE_IN_PIO->irq, 0x02);
    }
    if (MAPLE_IN_PIO->irq & (0x08))
    {
        mapleReadIsr[3]->readIsr();
        hw_set_bits(&MAPLE_IN_PIO->irq, 0x08);
    }
}
}

void MapleBus::initIsrs()
{
    uint outIdx = pio_get_index(MAPLE_OUT_PIO);
    irq_set_exclusive_handler(PIO0_IRQ_0 + (outIdx * 2), maple_write_isr0);
    irq_set_exclusive_handler(PIO0_IRQ_1 + (outIdx * 2), maple_write_isr1);
    irq_set_enabled(PIO0_IRQ_0 + (outIdx * 2), true);
    irq_set_enabled(PIO0_IRQ_1 + (outIdx * 2), true);
    pio_set_irq0_source_enabled(MAPLE_OUT_PIO, pis_interrupt0, true);
    pio_set_irq1_source_enabled(MAPLE_OUT_PIO, pis_interrupt1, true);
    pio_set_irq0_source_enabled(MAPLE_OUT_PIO, pis_interrupt2, true);
    pio_set_irq1_source_enabled(MAPLE_OUT_PIO, pis_interrupt3, true);

    uint inIdx = pio_get_index(MAPLE_IN_PIO);
    irq_set_exclusive_handler(PIO0_IRQ_0 + (inIdx * 2), maple_read_isr0);
    irq_set_exclusive_handler(PIO0_IRQ_1 + (inIdx * 2), maple_read_isr1);
    irq_set_enabled(PIO0_IRQ_0 + (inIdx * 2), true);
    irq_set_enabled(PIO0_IRQ_1 + (inIdx * 2), true);
    pio_set_irq0_source_enabled(MAPLE_IN_PIO, pis_interrupt0, true);
    pio_set_irq1_source_enabled(MAPLE_IN_PIO, pis_interrupt1, true);
    pio_set_irq0_source_enabled(MAPLE_IN_PIO, pis_interrupt2, true);
    pio_set_irq1_source_enabled(MAPLE_IN_PIO, pis_interrupt3, true);
}

MapleBus::MapleBus(uint32_t pinA, uint8_t senderAddr) :
    mPinA(pinA),
    mPinB(pinA + 1),
    mMaskA(1 << mPinA),
    mMaskB(1 << mPinB),
    mMaskAB(mMaskA | mMaskB),
    mSenderAddr(senderAddr),
    mSmOut(CPU_FREQ_KHZ, MAPLE_NS_PER_BIT, mPinA),
    mSmIn(mPinA),
    mDmaWriteChannel(dma_claim_unused_channel(true)),
    mDmaReadChannel(dma_claim_unused_channel(true)),
    mWriteBuffer(),
    mReadBuffer(),
    mLastRead(),
    mCurrentPhase(MapleBus::Phase::IDLE),
    mExpectingResponse(false),
    mProcKillTime(0xFFFFFFFFFFFFFFFFULL),
    mLastReceivedWordTimeUs(0),
    mLastReadTransferCount(0)
{
    mapleWriteIsr[mSmOut.mSmIdx] = this;
    mapleReadIsr[mSmIn.mSmIdx] = this;

    // This only needs to be called once but no issue calling it for each
    initIsrs();

    // Setup DMA to automaticlly put data on the FIFO
    dma_channel_config c = dma_channel_get_default_config(mDmaWriteChannel);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    // Bytes are instead manually swapped since CRC needs to be computed anyway
    // channel_config_set_bswap(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(mSmOut.mProgram.mPio, mSmOut.mSmIdx, true));
    dma_channel_configure(mDmaWriteChannel,
                            &c,
                            &mSmOut.mProgram.mPio->txf[mSmOut.mSmIdx],
                            mWriteBuffer,
                            sizeof(mWriteBuffer) / sizeof(mWriteBuffer[0]),
                            false);

    // Setup DMA to automaticlly read data from the FIFO
    c = dma_channel_get_default_config(mDmaReadChannel);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    // Bytes are instead manually swapped since CRC needs to be computed anyway
    // channel_config_set_bswap(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(mSmIn.mProgram.mPio, mSmIn.mSmIdx, false));
    dma_channel_configure(mDmaReadChannel,
                            &c,
                            mReadBuffer,
                            &mSmIn.mProgram.mPio->rxf[mSmIn.mSmIdx],
                            (sizeof(mReadBuffer) / sizeof(mReadBuffer[0])),
                            false);
}

inline void MapleBus::readIsr()
{
    // This ISR gets called twice within a read cycle:
    // - The first time tells us that start sequence was received
    // - The second time tells us that end sequence was received after completion

    if (mCurrentPhase == Phase::WAITING_FOR_READ_START)
    {
        mCurrentPhase = Phase::READ_IN_PROGRESS;
        mLastReceivedWordTimeUs = time_us_64();
    }
    else
    {
        mSmIn.stop();
        mCurrentPhase = Phase::READ_COMPLETE;
    }
}

inline void MapleBus::writeIsr()
{
    mSmOut.stop();
    if (mExpectingResponse)
    {
        mSmIn.start();
        mProcKillTime = time_us_64() + MAPLE_RESPONSE_TIMEOUT_US;
        mCurrentPhase = Phase::WAITING_FOR_READ_START;
    }
    else
    {
        mCurrentPhase = Phase::WRITE_COMPLETE;
    }
}

bool MapleBus::writeInit()
{
    const uint64_t targetTime = time_us_64() + MAPLE_OPEN_LINE_CHECK_TIME_US + 1;

    // Ensure no one is pulling low
    do
    {
        if ((gpio_get_all() & mMaskAB) != mMaskAB)
        {
            // Something is pulling low
            return false;
        }
    } while (time_us_64() < targetTime);

    mSmOut.start();

    return true;
}

bool MapleBus::write(const MaplePacket& packet,
                     bool expectResponse)
{
    bool rv = false;

    if (!isBusy() && packet.isValid())
    {
        // Make sure previous DMA instances are killed
        dma_channel_abort(mDmaWriteChannel);
        dma_channel_abort(mDmaReadChannel);

        // First 32 bits sent to the state machine is how many bits to output.
        uint8_t len = packet.payload.size();
        mWriteBuffer[0] = packet.getNumTotalBits();
        // The PIO state machine reads from "left to right" to achieve the right bit order, but the data
        // out needs to be little endian. Therefore, the data bytes needs to be swapped. Might as well
        // Compute the CRC while we're at it.
        uint8_t crc = 0;
        swapByteOrder(mWriteBuffer[1], packet.getFrameWord(mSenderAddr), crc);
        volatile uint32_t* pDest = &mWriteBuffer[2];
        for (std::vector<uint32_t>::const_iterator iter = packet.payload.begin();
             iter != packet.payload.end();
             ++iter, ++pDest)
        {
            swapByteOrder(*pDest, *iter, crc);
        }
        // Last byte left shifted out is the CRC
        mWriteBuffer[len + 2] = crc << 24;

        if (writeInit())
        {
            // Update flags before beginning to write
            mExpectingResponse = expectResponse;
            mCurrentPhase = Phase::WRITE_IN_PROGRESS;

            if (expectResponse)
            {
                // Start reading
                mLastReadTransferCount = sizeof(mReadBuffer) / sizeof(mReadBuffer[0]);
                dma_channel_transfer_to_buffer_now(
                    mDmaReadChannel, mReadBuffer, mLastReadTransferCount);
            }

            // Start writing
            dma_channel_transfer_from_buffer_now(mDmaWriteChannel, mWriteBuffer, len + 3);

            uint32_t totalWriteTimeNs = packet.getTxTimeNs();
            // Multiply by the extra percentage
            totalWriteTimeNs *= (1 + (MAPLE_WRITE_TIMEOUT_EXTRA_PERCENT / 100.0));
            // And then compute the time which the write process should complete
            mProcKillTime = time_us_64() + INT_DIVIDE_CEILING(totalWriteTimeNs, 1000);

            rv = true;
        }
    }

    return rv;
}

MapleBusInterface::Status MapleBus::processEvents(uint64_t currentTimeUs)
{
    Status status;
    // The state machine may still be running, so it is important to store the current phase and
    // fully process it at "this" moment in time i.e. the below must check against status.phase, not
    // mCurrentPhase.
    status.phase = mCurrentPhase;

    if (status.phase == Phase::READ_COMPLETE)
    {
        // Wait up to 1 ms for the RX FIFO to become empty (automatically drained by the read DMA)
        uint64_t timeoutTime = time_us_64() + 1000;
        while (!pio_sm_is_rx_fifo_empty(mSmIn.mProgram.mPio, mSmIn.mSmIdx)
               && time_us_64() < timeoutTime);

        // transfer_count is decrements down to 0, so compute the inverse to get number of words
        uint32_t dmaWordsRead = (sizeof(mReadBuffer) / sizeof(mReadBuffer[0]))
                                - dma_channel_hw_addr(mDmaReadChannel)->transfer_count;

        // Should have at least frame and CRC words
        if (dmaWordsRead > 1)
        {
            // The frame word always contains how many proceeding words there are [0,255]
            uint32_t len = mReadBuffer[0] >> 24;
            if (len == (dmaWordsRead - 2))
            {
                uint8_t crc = 0;
                // Bytes are loaded to the left, but the first byte is actually the LSB.
                // This means we need to byte swap (compute crc while we're at it)
                for (uint32_t i = 0; i < len + 1; ++i)
                {
                    swapByteOrder(mLastRead[i], mReadBuffer[i], crc);
                }
                // crc in the last word does not need to be byte swapped
                // Data is only valid if the CRC is correct and first word has something in it (cmd 0 is invalid)
                if (crc == mReadBuffer[len + 1])
                {
                    status.readBuffer = mLastRead;
                    status.readBufferLen = len + 1;
                }
                else
                {
                    // Read failed because CRC was invalid
                    status.phase = Phase::READ_FAILED;
                }
            }
            else
            {
                // Read failed because the packet length in package didn't match number of DMA words
                status.phase = Phase::READ_FAILED;
            }
        }
        else
        {
            // Read failed because nothing was sent through DMA
            status.phase = Phase::READ_FAILED;
        }

        // We processed the read, so the machine can go back to idle
        mCurrentPhase = Phase::IDLE;
    }
    else if (status.phase == Phase::WRITE_COMPLETE)
    {
        // Nothing to do here

        // We processed the write, so the machine can go back to idle
        mCurrentPhase = Phase::IDLE;
    }
    else if (status.phase == Phase::READ_IN_PROGRESS)
    {
        // Check for inter-word timeout
        uint32_t transferCount = dma_channel_hw_addr(mDmaReadChannel)->transfer_count;
        if (mLastReadTransferCount == transferCount)
        {
            if (currentTimeUs > mLastReceivedWordTimeUs
                && (currentTimeUs - mLastReceivedWordTimeUs) >= MAPLE_INTER_WORD_READ_TIMEOUT_US)
            {
                mSmIn.stop();
                // READ_FAILED is an impulse phase. Then the state machine goes back to IDLE.
                status.phase = Phase::READ_FAILED;
                mCurrentPhase = Phase::IDLE;
            }
        }
        else
        {
            mLastReadTransferCount = transferCount;
            mLastReceivedWordTimeUs = currentTimeUs;
        }

        // (mProcKillTime is ignored while actively reading)
    }
    else if (status.phase != Phase::IDLE && currentTimeUs >= mProcKillTime)
    {
        // The state machine is not idle, and it blew past a timeout - check what needs to be killed

        if (status.phase == Phase::WAITING_FOR_READ_START)
        {
            mSmIn.stop();
            // READ_FAILED is an impulse phase. Then the state machine goes back to IDLE.
            status.phase = Phase::READ_FAILED;
            mCurrentPhase = Phase::IDLE;
        }
        else // status.phase == Phase::WRITE_IN_PROGRESS - but also catches any other edge case
        {
            // Stopping both out and in just in case there was a race condition (state machine could
            // have *just* transitioned to read as we were processing this timeout)
            mSmOut.stop();
            mSmIn.stop();
            // WRITE_FAILED is an impulse phase. Then the state machine goes back to IDLE.
            status.phase = Phase::WRITE_FAILED;
            mCurrentPhase = Phase::IDLE;
        }
    }

    return status;
}