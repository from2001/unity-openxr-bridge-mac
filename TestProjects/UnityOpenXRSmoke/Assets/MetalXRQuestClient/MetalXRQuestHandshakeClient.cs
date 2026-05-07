using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net.Sockets;
using System.Threading;

namespace MetalXR.QuestClient
{
    public sealed class MetalXRQuestHandshakeClient : IDisposable
    {
        private const int ConnectTimeoutMs = 1000;
        private const int ReadTimeoutMs = 5000;
        private const int WriteTimeoutMs = 5000;
        private const int RetryDelayMs = 2500;
        private const int MaxPayloadBytes = 8 * 1024 * 1024;
        private const int MaxQueuedFrameSets = 4;
        private const int MaxPartialFrameSets = 4;
        private const int MaxQueuedOutgoingPackets = 32;
        private const int MaxQueuedHaptics = 8;
        private readonly MetalXRQuestEndpoint _endpoint;
        private readonly MetalXRQuestDeviceProfile _deviceProfile;
        private readonly uint _capabilities;
        private readonly ConcurrentQueue<string> _logs = new ConcurrentQueue<string>();
        private readonly ConcurrentQueue<MetalXRQuestEncodedStereoFrameSet> _frameSets = new ConcurrentQueue<MetalXRQuestEncodedStereoFrameSet>();
        private readonly ConcurrentQueue<byte[]> _outgoingPackets = new ConcurrentQueue<byte[]>();
        private readonly ConcurrentQueue<MetalXRQuestHapticCommand> _hapticCommands = new ConcurrentQueue<MetalXRQuestHapticCommand>();
        private readonly Dictionary<ulong, PartialStereoFrameSet> _partialFrameSets = new Dictionary<ulong, PartialStereoFrameSet>();
        private readonly object _lifecycleLock = new object();
        private readonly object _sequenceLock = new object();
        private Thread _thread;
        private bool _stopRequested;
        private ulong _sequence = 1;
        private int _failureCount;
        private ulong _receivedFrameSets;

        public MetalXRQuestHandshakeClient(MetalXRQuestEndpoint endpoint, MetalXRQuestDeviceProfile deviceProfile)
            : this(endpoint, deviceProfile, MetalXRQuestProtocol.DefaultClientCapabilities)
        {
        }

        public MetalXRQuestHandshakeClient(
            MetalXRQuestEndpoint endpoint,
            MetalXRQuestDeviceProfile deviceProfile,
            uint capabilities)
        {
            _endpoint = endpoint;
            _deviceProfile = deviceProfile ?? MetalXRQuestDeviceProfile.Default;
            _capabilities = capabilities == 0 ? MetalXRQuestProtocol.DefaultClientCapabilities : capabilities;
        }

        public int QueuedFrameCount { get { return _frameSets.Count * 2; } }

        public void Start()
        {
            lock (_lifecycleLock)
            {
                if (_thread != null)
                {
                    return;
                }

                _thread = new Thread(Run);
                _thread.Name = "MetalXR Quest Handshake";
                _thread.IsBackground = true;
                _thread.Start();
            }
        }

        public void DrainLogs(Action<string> sink)
        {
            if (sink == null)
            {
                return;
            }

            string message;
            while (_logs.TryDequeue(out message))
            {
                sink(message);
            }
        }

        public int DrainFrameSets(Action<MetalXRQuestEncodedStereoFrameSet> sink, int maxFrameSets)
        {
            if (sink == null || maxFrameSets <= 0)
            {
                return 0;
            }

            int drained = 0;
            MetalXRQuestEncodedStereoFrameSet frameSet;
            while (drained < maxFrameSets && _frameSets.TryDequeue(out frameSet))
            {
                sink(frameSet);
                drained++;
            }

            return drained;
        }

        public int DrainHapticCommands(Action<MetalXRQuestHapticCommand> sink, int maxCommands)
        {
            if (sink == null || maxCommands <= 0)
            {
                return 0;
            }

            int drained = 0;
            MetalXRQuestHapticCommand command;
            while (drained < maxCommands && _hapticCommands.TryDequeue(out command))
            {
                sink(command);
                drained++;
            }

            return drained;
        }

        public void EnqueueOutgoingPacket(byte[] packet)
        {
            if (packet == null || packet.Length == 0)
            {
                return;
            }

            _outgoingPackets.Enqueue(packet);
            while (_outgoingPackets.Count > MaxQueuedOutgoingPackets)
            {
                byte[] dropped;
                if (!_outgoingPackets.TryDequeue(out dropped))
                {
                    break;
                }
            }
        }

        public void EnqueuePoseSample(
            ulong sampleId,
            ulong timestampNs,
            ulong predictedDisplayTimeNs,
            float[] position,
            float[] orientation,
            uint trackingFlags)
        {
            EnqueueOutgoingPacket(MetalXRQuestProtocol.CreatePoseSamplePacket(
                NextSequence(),
                sampleId,
                timestampNs,
                predictedDisplayTimeNs,
                position,
                orientation,
                trackingFlags));
        }

        public void EnqueueControllerInput(
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
            EnqueueOutgoingPacket(MetalXRQuestProtocol.CreateControllerInputPacket(
                NextSequence(),
                sampleId,
                timestampNs,
                hand,
                buttons,
                trigger,
                grip,
                thumbstick,
                trackingFlags,
                aimPosition,
                aimOrientation,
                gripPosition,
                gripOrientation));
        }

        public void EnqueueTimingSample(MetalXRQuestTimingSample sample)
        {
            if (sample == null)
            {
                return;
            }

            EnqueueOutgoingPacket(MetalXRQuestProtocol.CreateTimingSamplePacket(NextSequence(), sample));
        }

        public void Dispose()
        {
            lock (_lifecycleLock)
            {
                _stopRequested = true;
                if (_thread != null && _thread.IsAlive)
                {
                    _thread.Join(250);
                }

                _thread = null;
            }
        }

        private void Run()
        {
            _logs.Enqueue("stream thread started");
            while (!_stopRequested)
            {
                RunConnectionOnce();

                int waitedMs = 0;
                while (!_stopRequested && waitedMs < RetryDelayMs)
                {
                    Thread.Sleep(100);
                    waitedMs += 100;
                }
            }

            _logs.Enqueue("stream thread stopped");
        }

        private void RunConnectionOnce()
        {
            try
            {
                using (TcpClient client = new TcpClient())
                {
                    IAsyncResult connectResult = client.BeginConnect(_endpoint.Host, _endpoint.Port, null, null);
                    if (!connectResult.AsyncWaitHandle.WaitOne(ConnectTimeoutMs))
                    {
                        client.Close();
                        LogConnectionFailure("host stream endpoint timed out");
                        return;
                    }

                    client.EndConnect(connectResult);
                    client.NoDelay = true;
                    client.ReceiveTimeout = ReadTimeoutMs;
                    client.SendTimeout = WriteTimeoutMs;
                    _failureCount = 0;

                    using (NetworkStream stream = client.GetStream())
                    {
                        stream.ReadTimeout = ReadTimeoutMs;
                        stream.WriteTimeout = WriteTimeoutMs;
                        byte[] hello = MetalXRQuestProtocol.CreateHelloPacket(NextSequence(), _deviceProfile, _capabilities);
                        stream.Write(hello, 0, hello.Length);
                        _logs.Enqueue(
                            "sent HELLO to " + _endpoint.Host + ":" + _endpoint.Port +
                            " caps=0x" + _capabilities.ToString("x8") +
                            " max=" + _deviceProfile.MaxVideoWidth + "x" + _deviceProfile.MaxVideoHeight +
                            " fps=" + _deviceProfile.PreferredFps);

                        MetalXRQuestProtocol.PacketHeader header;
                        if (TryReadHeader(stream, out header))
                        {
                            if (header.Type == MetalXRQuestProtocol.PacketHelloAck)
                            {
                                if (!SkipPayload(stream, header.PayloadSize))
                                {
                                    _logs.Enqueue("host closed while sending HELLO_ACK payload");
                                    return;
                                }

                                _logs.Enqueue("received HELLO_ACK from host");
                                ConnectionWriterState writerState = new ConnectionWriterState();
                                Thread writerThread = StartOutgoingWriter(stream, writerState);
                                try
                                {
                                    ReceiveStreamPackets(stream);
                                }
                                finally
                                {
                                    writerState.StopRequested = true;
                                    if (writerThread != null && writerThread.IsAlive)
                                    {
                                        writerThread.Join(250);
                                    }
                                }
                            }
                            else if (header.Type == MetalXRQuestProtocol.PacketError)
                            {
                                _logs.Enqueue("received ERROR packet from host");
                                SkipPayload(stream, header.PayloadSize);
                            }
                            else
                            {
                                _logs.Enqueue("received unexpected packet type " + header.Type);
                                SkipPayload(stream, header.PayloadSize);
                            }
                        }
                        else
                        {
                            _logs.Enqueue("host closed before HELLO_ACK");
                        }
                    }
                }
            }
            catch (Exception exception)
            {
                LogConnectionFailure(exception.Message);
            }
        }

        private void ReceiveStreamPackets(NetworkStream stream)
        {
            while (!_stopRequested)
            {
                MetalXRQuestProtocol.PacketHeader header;
                if (!TryReadHeader(stream, out header))
                {
                    _logs.Enqueue("host stream closed");
                    return;
                }

                if (header.PayloadSize > MaxPayloadBytes)
                {
                    _logs.Enqueue("dropping oversized packet type " + header.Type + " bytes=" + header.PayloadSize);
                    if (!SkipPayload(stream, header.PayloadSize))
                    {
                        return;
                    }
                    continue;
                }

                byte[] payload;
                if (!TryReadPayload(stream, header.PayloadSize, out payload))
                {
                    _logs.Enqueue("host stream closed while reading packet type " + header.Type);
                    return;
                }

                if (header.Type == MetalXRQuestProtocol.PacketVideoFrame)
                {
                    MetalXRQuestEncodedVideoFrame frame;
                    if (MetalXRQuestProtocol.TryParseVideoFramePayload(
                        header,
                        payload,
                        MetalXRQuestProtocol.NowNs(),
                        out frame))
                    {
                        AssembleFrame(frame);
                    }
                    else
                    {
                        _logs.Enqueue("dropped malformed VIDEO_FRAME packet bytes=" + header.PayloadSize);
                    }
                }
                else if (header.Type == MetalXRQuestProtocol.PacketError)
                {
                    _logs.Enqueue("received ERROR packet from host");
                }
                else if (header.Type == MetalXRQuestProtocol.PacketHapticCommand)
                {
                    MetalXRQuestHapticCommand command;
                    if (MetalXRQuestProtocol.TryParseHapticCommandPayload(payload, out command))
                    {
                        EnqueueHaptic(command);
                    }
                    else
                    {
                        _logs.Enqueue("dropped malformed HAPTIC_COMMAND packet bytes=" + header.PayloadSize);
                    }
                }
                else if (header.Type == MetalXRQuestProtocol.PacketTimingSample)
                {
                    MetalXRQuestTimingSample sample;
                    if (MetalXRQuestProtocol.TryParseTimingSamplePayload(payload, out sample))
                    {
                        HandleTimingSample(sample);
                    }
                    else
                    {
                        _logs.Enqueue("dropped malformed TIMING_SAMPLE packet bytes=" + header.PayloadSize);
                    }
                }

            }
        }

        private Thread StartOutgoingWriter(Stream stream, ConnectionWriterState state)
        {
            Thread writerThread = new Thread(() => RunOutgoingWriter(stream, state));
            writerThread.Name = "MetalXR Quest Control Writer";
            writerThread.IsBackground = true;
            writerThread.Start();
            return writerThread;
        }

        private void RunOutgoingWriter(Stream stream, ConnectionWriterState state)
        {
            _logs.Enqueue("control writer thread started");
            try
            {
                while (!_stopRequested && state != null && !state.StopRequested)
                {
                    bool wrotePacket = FlushOutgoingPackets(stream);
                    Thread.Sleep(wrotePacket ? 1 : 5);
                }
            }
            catch (Exception exception)
            {
                _logs.Enqueue("control writer stopped: " + exception.Message);
            }

            _logs.Enqueue("control writer thread stopped");
        }

        private void AssembleFrame(MetalXRQuestEncodedVideoFrame frame)
        {
            if (frame == null)
            {
                return;
            }

            PartialStereoFrameSet partial;
            if (!_partialFrameSets.TryGetValue(frame.FrameId, out partial))
            {
                partial = new PartialStereoFrameSet(frame.FrameId);
                _partialFrameSets[frame.FrameId] = partial;
            }

            if (frame.IsLeftEye)
            {
                partial.Left = frame;
            }
            else
            {
                partial.Right = frame;
            }

            if (!partial.IsComplete)
            {
                DropOldPartialFrameSets();
                return;
            }

            _partialFrameSets.Remove(frame.FrameId);
            EnqueueFrameSet(new MetalXRQuestEncodedStereoFrameSet(frame.FrameId, partial.Left, partial.Right));
        }

        private void EnqueueFrameSet(MetalXRQuestEncodedStereoFrameSet frameSet)
        {
            _frameSets.Enqueue(frameSet);
            while (_frameSets.Count > MaxQueuedFrameSets)
            {
                MetalXRQuestEncodedStereoFrameSet dropped;
                if (!_frameSets.TryDequeue(out dropped))
                {
                    break;
                }
            }

            _receivedFrameSets++;
            if (_receivedFrameSets <= 4 || (_receivedFrameSets % 120) == 0)
            {
                _logs.Enqueue(
                    "received complete VIDEO_FRAME FrameSet frame=" + frameSet.FrameId +
                    " left_bytes=" + frameSet.Left.EncodedBytes.Length +
                    " right_bytes=" + frameSet.Right.EncodedBytes.Length +
                    " left_keyframe=" + frameSet.Left.IsKeyframe +
                    " right_keyframe=" + frameSet.Right.IsKeyframe +
                    " left_projection_metadata=" + frameSet.Left.HasProjectionMetadata +
                    " right_projection_metadata=" + frameSet.Right.HasProjectionMetadata);
            }
        }

        private void DropOldPartialFrameSets()
        {
            while (_partialFrameSets.Count > MaxPartialFrameSets)
            {
                ulong oldestFrameId = ulong.MaxValue;
                foreach (ulong frameId in _partialFrameSets.Keys)
                {
                    if (frameId < oldestFrameId)
                    {
                        oldestFrameId = frameId;
                    }
                }

                if (oldestFrameId == ulong.MaxValue)
                {
                    break;
                }

                _partialFrameSets.Remove(oldestFrameId);
            }
        }

        private void HandleTimingSample(MetalXRQuestTimingSample sample)
        {
            if (!sample.IsClockSync)
            {
                return;
            }

            ulong nowNs = MetalXRQuestProtocol.NowNs();
            EnqueueTimingSample(new MetalXRQuestTimingSample(
                sample.FrameId,
                sample.HostCaptureTimeNs,
                sample.PredictedDisplayTimeNs,
                sample.EncodeStartTimeNs,
                sample.EncodeEndTimeNs,
                nowNs,
                nowNs,
                0,
                0,
                nowNs,
                (uint)QueuedFrameCount,
                MetalXRQuestProtocol.TimingFlagClockSync));
        }

        private void EnqueueHaptic(MetalXRQuestHapticCommand command)
        {
            _hapticCommands.Enqueue(command);
            while (_hapticCommands.Count > MaxQueuedHaptics)
            {
                MetalXRQuestHapticCommand dropped;
                if (!_hapticCommands.TryDequeue(out dropped))
                {
                    break;
                }
            }

            string handName = command.IsLeftHand ? "left" : "right";
            _logs.Enqueue(
                "received HAPTIC_COMMAND id=" + command.CommandId +
                " hand=" + handName +
                " amplitude=" + command.Amplitude +
                " duration_us=" + command.DurationUs);
        }

        private bool FlushOutgoingPackets(Stream stream)
        {
            bool wrotePacket = false;
            byte[] packet;
            while (_outgoingPackets.TryDequeue(out packet))
            {
                stream.Write(packet, 0, packet.Length);
                wrotePacket = true;
            }

            return wrotePacket;
        }

        private static bool TryReadHeader(Stream stream, out MetalXRQuestProtocol.PacketHeader header)
        {
            header = default;
            byte[] buffer = new byte[MetalXRQuestProtocol.HeaderSize];
            return ReadExact(stream, buffer, buffer.Length) &&
                   MetalXRQuestProtocol.TryParseHeader(buffer, out header);
        }

        private static bool TryReadPayload(Stream stream, uint payloadSize, out byte[] payload)
        {
            payload = new byte[(int)payloadSize];
            return payloadSize == 0 || ReadExact(stream, payload, payload.Length);
        }

        private static bool SkipPayload(Stream stream, uint payloadSize)
        {
            byte[] buffer = new byte[4096];
            uint remaining = payloadSize;
            while (remaining > 0)
            {
                int chunk = remaining > (uint)buffer.Length ? buffer.Length : (int)remaining;
                if (!ReadExact(stream, buffer, chunk))
                {
                    return false;
                }

                remaining -= (uint)chunk;
            }

            return true;
        }

        private static bool ReadExact(Stream stream, byte[] buffer, int count)
        {
            int offset = 0;
            while (offset < count)
            {
                int read = stream.Read(buffer, offset, count - offset);
                if (read <= 0)
                {
                    return false;
                }

                offset += read;
            }

            return true;
        }

        private void LogConnectionFailure(string reason)
        {
            _failureCount++;
            if (_failureCount <= 3 || (_failureCount % 10) == 0)
            {
                _logs.Enqueue("host stream unavailable at " + _endpoint.Host + ":" + _endpoint.Port + " (" + reason + ")");
            }
        }

        private ulong NextSequence()
        {
            lock (_sequenceLock)
            {
                ulong sequence = _sequence;
                _sequence++;
                return sequence;
            }
        }

        private sealed class PartialStereoFrameSet
        {
            public PartialStereoFrameSet(ulong frameId)
            {
                FrameId = frameId;
            }

            public ulong FrameId { get; private set; }
            public MetalXRQuestEncodedVideoFrame Left { get; set; }
            public MetalXRQuestEncodedVideoFrame Right { get; set; }
            public bool IsComplete { get { return Left != null && Right != null; } }
        }

        private sealed class ConnectionWriterState
        {
            public volatile bool StopRequested;
        }
    }
}
