// Copyright © 2026 CCP ehf.

/**
 * CCP Network protocol related definitions shared between CarbonIO and StacklessIO
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

// bits defined in the 4-byte packet header
enum EHeaderBits
{
	ceHeaderBitsMask = 0xF0000000,
	ceHeaderSizeMask = 0x0FFFFFFF,

	ceHeaderExpectPayloadOffset = 1 << 28, // if defined in a header, tells where the OOB data ends
	ceHeaderBitZlibCompressed = 1 << 29, // was compressed with zlib
	ceHeaderBitSnappyCompressed = 1 << 30, // was compressed with snappy, no longer supported.
	ceHeaderExtraHeaderBitsFollow = 1 << 31, // in case it needs to be extended, reserve this bit [unimplemented]
};

#endif //PROTOCOL_H
