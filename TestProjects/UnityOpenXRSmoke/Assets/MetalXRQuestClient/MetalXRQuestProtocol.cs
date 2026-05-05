using System;
using System.Diagnostics;
using System.Text;

namespace MetalXR.QuestClient
{
    public static class MetalXRQuestProtocol
    {
        public const uint Magic = 0x4d585250;
        public const ushort VersionMajor = 0;
        public const ushort VersionMinor = 1;
        public const ushort HeaderSize = 40;
        public const ushort PacketHello = 1;
        public const ushort PacketHelloAck = 2;
        public const ushort PacketError = 4;

        private const uint RoleQuestClient = 2;
        private const uint CapabilityH264 = 0x00000001;
        private const uint CapabilityStereoSeparateEyes = 0x00000004;
        private const uint CapabilityLogStream = 0x00000080;
        private const int HelloPayloadSize = 96;
        private const int DeviceNameSize = 64;

        public readonly struct PacketHeader
        {
            public PacketHeader(uint magic, ushort headerSize, ushort type, ushort versionMajor, ushort versionMinor, uint payloadSize)
            {
                MagicValue = magic;
                HeaderSizeValue = headerSize;
                Type = type;
                VersionMajorValue = versionMajor;
                VersionMinorValue = versionMinor;
                PayloadSize = payloadSize;
            }

            public uint MagicValue { get; }
            public ushort HeaderSizeValue { get; }
            public ushort Type { get; }
            public ushort VersionMajorValue { get; }
            public ushort VersionMinorValue { get; }
            public uint PayloadSize { get; }
        }

        public static byte[] CreateHelloPacket(ulong sequence)
        {
            byte[] packet = new byte[HeaderSize + HelloPayloadSize];
            int offset = 0;
            WriteUInt32(packet, ref offset, Magic);
            WriteUInt16(packet, ref offset, HeaderSize);
            WriteUInt16(packet, ref offset, PacketHello);
            WriteUInt16(packet, ref offset, VersionMajor);
            WriteUInt16(packet, ref offset, VersionMinor);
            WriteUInt32(packet, ref offset, 0);
            WriteUInt64(packet, ref offset, sequence);
            WriteUInt64(packet, ref offset, NowNs());
            WriteUInt32(packet, ref offset, HelloPayloadSize);
            WriteUInt32(packet, ref offset, 0);

            WriteUInt32(packet, ref offset, RoleQuestClient);
            WriteUInt32(packet, ref offset, CapabilityH264 | CapabilityStereoSeparateEyes | CapabilityLogStream);
            WriteUInt32(packet, ref offset, 2064);
            WriteUInt32(packet, ref offset, 2208);
            WriteUInt32(packet, ref offset, 72);
            WriteUInt32(packet, ref offset, (uint)MetalXRQuestEndpoint.DefaultPort);
            WriteUInt32(packet, ref offset, 0);
            WriteUInt32(packet, ref offset, 0);
            WriteFixedAscii(packet, ref offset, "MetalXR Quest Unity Client", DeviceNameSize);

            return packet;
        }

        public static bool TryParseHeader(byte[] bytes, out PacketHeader header)
        {
            header = default;
            if (bytes == null || bytes.Length < HeaderSize)
            {
                return false;
            }

            int offset = 0;
            uint magic = ReadUInt32(bytes, ref offset);
            ushort headerSize = ReadUInt16(bytes, ref offset);
            ushort type = ReadUInt16(bytes, ref offset);
            ushort versionMajor = ReadUInt16(bytes, ref offset);
            ushort versionMinor = ReadUInt16(bytes, ref offset);
            offset += 4;
            offset += 8;
            offset += 8;
            uint payloadSize = ReadUInt32(bytes, ref offset);

            header = new PacketHeader(magic, headerSize, type, versionMajor, versionMinor, payloadSize);
            return magic == Magic && headerSize == HeaderSize;
        }

        private static ulong NowNs()
        {
            double timestamp = Stopwatch.GetTimestamp();
            double nanoseconds = timestamp * 1000000000.0 / Stopwatch.Frequency;
            return (ulong)nanoseconds;
        }

        private static void WriteFixedAscii(byte[] bytes, ref int offset, string value, int length)
        {
            byte[] encoded = Encoding.ASCII.GetBytes(value);
            int copyLength = encoded.Length < length - 1 ? encoded.Length : length - 1;
            Array.Copy(encoded, 0, bytes, offset, copyLength);
            offset += length;
        }

        private static void WriteUInt16(byte[] bytes, ref int offset, ushort value)
        {
            bytes[offset++] = (byte)(value & 0xff);
            bytes[offset++] = (byte)((value >> 8) & 0xff);
        }

        private static void WriteUInt32(byte[] bytes, ref int offset, uint value)
        {
            bytes[offset++] = (byte)(value & 0xff);
            bytes[offset++] = (byte)((value >> 8) & 0xff);
            bytes[offset++] = (byte)((value >> 16) & 0xff);
            bytes[offset++] = (byte)((value >> 24) & 0xff);
        }

        private static void WriteUInt64(byte[] bytes, ref int offset, ulong value)
        {
            WriteUInt32(bytes, ref offset, (uint)(value & 0xffffffff));
            WriteUInt32(bytes, ref offset, (uint)((value >> 32) & 0xffffffff));
        }

        private static ushort ReadUInt16(byte[] bytes, ref int offset)
        {
            ushort value = (ushort)(bytes[offset] | (bytes[offset + 1] << 8));
            offset += 2;
            return value;
        }

        private static uint ReadUInt32(byte[] bytes, ref int offset)
        {
            uint value =
                (uint)bytes[offset] |
                ((uint)bytes[offset + 1] << 8) |
                ((uint)bytes[offset + 2] << 16) |
                ((uint)bytes[offset + 3] << 24);
            offset += 4;
            return value;
        }
    }
}
