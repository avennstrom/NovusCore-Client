#pragma once

enum Opcode
{
    CMSG_LOGON_CHALLENGE,
    SMSG_LOGON_CHALLENGE,
    CMSG_LOGON_RESPONSE,
    SMSG_LOGON_RESPONSE,
    IMSG_HANDSHAKE,
    IMSG_HANDSHAKE_RESPONSE,
    OPCODE_MAX_COUNT
};