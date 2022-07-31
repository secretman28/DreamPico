#ifndef __MAPLE_PACKET_H__
#define __MAPLE_PACKET_H__

#include <stdint.h>
#include <vector>
#include "dreamcast_constants.h"

struct MaplePacket
{
    //! Byte position of the command in the frame word
    static const uint32_t COMMAND_POSITION = 24;
    //! Byte position of the recipient address in the frame word
    static const uint32_t RECIPIENT_ADDR_POSITION = 16;
    //! Byte position of the sender address in the frame word
    static const uint32_t SENDER_ADDR_POSITION = 8;
    //! Byte position of the payload length in the frame word
    static const uint32_t LEN_POSITION = 0;

    //! Frame word
    const uint32_t frameWord;
    //! Following payload words
    const std::vector<uint32_t> payload;

    //! Constructor 1 (frame word is built without sender address)
    //! @param[in] command  The command byte - should be a value in Command enumeration
    //! @param[in] recipientAddr  The address of the device receiving this command
    //! @param[in] payload  The payload words to send
    //! @param[in] len  Number of words in payload
    MaplePacket(uint8_t command,
                uint8_t recipientAddr,
                const uint32_t* payload,
                uint8_t len):
        frameWord((len << LEN_POSITION)
                  | (recipientAddr << RECIPIENT_ADDR_POSITION)
                  | (command << COMMAND_POSITION)),
        payload(payload, payload + len)
    {}

    //! Constructor 2 (frame word is built without sender address)
    //! @param[in] command  The command byte - should be a value in Command enumeration
    //! @param[in] recipientAddr  The address of the device receiving this command
    //! @param[in] payload  The single payload word to send
    MaplePacket(uint8_t command,
                uint8_t recipientAddr,
                uint32_t payload):
        frameWord((1 << LEN_POSITION)
                  | (recipientAddr << RECIPIENT_ADDR_POSITION)
                  | (command << COMMAND_POSITION)),
        payload(&payload, &payload + 1)
    {}

    //! Constructor 3
    //! @param[in] frameWord  The first word to put out on the bus
    //! @param[in] payload  The payload words to send
    //! @param[in] len  Number of words in payload
    MaplePacket(uint32_t frameWord,
                const uint32_t* payload,
                uint8_t len):
        frameWord(frameWord),
        payload(payload, payload + len)
    {}

    //! Constructor 4
    //! @param[in] words  All words to send
    //! @param[in] len  Number of words in words (must be at least 1 for frame word to be valid)
    MaplePacket(const uint32_t* words,
                uint8_t len):
        frameWord(len > 0 ? words[0] : (COMMAND_INVALID << COMMAND_POSITION)),
        payload(&words[1], &words[1] + (len > 1 ? (len - 1) : 0))
    {}

    //! == operator for this class
    bool operator==(const MaplePacket& rhs) const
    {
        return frameWord == rhs.frameWord && payload == rhs.payload;
    }

    //! @returns true iff frame word is valid
    bool isValid() const
    {
        return (getFrameCommand() != COMMAND_INVALID && getFramePacketLength() == payload.size());
    }

    //! @returns the packet length specified in the frame word
    uint8_t getFramePacketLength() const
    {
        return ((frameWord >> LEN_POSITION) & 0xFF);
    }

    //! @returns the sender address specified in the frame word
    uint8_t getFrameSenderAddr() const
    {
        return ((frameWord >> SENDER_ADDR_POSITION) & 0xFF);
    }

    //! @returns the recipient address specified in the frame word
    uint8_t getFrameRecipientAddr() const
    {
        return ((frameWord >> RECIPIENT_ADDR_POSITION) & 0xFF);
    }

    //! @returns the command specified in the frame word
    uint8_t getFrameCommand() const
    {
        return ((frameWord >> COMMAND_POSITION) & 0xFF);
    }

    //! @returns the frame word with an overloaded sender address
    uint32_t getFrameWord(uint8_t overloadedSenderAddress) const
    {
        return ((frameWord & ~(0xFF << SENDER_ADDR_POSITION))
                | (overloadedSenderAddress << SENDER_ADDR_POSITION));
    }
};

#endif // __MAPLE_PACKET_H__