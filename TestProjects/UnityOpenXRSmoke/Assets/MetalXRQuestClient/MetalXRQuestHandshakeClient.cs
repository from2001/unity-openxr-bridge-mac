using System;
using System.Collections.Concurrent;
using System.IO;
using System.Net.Sockets;
using System.Threading;

namespace MetalXR.QuestClient
{
    public sealed class MetalXRQuestHandshakeClient : IDisposable
    {
        private const int ConnectTimeoutMs = 1000;
        private const int ReadTimeoutMs = 1000;
        private const int RetryDelayMs = 2500;
        private const int MaxPayloadBytes = 8 * 1024 * 1024;
        private const int MaxQueuedFrames = 8;
        private const int MaxQueuedOutgoingPackets = 32;
        private const int MaxQueuedHaptics = 8;
        private readonly MetalXRQuestEndpoint _endpoint;
        private readonly ConcurrentQueue<string> _logs = new ConcurrentQueue<string>();
        private readonly ConcurrentQueue<MetalXRQuestEncodedVideoFrame> _frames = new ConcurrentQueue<MetalXRQuestEncodedVideoFrame>();
        private readonly ConcurrentQueue<byte[]> _outgoingPackets = new ConcurrentQueue<byte[]>();
        private readonly ConcurrentQueue<MetalXRQuestHapticCommand> _hapticCommands = new ConcurrentQueue<MetalXRQuestHapticCommand>();
        private readonly object _lifecycleLock = new object();
        private readonly object _sequenceLock = new object();
        private Thread _thread;
        private bool _stopRequested;
        private ulong _sequence = 1;
        private int _failureCount;
        private ulong _receivedEyeFrames;

        public MetalXRQuestHandshakeClient(MetalXRQuestEndpoint endpoint)
        {
            _endpoint = endpoint;
        }

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

        public int DrainFrames(Action<MetalXRQuestEncodedVideoFrame> sink, int maxFrames)
        {
            if (sink == null || maxFrames <= 0)
            {
                return 0;
            }

            int drained = 0;
            MetalXRQuestEncodedVideoFrame frame;
            while (drained < maxFrames && _frames.TryDequeue(out frame))
            {
                sink(frame);
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
                    _failureCount = 0;

                    using (NetworkStream stream = client.GetStream())
                    {
                        stream.ReadTimeout = ReadTimeoutMs;
                        byte[] hello = MetalXRQuestProtocol.CreateHelloPacket(NextSequence());
                        stream.Write(hello, 0, hello.Length);
                        _logs.Enqueue("sent HELLO to " + _endpoint.Host + ":" + _endpoint.Port);

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
                                ReceiveStreamPackets(stream);
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
                        EnqueueFrame(frame);
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

                FlushOutgoingPackets(stream);
            }
        }

        private void EnqueueFrame(MetalXRQuestEncodedVideoFrame frame)
        {
            _frames.Enqueue(frame);
            while (_frames.Count > MaxQueuedFrames)
            {
                MetalXRQuestEncodedVideoFrame dropped;
                if (!_frames.TryDequeue(out dropped))
                {
                    break;
                }
            }

            _receivedEyeFrames++;
            if (_receivedEyeFrames <= 4 || (_receivedEyeFrames % 120) == 0)
            {
                string eyeName = frame.IsLeftEye ? "left" : "right";
                _logs.Enqueue(
                    "received VIDEO_FRAME frame=" + frame.FrameId +
                    " eye=" + eyeName +
                    " bytes=" + frame.EncodedBytes.Length +
                    " keyframe=" + frame.IsKeyframe);
            }
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

        private void FlushOutgoingPackets(Stream stream)
        {
            byte[] packet;
            while (_outgoingPackets.TryDequeue(out packet))
            {
                stream.Write(packet, 0, packet.Length);
            }
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
    }
}
