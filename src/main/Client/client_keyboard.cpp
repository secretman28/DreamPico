#ifndef ENABLE_UNIT_TEST

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/platform.h"

#include "bsp/board.h"
#include "tusb.h"

#include "configuration.h"

#include "CriticalSectionMutex.hpp"
#include "Mutex.hpp"
#include "Clock.hpp"
#include "NonVolatilePicoSystemMemory.hpp"

#include "hal/System/LockGuard.hpp"
#include "hal/MapleBus/MapleBusInterface.hpp"

#include "DreamcastMainPeripheral.hpp"
#include "DreamcastKeyboard.hpp"
#include "DreamcastStorage.hpp"

#include <memory>
#include <algorithm>

// Only a single LED function for client
#define CLIENT_LED_PIN ((USB_LED_PIN >= 0) ? USB_LED_PIN : SIMPLE_USB_LED_PIN)

void led_task();

std::shared_ptr<NonVolatilePicoSystemMemory> mem =
    std::make_shared<NonVolatilePicoSystemMemory>(
        PICO_FLASH_SIZE_BYTES - client::DreamcastStorage::MEMORY_SIZE_BYTES,
        client::DreamcastStorage::MEMORY_SIZE_BYTES);

// Second Core Process
void core1()
{
    set_sys_clock_khz(CPU_FREQ_KHZ, true);

    while (true)
    {
        mem->process();
    }
}

// First Core Process
void core0()
{
    set_sys_clock_khz(CPU_FREQ_KHZ, true);

    board_init();
    tusb_init();

#if SHOW_DEBUG_MESSAGES
    stdio_uart_init();
#endif

    multicore_launch_core1(core1);

    std::shared_ptr<MapleBusInterface> bus = create_maple_bus(P1_BUS_START_PIN);
    client::DreamcastMainPeripheral mainPeripheral(
        0x20,
        0x01,
        0x00,
        "Keyboard",
        "Produced By or Under License From SEGA ENTERPRISES,LTD.",
        "Version 1.010,1999/04/27,315-6211-AM   ,Key Scan Module : The 2nd Edition. 04/25",
        30.0,
        40.0);
    std::shared_ptr<client::DreamcastKeyboard> keyboard =
        std::make_shared<client::DreamcastKeyboard>(
            client::DreamcastKeyboard::Language::America,
            client::DreamcastKeyboard::Type::Key104,
            true,
            true,
            true,
            false,
            false,
            false,
            false);
    mainPeripheral.addFunction(keyboard);
    std::shared_ptr<client::DreamcastPeripheral> subPeripheral1 =
        std::make_shared<client::DreamcastPeripheral>(
            0x01,
            0xFF,
            0x00,
            "Memory",
            "Produced By or Under License From SEGA ENTERPRISES,LTD.",
            "Version 1.005,1999/04/15,315-6208-03,SEGA Visual Memory System BIOS Produced by IOS Produced",
            12.4,
            13.0);
    std::shared_ptr<client::DreamcastStorage> dremcastStorage =
        std::make_shared<client::DreamcastStorage>(mem, 0);
    subPeripheral1->addFunction(dremcastStorage);
    mainPeripheral.addSubPeripheral(subPeripheral1);

    uint8_t lastSender = 0;
    MaplePacket packetOut;
    packetOut.reservePayload(256);
    MaplePacket lastPacketOut;
    lastPacketOut.reservePayload(256);
    bool packetSent = false;
    MaplePacket packetIn;
    packetIn.reservePayload(256);
    while(true)
    {
        if (bus->startRead(1000000))
        {
            MapleBusInterface::Status status;
            do
            {
                status = bus->processEvents(time_us_64());
            } while (status.phase == MapleBusInterface::Phase::WAITING_FOR_READ_START
                    || status.phase == MapleBusInterface::Phase::READ_IN_PROGRESS);

            if (status.phase == MapleBusInterface::Phase::READ_COMPLETE)
            {
                bool writeIt = false;

                packetIn.set(status.readBuffer, status.readBufferLen);
                lastSender = packetIn.frame.senderAddr;

                if (packetIn.frame.command == COMMAND_RESPONSE_REQUEST_RESEND)
                {
                    if (packetSent)
                    {
                        // Write the previous packet
                        packetOut = lastPacketOut;
                        writeIt = true;
                    }
                }
                else
                {
                    writeIt = mainPeripheral.dispensePacket(packetIn, packetOut);
                }

                if (writeIt)
                {
                    packetSent = true;
                    lastPacketOut = packetOut;
                    if (bus->write(packetOut, false))
                    {
                        do
                        {
                            status = bus->processEvents(time_us_64());
                        } while (status.phase == MapleBusInterface::Phase::WRITE_IN_PROGRESS);
                    }
                }
                // else: write nothing, and the host will eventually stall out
            }
            else if(status.phase == MapleBusInterface::Phase::READ_FAILED
                    && status.failureReason == MapleBusInterface::FailureReason::CRC_INVALID
                    && mainPeripheral.isConnected())
            {
                packetOut.reset();
                packetOut.frame.command = COMMAND_RESPONSE_REQUEST_RESEND;
                packetOut.frame.recipientAddr = lastSender;
                packetOut.frame.senderAddr = mainPeripheral.getAddress();
                packetOut.updateFrameLength();
                if (bus->write(packetOut, false))
                {
                    do
                    {
                        status = bus->processEvents(time_us_64());
                    } while (status.phase == MapleBusInterface::Phase::WRITE_IN_PROGRESS);
                }
            }
            else
            {
                mainPeripheral.reset();
            }
        }

        led_task();
    }
}

int main()
{
    gpio_init(CLIENT_LED_PIN);
    gpio_set_dir_out_masked(1<<CLIENT_LED_PIN);
    core0();
    return 0;
}

void led_task()
{
    static bool ledOn = false;
    static uint64_t startUs = 0;
    static const uint32_t BLINK_TIME_US = 250000;
    static const uint32_t ACTIVITY_DELAY_US = 500000;

    // To correct for the non-atomic read in getLastActivityTime(), only update activityStopTime
    // if a new time is greater
    uint64_t currentTime = time_us_64();
    static uint64_t activityStopTime = 0;
    uint64_t lastActivityTime = mem->getLastActivityTime();
    if (lastActivityTime != 0)
    {
        lastActivityTime += ACTIVITY_DELAY_US;
        if (lastActivityTime > activityStopTime)
        {
            activityStopTime = lastActivityTime;
        }
    }

    if (activityStopTime > currentTime)
    {
        uint64_t t = currentTime - startUs;
        if (t >= BLINK_TIME_US)
        {
            startUs += BLINK_TIME_US;
            ledOn = !ledOn;
        }
    }
    else
    {
        startUs = currentTime;
        ledOn = true;
    }

    gpio_put(CLIENT_LED_PIN, ledOn);
}

#endif
