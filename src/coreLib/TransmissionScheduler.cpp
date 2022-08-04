#include "TransmissionScheduler.hpp"
#include "utils.h"

TransmittionScheduler::TransmittionScheduler(): mNextId(0), mSchedule() {}

TransmittionScheduler::~TransmittionScheduler() {}

uint32_t TransmittionScheduler::add(std::shared_ptr<Transmission> tx)
{
    // Keep iterating until correct position is found
    // For this to be the right position, it either needs to start and end before the next
    // packet or starts before the next packet and is higher or same priority.
    std::list<std::shared_ptr<Transmission>>::const_iterator iter = mSchedule.cbegin();
    while(iter != mSchedule.cend())
    {
        // If this transmission starts before this element...
        if (tx->nextTxTimeUs < (*iter)->nextTxTimeUs)
        {
            // if this transmission also ends before this element starts
            // or is of higher or same priority...
            if (tx->getNextCompletionTime() < (*iter)->nextTxTimeUs
                || tx->priority <= (*iter)->priority)
            {
                // Stop here
                break;
            }
        }
        // If this transmission starts before this element completes
        // and is higher priority...
        else if (tx->nextTxTimeUs < (*iter)->getNextCompletionTime()
                    && tx->priority < (*iter)->priority)
        {
            // Stop here
            break;
        }
        ++iter;
    }

    mSchedule.insert(iter, tx);
    return tx->transmissionId;
}

uint32_t TransmittionScheduler::add(uint8_t priority,
                                    uint64_t txTime,
                                    MaplePacket& packet,
                                    bool expectResponse,
                                    uint32_t expectedResponseNumPayloadWords,
                                    uint32_t autoRepeatUs,
                                    uint32_t readTimeoutUs)
{
    uint32_t pktDurationUs =
        INT_DIVIDE_CEILING(
            packet.getTxTimeNs()
                + RX_DELAY_NS
                + MaplePacket::getTxTimeNs(expectedResponseNumPayloadWords, RX_NS_PER_BIT),
            1000);
    std::shared_ptr<Transmission> tx =
        std::make_shared<Transmission>(mNextId++,
                                       priority,
                                       expectResponse,
                                       readTimeoutUs,
                                       autoRepeatUs,
                                       pktDurationUs,
                                       txTime,
                                       std::make_shared<MaplePacket>(std::move(packet)));
    return add(tx);
}

std::shared_ptr<const TransmittionScheduler::Transmission> TransmittionScheduler::popNext(uint64_t time)
{
    std::shared_ptr<Transmission> item = nullptr;
    if (!mSchedule.empty())
    {
        if (time >= (*mSchedule.begin())->nextTxTimeUs)
        {
            item = (*mSchedule.begin());
            mSchedule.erase(mSchedule.begin());
            if (item->autoRepeatUs > 0)
            {
                // Determine how many intervals to add in cast this auto reload packet has overflowed
                uint32_t n = INT_DIVIDE_CEILING(time - item->nextTxTimeUs, item->autoRepeatUs);
                if (n == 0)
                {
                    n = 1;
                }
                // Reinsert it back into the schedule with a new time
                item->nextTxTimeUs += (item->autoRepeatUs * n);
                add(item);
            }
        }
        else
        {
            // See if a item down the schedule could start now and end before mSchedule.begin().
            // It's easier to check here than to rearrange schedule every time a higher priority
            // transmission preempts a lower priority one.
            std::list<std::shared_ptr<Transmission>>::iterator iter = mSchedule.begin();
            ++iter;
            while(iter != mSchedule.end())
            {
                if (time >= (*iter)->nextTxTimeUs
                    && (*iter)->getNextCompletionTime() < (*mSchedule.begin())->nextTxTimeUs)
                {
                    item = (*iter);
                    mSchedule.erase(iter);
                    break;
                }
                ++iter;
            }
        }
    }
    return item;
}

uint32_t TransmittionScheduler::cancelById(uint32_t transmissionId)
{
    uint32_t n = 0;
    std::list<std::shared_ptr<Transmission>>::iterator iter = mSchedule.begin();
    while (iter != mSchedule.end())
    {
        if ((*iter)->transmissionId == transmissionId)
        {
            iter = mSchedule.erase(iter);
            ++n;
        }
        else
        {
            ++iter;
        }
    }
    return n;
}

uint32_t TransmittionScheduler::cancelByRecipient(uint8_t recipientAddr)
{
    uint32_t n = 0;
    std::list<std::shared_ptr<Transmission>>::iterator iter = mSchedule.begin();
    while (iter != mSchedule.end())
    {
        if ((*iter)->packet->getFrameRecipientAddr() == recipientAddr)
        {
            iter = mSchedule.erase(iter);
            ++n;
        }
        else
        {
            ++iter;
        }
    }
    return n;
}

uint32_t TransmittionScheduler::cancelAll()
{
    uint32_t n = mSchedule.size();
    mSchedule.clear();
    return n;
}