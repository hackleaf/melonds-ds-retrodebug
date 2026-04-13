# PatchMelonDSRtc.cmake — fix is_read classification in RTC.cpp's ByteIn hook.
#
# The core's RTC::ByteIn at InputPos==0 dispatches to OnRegAccess with
# `is_read = (val & 1)`. But `val` is the RAW wire byte from a bit-banged SPI
# transfer, which is the bit-reversal of the logical opcode (e.g., opcode
# 0x6F arrives here as 0xF6). Bit 0 of the raw byte is therefore bit 7 of
# the logical opcode, which is the bank bit, not the read/write bit. So
# read opcodes (0x6B/0x6D/0x6F/0x71) all get misclassified as writes, their
# Output[0] is never populated from the hook, and multi-byte reads return 0.
#
# Reverse the byte first, then test bit 0 of the logical opcode — which is
# the NDS RTC convention for read (1) vs write (0).
#
# Called from FetchDependencies.cmake after melonDS source is fetched.
# MELONDS_SRC must be set to the fetched melonDS source directory.

file(READ "${MELONDS_SRC}/src/RTC.cpp" RTC_CPP)

set(_old [[
        // Retrodebug: intercept raw command byte before decoding.
        // RTCom and other hacks send commands MSB-first (via rtcTransferReversed),
        // which arrive here as raw bytes like 0x6C-0x6F. These would collide with
        // standard NDS registers after bit-reversal. Let the hook handle them
        // before the reversal step.
        if (OnRegAccess)
        {
            bool is_read = (val & 1);
            u8 v = 0;
            if (OnRegAccess(OnRegAccessUserData, val, is_read, &v))
            {
                CurCmd = val;
                if (is_read)
                {
                    Output[0] = v;
                    OutputPos = 0;
                    OutputBit = 0;
                }
                return;
            }
        }]])

set(_new [[
        // Retrodebug: intercept raw command byte before decoding.
        // RTCom and other hacks send commands MSB-first (via rtcTransferReversed);
        // melonDS's SPI accumulator stores LSB-first, so the byte accumulates in
        // bit-reversed form. To classify the access as read vs write, reverse
        // the byte and test bit 0 of the logical opcode (NDS RTC convention:
        // bit 0 of the opcode = read/write flag).
        if (OnRegAccess)
        {
            u8 revv = val;
            revv = (revv >> 4) | (revv << 4);
            revv = ((revv & 0xCC) >> 2) | ((revv & 0x33) << 2);
            revv = ((revv & 0xAA) >> 1) | ((revv & 0x55) << 1);
            bool is_read = (revv & 1);
            u8 v = 0;
            if (OnRegAccess(OnRegAccessUserData, val, is_read, &v))
            {
                CurCmd = val;
                if (is_read)
                {
                    Output[0] = v;
                    OutputPos = 0;
                    OutputBit = 0;
                }
                return;
            }
        }]])

string(REPLACE "${_old}" "${_new}" RTC_CPP_NEW "${RTC_CPP}")

if (RTC_CPP_NEW STREQUAL RTC_CPP)
    message(WARNING "PatchMelonDSRtc: pattern not found in ${MELONDS_SRC}/src/RTC.cpp — patch skipped")
else()
    file(WRITE "${MELONDS_SRC}/src/RTC.cpp" "${RTC_CPP_NEW}")
    message(STATUS "  Patched RTC.cpp")
endif()
