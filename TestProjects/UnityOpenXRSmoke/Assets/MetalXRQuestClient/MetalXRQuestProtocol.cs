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
        public const ushort PacketVideoFrame = 10;
        public const ushort PacketPoseSample = 20;
        public const ushort PacketControllerInput = 21;
        public const ushort PacketHapticCommand = 22;
        public const ushort PacketTimingSample = 30;
        public const uint CodecH264 = 1;
        public const uint EyeLeft = 0;
        public const uint EyeRight = 1;
        public const uint VideoFrameFlagKeyframe = 0x00000001;
        public const uint TimingFlagClockSync = 0x00000001;
        public const uint TimingFlagFrameDisplay = 0x00000002;
        public const uint TrackingOrientationValid = 0x00000001;
        public const uint TrackingPositionValid = 0x00000002;
        public const uint TrackingOrientationTracked = 0x00000004;
        public const uint TrackingPositionTracked = 0x00000008;
        public const uint ControllerHandLeft = 0;
        public const uint ControllerHandRight = 1;
        public const uint ControllerButtonPrimary = 0x00000001;
        public const uint ControllerButtonSecondary = 0x00000002;
        public const uint ControllerButtonMenu = 0x00000004;
        public const uint ControllerButtonThumbstick = 0x00000008;

        private const uint RoleQuestClient = 2;
        private const uint CapabilityH264 = 0x00000001;
        private const uint CapabilityStereoSeparateEyes = 0x00000004;
        private const uint CapabilityPoseInput = 0x00000010;
        private const uint CapabilityControllerInput = 0x00000020;
        private const uint CapabilityHaptics = 0x00000040;
        private const uint CapabilityLogStream = 0x00000080;
        private const int HelloPayloadSize = 96;
        private const int VideoFramePayloadSize = 56;
        private const int PoseSamplePayloadSize = 60;
        private const int ControllerInputPayloadSize = 104;
        private const int HapticCommandPayloadSize = 32;
        private const int TimingSamplePayloadSize = 88;
        private const int DeviceNameSize = 64;

        public readonly struct PacketHeader
        {
            public PacketHeader(
                uint magic,
                ushort headerSize,
                ushort type,
                ushort versionMajor,
                ushort versionMinor,
                uint flags,
                ulong sequence,
                ulong timestampNs,
                uint payloadSize)
            {
                MagicValue = magic;
                HeaderSizeValue = headerSize;
                Type = type;
                VersionMajorValue = versionMajor;
                VersionMinorValue = versionMinor;
                Flags = flags;
                Sequence = sequence;
                TimestampNs = timestampNs;
                PayloadSize = payloadSize;
            }

            public uint MagicValue { get; }
            public ushort HeaderSizeValue { get; }
            public ushort Type { get; }
            public ushort VersionMajorValue { get; }
            public ushort VersionMinorValue { get; }
            public uint Flags { get; }
            public ulong Sequence { get; }
            public ulong TimestampNs { get; }
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
            WriteUInt32(
                packet,
                ref offset,
                CapabilityH264 |
                CapabilityStereoSeparateEyes |
                CapabilityPoseInput |
                CapabilityControllerInput |
                CapabilityHaptics |
                CapabilityLogStream);
            WriteUInt32(packet, ref offset, 2064);
            WriteUInt32(packet, ref offset, 2208);
            WriteUInt32(packet, ref offset, 72);
            WriteUInt32(packet, ref offset, (uint)MetalXRQuestEndpoint.DefaultPort);
            WriteUInt32(packet, ref offset, 0);
            WriteUInt32(packet, ref offset, 0);
            WriteFixedAscii(packet, ref offset, "MetalXR Quest Unity Client", DeviceNameSize);

            return packet;
        }

        public static byte[] CreatePoseSamplePacket(
            ulong sequence,
            ulong sampleId,
            ulong timestampNs,
            ulong predictedDisplayTimeNs,
            float[] position,
            float[] orientation,
            uint trackingFlags)
        {
            byte[] packet = CreatePacketHeader(PacketPoseSample, sequence, PoseSamplePayloadSize);
            int offset = HeaderSize;
            WriteUInt64(packet, ref offset, sampleId);
            WriteUInt64(packet, ref offset, timestampNs);
            WriteUInt64(packet, ref offset, predictedDisplayTimeNs);
            WriteFloatArray(packet, ref offset, position, 3);
            WriteFloatArray(packet, ref offset, orientation, 4);
            WriteUInt32(packet, ref offset, trackingFlags);
            WriteUInt32(packet, ref offset, 0);
            return packet;
        }

        public static byte[] CreateControllerInputPacket(
            ulong sequence,
            ulong sampleId,
            ulong timestampNs,
            uint hand,
            uint buttons,
            float trigger,
            float grip,
            float[] thumbstick,
            uint trackingFlags,
            float[] aimPosition,
            float[] aimOrientation,
            float[] gripPosition,
            float[] gripOrientation)
        {
            byte[] packet = CreatePacketHeader(PacketControllerInput, sequence, ControllerInputPayloadSize);
            int offset = HeaderSize;
            WriteUInt64(packet, ref offset, sampleId);
            WriteUInt64(packet, ref offset, timestampNs);
            WriteUInt32(packet, ref offset, hand);
            WriteUInt32(packet, ref offset, buttons);
            WriteSingle(packet, ref offset, trigger);
            WriteSingle(packet, ref offset, grip);
            WriteFloatArray(packet, ref offset, thumbstick, 2);
            WriteUInt32(packet, ref offset, trackingFlags);
            WriteUInt32(packet, ref offset, 0);
            WriteFloatArray(packet, ref offset, aimPosition, 3);
            WriteFloatArray(packet, ref offset, aimOrientation, 4);
            WriteFloatArray(packet, ref offset, gripPosition, 3);
            WriteFloatArray(packet, ref offset, gripOrientation, 4);
            return packet;
        }

        public static byte[] CreateTimingSamplePacket(
            ulong sequence,
            MetalXRQuestTimingSample sample)
        {
            byte[] packet = CreatePacketHeader(PacketTimingSample, sequence, TimingSamplePayloadSize);
            int offset = HeaderSize;
            WriteUInt64(packet, ref offset, sample.FrameId);
            WriteUInt64(packet, ref offset, sample.HostCaptureTimeNs);
            WriteUInt64(packet, ref offset, sample.PredictedDisplayTimeNs);
            WriteUInt64(packet, ref offset, sample.EncodeStartTimeNs);
            WriteUInt64(packet, ref offset, sample.EncodeEndTimeNs);
            WriteUInt64(packet, ref offset, sample.ClientReceiveTimeNs);
            WriteUInt64(packet, ref offset, sample.ClientDisplayTimeNs);
            WriteUInt64(packet, ref offset, sample.ClientDecodeStartTimeNs);
            WriteUInt64(packet, ref offset, sample.ClientDecodeEndTimeNs);
            WriteUInt64(packet, ref offset, sample.ClientCompositorSubmitTimeNs);
            WriteUInt32(packet, ref offset, sample.QueueDepth);
            WriteUInt32(packet, ref offset, sample.Flags);
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
            uint flags = ReadUInt32(bytes, ref offset);
            ulong sequence = ReadUInt64(bytes, ref offset);
            ulong timestampNs = ReadUInt64(bytes, ref offset);
            uint payloadSize = ReadUInt32(bytes, ref offset);
            offset += 4;

            header = new PacketHeader(magic, headerSize, type, versionMajor, versionMinor, flags, sequence, timestampNs, payloadSize);
            return magic == Magic && headerSize == HeaderSize;
        }

        public static bool TryParseVideoFramePayload(
            PacketHeader header,
            byte[] payload,
            ulong receiveTimeNs,
            out MetalXRQuestEncodedVideoFrame frame)
        {
            frame = null;
            if (payload == null || payload.Length < VideoFramePayloadSize)
            {
                return false;
            }

            int offset = 0;
            ulong frameId = ReadUInt64(payload, ref offset);
            uint eye = ReadUInt32(payload, ref offset);
            uint codec = ReadUInt32(payload, ref offset);
            uint width = ReadUInt32(payload, ref offset);
            uint height = ReadUInt32(payload, ref offset);
            ulong timestampNs = ReadUInt64(payload, ref offset);
            ulong predictedDisplayTimeNs = ReadUInt64(payload, ref offset);
            ulong encoderLatencyUs = ReadUInt64(payload, ref offset);
            uint payloadBytes = ReadUInt32(payload, ref offset);
            uint flags = ReadUInt32(payload, ref offset);

            if (codec != CodecH264 || (eye != EyeLeft && eye != EyeRight) || width == 0 || height == 0)
            {
                return false;
            }

            if (payloadBytes == 0 || payloadBytes > payload.Length - VideoFramePayloadSize)
            {
                return false;
            }

            byte[] encodedBytes = new byte[payloadBytes];
            Array.Copy(payload, VideoFramePayloadSize, encodedBytes, 0, encodedBytes.Length);
            frame = new MetalXRQuestEncodedVideoFrame(
                frameId,
                eye,
                codec,
                (int)width,
                (int)height,
                timestampNs,
                predictedDisplayTimeNs,
                encoderLatencyUs,
                flags,
                header.Sequence,
                header.TimestampNs,
                receiveTimeNs,
                encodedBytes);
            return true;
        }

        public static bool TryParseHapticCommandPayload(byte[] payload, out MetalXRQuestHapticCommand command)
        {
            command = null;
            if (payload == null || payload.Length < HapticCommandPayloadSize)
            {
                return false;
            }

            int offset = 0;
            ulong commandId = ReadUInt64(payload, ref offset);
            ulong timestampNs = ReadUInt64(payload, ref offset);
            uint hand = ReadUInt32(payload, ref offset);
            float amplitude = ReadSingle(payload, ref offset);
            float frequencyHz = ReadSingle(payload, ref offset);
            uint durationUs = ReadUInt32(payload, ref offset);
            if (hand != ControllerHandLeft && hand != ControllerHandRight)
            {
                return false;
            }

            command = new MetalXRQuestHapticCommand(commandId, timestampNs, hand, amplitude, frequencyHz, durationUs);
            return true;
        }

        public static bool TryParseTimingSamplePayload(byte[] payload, out MetalXRQuestTimingSample sample)
        {
            sample = null;
            if (payload == null || payload.Length < TimingSamplePayloadSize)
            {
                return false;
            }

            int offset = 0;
            ulong frameId = ReadUInt64(payload, ref offset);
            ulong hostCaptureTimeNs = ReadUInt64(payload, ref offset);
            ulong predictedDisplayTimeNs = ReadUInt64(payload, ref offset);
            ulong encodeStartTimeNs = ReadUInt64(payload, ref offset);
            ulong encodeEndTimeNs = ReadUInt64(payload, ref offset);
            ulong clientReceiveTimeNs = ReadUInt64(payload, ref offset);
            ulong clientDisplayTimeNs = ReadUInt64(payload, ref offset);
            ulong clientDecodeStartTimeNs = ReadUInt64(payload, ref offset);
            ulong clientDecodeEndTimeNs = ReadUInt64(payload, ref offset);
            ulong clientCompositorSubmitTimeNs = ReadUInt64(payload, ref offset);
            uint queueDepth = ReadUInt32(payload, ref offset);
            uint flags = ReadUInt32(payload, ref offset);

            sample = new MetalXRQuestTimingSample(
                frameId,
                hostCaptureTimeNs,
                predictedDisplayTimeNs,
                encodeStartTimeNs,
                encodeEndTimeNs,
                clientReceiveTimeNs,
                clientDisplayTimeNs,
                clientDecodeStartTimeNs,
                clientDecodeEndTimeNs,
                clientCompositorSubmitTimeNs,
                queueDepth,
                flags);
            return true;
        }

        private static byte[] CreatePacketHeader(ushort packetType, ulong sequence, int payloadSize)
        {
            byte[] packet = new byte[HeaderSize + payloadSize];
            int offset = 0;
            WriteUInt32(packet, ref offset, Magic);
            WriteUInt16(packet, ref offset, HeaderSize);
            WriteUInt16(packet, ref offset, packetType);
            WriteUInt16(packet, ref offset, VersionMajor);
            WriteUInt16(packet, ref offset, VersionMinor);
            WriteUInt32(packet, ref offset, 0);
            WriteUInt64(packet, ref offset, sequence);
            WriteUInt64(packet, ref offset, NowNs());
            WriteUInt32(packet, ref offset, (uint)payloadSize);
            WriteUInt32(packet, ref offset, 0);
            return packet;
        }

        public static ulong NowNs()
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

        private static void WriteSingle(byte[] bytes, ref int offset, float value)
        {
            byte[] encoded = BitConverter.GetBytes(value);
            Array.Copy(encoded, 0, bytes, offset, 4);
            offset += 4;
        }

        private static void WriteFloatArray(byte[] bytes, ref int offset, float[] values, int count)
        {
            for (int i = 0; i < count; i++)
            {
                float value = values != null && i < values.Length ? values[i] : 0.0f;
                WriteSingle(bytes, ref offset, value);
            }
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

        private static ulong ReadUInt64(byte[] bytes, ref int offset)
        {
            ulong low = ReadUInt32(bytes, ref offset);
            ulong high = ReadUInt32(bytes, ref offset);
            return low | (high << 32);
        }

        private static float ReadSingle(byte[] bytes, ref int offset)
        {
            float value = BitConverter.ToSingle(bytes, offset);
            offset += 4;
            return value;
        }
    }

    public sealed class MetalXRQuestEncodedVideoFrame
    {
        public MetalXRQuestEncodedVideoFrame(
            ulong frameId,
            uint eye,
            uint codec,
            int width,
            int height,
            ulong timestampNs,
            ulong predictedDisplayTimeNs,
            ulong encoderLatencyUs,
            uint flags,
            ulong sequence,
            ulong packetTimestampNs,
            ulong receiveTimeNs,
            byte[] encodedBytes)
        {
            FrameId = frameId;
            Eye = eye;
            Codec = codec;
            Width = width;
            Height = height;
            TimestampNs = timestampNs;
            PredictedDisplayTimeNs = predictedDisplayTimeNs;
            EncoderLatencyUs = encoderLatencyUs;
            Flags = flags;
            Sequence = sequence;
            PacketTimestampNs = packetTimestampNs;
            ReceiveTimeNs = receiveTimeNs;
            EncodedBytes = encodedBytes;
        }

        public ulong FrameId { get; }
        public uint Eye { get; }
        public uint Codec { get; }
        public int Width { get; }
        public int Height { get; }
        public ulong TimestampNs { get; }
        public ulong PredictedDisplayTimeNs { get; }
        public ulong EncoderLatencyUs { get; }
        public uint Flags { get; }
        public ulong Sequence { get; }
        public ulong PacketTimestampNs { get; }
        public ulong ReceiveTimeNs { get; }
        public byte[] EncodedBytes { get; }
        public bool IsLeftEye { get { return Eye == MetalXRQuestProtocol.EyeLeft; } }
        public bool IsKeyframe { get { return (Flags & MetalXRQuestProtocol.VideoFrameFlagKeyframe) != 0; } }
    }

    public sealed class MetalXRQuestHapticCommand
    {
        public MetalXRQuestHapticCommand(
            ulong commandId,
            ulong timestampNs,
            uint hand,
            float amplitude,
            float frequencyHz,
            uint durationUs)
        {
            CommandId = commandId;
            TimestampNs = timestampNs;
            Hand = hand;
            Amplitude = amplitude;
            FrequencyHz = frequencyHz;
            DurationUs = durationUs;
        }

        public ulong CommandId { get; }
        public ulong TimestampNs { get; }
        public uint Hand { get; }
        public float Amplitude { get; }
        public float FrequencyHz { get; }
        public uint DurationUs { get; }
        public bool IsLeftHand { get { return Hand == MetalXRQuestProtocol.ControllerHandLeft; } }
    }

    public sealed class MetalXRQuestTimingSample
    {
        public MetalXRQuestTimingSample(
            ulong frameId,
            ulong hostCaptureTimeNs,
            ulong predictedDisplayTimeNs,
            ulong encodeStartTimeNs,
            ulong encodeEndTimeNs,
            ulong clientReceiveTimeNs,
            ulong clientDisplayTimeNs,
            ulong clientDecodeStartTimeNs,
            ulong clientDecodeEndTimeNs,
            ulong clientCompositorSubmitTimeNs,
            uint queueDepth,
            uint flags)
        {
            FrameId = frameId;
            HostCaptureTimeNs = hostCaptureTimeNs;
            PredictedDisplayTimeNs = predictedDisplayTimeNs;
            EncodeStartTimeNs = encodeStartTimeNs;
            EncodeEndTimeNs = encodeEndTimeNs;
            ClientReceiveTimeNs = clientReceiveTimeNs;
            ClientDisplayTimeNs = clientDisplayTimeNs;
            ClientDecodeStartTimeNs = clientDecodeStartTimeNs;
            ClientDecodeEndTimeNs = clientDecodeEndTimeNs;
            ClientCompositorSubmitTimeNs = clientCompositorSubmitTimeNs;
            QueueDepth = queueDepth;
            Flags = flags;
        }

        public ulong FrameId { get; }
        public ulong HostCaptureTimeNs { get; }
        public ulong PredictedDisplayTimeNs { get; }
        public ulong EncodeStartTimeNs { get; }
        public ulong EncodeEndTimeNs { get; }
        public ulong ClientReceiveTimeNs { get; }
        public ulong ClientDisplayTimeNs { get; }
        public ulong ClientDecodeStartTimeNs { get; }
        public ulong ClientDecodeEndTimeNs { get; }
        public ulong ClientCompositorSubmitTimeNs { get; }
        public uint QueueDepth { get; }
        public uint Flags { get; }
        public bool IsClockSync { get { return (Flags & MetalXRQuestProtocol.TimingFlagClockSync) != 0; } }
    }
}
