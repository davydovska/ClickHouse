#include <Compression/CompressionCodecGorilla.h>
#include <Compression/CompressionInfo.h>
#include <Compression/CompressionFactory.h>
#include <common/unaligned.h>
#include <Parsers/IAST_fwd.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/BitHelpers.h>

#include <string.h>
#include <algorithm>
#include <cstdlib>
#include <type_traits>

#include <bitset>

namespace DB
{

namespace ErrorCodes
{
extern const int CANNOT_COMPRESS;
extern const int CANNOT_DECOMPRESS;
extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
extern const int ILLEGAL_CODEC_PARAMETER;
}

namespace
{

constexpr inline UInt8 getBitLengthOfLength(UInt8 data_bytes_size)
{
    // 1-byte value is 8 bits, and we need 4 bits to represent 8 : 1000,
    // 2-byte         16 bits        =>    5
    // 4-byte         32 bits        =>    6
    // 8-byte         64 bits        =>    7
    const UInt8 bit_lengths[] = {0, 4, 5, 0, 6, 0, 0, 0, 7};
    assert(data_bytes_size >= 1 && data_bytes_size < sizeof(bit_lengths) && bit_lengths[data_bytes_size] != 0);

    return bit_lengths[data_bytes_size];
}


UInt32 getCompressedHeaderSize(UInt8 data_bytes_size)
{
    const UInt8 items_count_size = 4;

    return items_count_size + data_bytes_size;
}

UInt32 getCompressedDataSize(UInt8 data_bytes_size, UInt32 uncompressed_size)
{
    const UInt32 items_count = uncompressed_size / data_bytes_size;
    static const auto DATA_BIT_LENGTH = getBitLengthOfLength(data_bytes_size);
    // -1 since there must be at least 1 non-zero bit.
    static const auto LEADING_ZEROES_BIT_LENGTH = DATA_BIT_LENGTH - 1;

    // worst case (for 32-bit value):
    // 11 + 5 bits of leading zeroes bit-size + 5 bits of data bit-size + non-zero data bits.
    const UInt32 max_item_size_bits = 2 + LEADING_ZEROES_BIT_LENGTH + DATA_BIT_LENGTH + data_bytes_size * 8;

    // + 8 is to round up to next byte.
    return (items_count * max_item_size_bits + 8) / 8;
}

struct binary_value_info
{
    UInt8 leading_zero_bits;
    UInt8 data_bits;
    UInt8 trailing_zero_bits;
};

template <typename T>
binary_value_info getLeadingAndTrailingBits(const T & value)
{
    constexpr UInt8 bit_size = sizeof(T) * 8;

    const UInt8 lz = getLeadingZeroBits(value);
    const UInt8 tz = getTrailingZeroBits(value);
    const UInt8 data_size = value == 0 ? 0 : static_cast<UInt8>(bit_size - lz - tz);
    return binary_value_info{lz, data_size, tz};
}

template <typename T>
UInt32 compressDataForType(const char * source, UInt32 source_size, char * dest)
{
    static const auto DATA_BIT_LENGTH = getBitLengthOfLength(sizeof(T));
    // -1 since there must be at least 1 non-zero bit.
    static const auto LEADING_ZEROES_BIT_LENGTH = DATA_BIT_LENGTH - 1;

    if (source_size % sizeof(T) != 0)
        throw Exception("Cannot compress, data size " + toString(source_size) + " is not aligned to " + toString(sizeof(T)), ErrorCodes::CANNOT_COMPRESS);
    const char * source_end = source + source_size;

    const UInt32 items_count = source_size / sizeof(T);

    unalignedStore(dest, items_count);
    dest += sizeof(items_count);

    T prev_value{};
    // That would cause first XORed value to be written in-full.
    binary_value_info prev_xored_info{0, 0, 0};

    if (source < source_end)
    {
        prev_value = unalignedLoad<T>(source);
        unalignedStore(dest, prev_value);

        source += sizeof(prev_value);
        dest += sizeof(prev_value);
    }

    WriteBuffer buffer(dest, getCompressedDataSize(sizeof(T), source_size - sizeof(items_count) - sizeof(prev_value)));
    BitWriter writer(buffer);

    while (source < source_end)
    {
        const T curr_value = unalignedLoad<T>(source);
        source += sizeof(curr_value);

        const auto xored_data = curr_value ^ prev_value;
        const binary_value_info curr_xored_info = getLeadingAndTrailingBits(xored_data);

        if (xored_data == 0)
        {
            writer.writeBits(1, 0);
        }
        else if (prev_xored_info.data_bits != 0
                && prev_xored_info.leading_zero_bits <= curr_xored_info.leading_zero_bits
                && prev_xored_info.trailing_zero_bits <= curr_xored_info.trailing_zero_bits)
        {
            writer.writeBits(2, 0b10);
            writer.writeBits(prev_xored_info.data_bits, xored_data >> prev_xored_info.trailing_zero_bits);
        }
        else
        {
            writer.writeBits(2, 0b11);
            writer.writeBits(LEADING_ZEROES_BIT_LENGTH, curr_xored_info.leading_zero_bits);
            writer.writeBits(DATA_BIT_LENGTH, curr_xored_info.data_bits);
            writer.writeBits(curr_xored_info.data_bits, xored_data >> curr_xored_info.trailing_zero_bits);
            prev_xored_info = curr_xored_info;
        }

        prev_value = curr_value;
    }

    writer.flush();

    return sizeof(items_count) + sizeof(prev_value) + buffer.count();
}

template <typename T>
void decompressDataForType(const char * source, UInt32 source_size, char * dest)
{
    static const auto DATA_BIT_LENGTH = getBitLengthOfLength(sizeof(T));
    // -1 since there must be at least 1 non-zero bit.
    static const auto LEADING_ZEROES_BIT_LENGTH = DATA_BIT_LENGTH - 1;

    const char * source_end = source + source_size;

    const UInt32 items_count = unalignedLoad<UInt32>(source);
    source += sizeof(items_count);

    T prev_value{};

    if (source < source_end)
    {
        prev_value = unalignedLoad<T>(source);
        unalignedStore(dest, prev_value);

        source += sizeof(prev_value);
        dest += sizeof(prev_value);
    }

    ReadBufferFromMemory buffer(source, source_size - sizeof(items_count) - sizeof(prev_value));
    BitReader reader(buffer);

    binary_value_info prev_xored_info{0, 0, 0};

    // since data is tightly packed, up to 1 bit per value, and last byte is padded with zeroes,
    // we have to keep track of items to avoid reading more that there is.
    for (UInt32 items_read = 1; items_read < items_count && !reader.eof(); ++items_read)
    {
        T curr_value = prev_value;
        binary_value_info curr_xored_info = prev_xored_info;
        T xored_data{};

        if (reader.readBit() == 1)
        {
            if (reader.readBit() == 1)
            {
                // 0b11 prefix
                curr_xored_info.leading_zero_bits = reader.readBits(LEADING_ZEROES_BIT_LENGTH);
                curr_xored_info.data_bits = reader.readBits(DATA_BIT_LENGTH);
                curr_xored_info.trailing_zero_bits = sizeof(T) * 8 - curr_xored_info.leading_zero_bits - curr_xored_info.data_bits;
            }
            // else: 0b10 prefix - use prev_xored_info

            if (curr_xored_info.leading_zero_bits == 0
                && curr_xored_info.data_bits == 0
                && curr_xored_info.trailing_zero_bits == 0)
            {
                throw Exception("Cannot decompress gorilla-encoded data: corrupted input data.",
                        ErrorCodes::CANNOT_DECOMPRESS);
            }

            xored_data = reader.readBits(curr_xored_info.data_bits);
            xored_data <<= curr_xored_info.trailing_zero_bits;
            curr_value = prev_value ^ xored_data;
        }
        // else: 0b0 prefix - use prev_value

        unalignedStore(dest, curr_value);
        dest += sizeof(curr_value);

        prev_xored_info = curr_xored_info;
        prev_value = curr_value;
    }
}

UInt8 getDataBytesSize(DataTypePtr column_type)
{
    UInt8 delta_bytes_size = 1;
    if (column_type && column_type->haveMaximumSizeOfValue())
    {
        size_t max_size = column_type->getSizeOfValueInMemory();
        if (max_size == 1 || max_size == 2 || max_size == 4 || max_size == 8)
            delta_bytes_size = static_cast<UInt8>(max_size);
    }
    return delta_bytes_size;
}

}


CompressionCodecGorilla::CompressionCodecGorilla(UInt8 data_bytes_size_)
    : data_bytes_size(data_bytes_size_)
{
}

UInt8 CompressionCodecGorilla::getMethodByte() const
{
    return static_cast<UInt8>(CompressionMethodByte::Gorilla);
}

String CompressionCodecGorilla::getCodecDesc() const
{
    return "Gorilla";
}

UInt32 CompressionCodecGorilla::getMaxCompressedDataSize(UInt32 uncompressed_size) const
{
    const auto result = 2 // common header
            + data_bytes_size // max bytes skipped if source is not properly aligned.
            + getCompressedHeaderSize(data_bytes_size) // data-specific header
            + getCompressedDataSize(data_bytes_size, uncompressed_size);

    return result;
}

UInt32 CompressionCodecGorilla::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    UInt8 bytes_to_skip = source_size % data_bytes_size;
    dest[0] = data_bytes_size;
    dest[1] = bytes_to_skip;
    memcpy(&dest[2], source, bytes_to_skip);
    size_t start_pos = 2 + bytes_to_skip;
    UInt32 compressed_size = 0;
    switch (data_bytes_size)
    {
    case 1:
        compressed_size = compressDataForType<UInt8>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
        break;
    case 2:
        compressed_size = compressDataForType<UInt16>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
        break;
    case 4:
        compressed_size = compressDataForType<UInt32>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
        break;
    case 8:
        compressed_size = compressDataForType<UInt64>(&source[bytes_to_skip], source_size - bytes_to_skip, &dest[start_pos]);
        break;
    }

    return 1 + 1 + compressed_size;
}

void CompressionCodecGorilla::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 /* uncompressed_size */) const
{
    UInt8 bytes_size = source[0];
    UInt8 bytes_to_skip = source[1];

    memcpy(dest, &source[2], bytes_to_skip);
    UInt32 source_size_no_header = source_size - bytes_to_skip - 2;
    switch (bytes_size)
    {
    case 1:
        decompressDataForType<UInt8>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip]);
        break;
    case 2:
        decompressDataForType<UInt16>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip]);
        break;
    case 4:
        decompressDataForType<UInt32>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip]);
        break;
    case 8:
        decompressDataForType<UInt64>(&source[2 + bytes_to_skip], source_size_no_header, &dest[bytes_to_skip]);
        break;
    }
}

void CompressionCodecGorilla::useInfoAboutType(DataTypePtr data_type)
{
    data_bytes_size = getDataBytesSize(data_type);
}

void registerCodecGorilla(CompressionCodecFactory & factory)
{
    UInt8 method_code = UInt8(CompressionMethodByte::Gorilla);
    factory.registerCompressionCodecWithType("Gorilla", method_code, [&](const ASTPtr &, DataTypePtr column_type) -> CompressionCodecPtr
    {
        UInt8 delta_bytes_size = getDataBytesSize(column_type);
        return std::make_shared<CompressionCodecGorilla>(delta_bytes_size);
    });
}
}
