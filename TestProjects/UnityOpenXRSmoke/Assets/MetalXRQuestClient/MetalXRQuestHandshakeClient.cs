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
        private readonly MetalXRQuestEndpoint _endpoint;
        private readonly ConcurrentQueue<string> _logs = new ConcurrentQueue<string>();
        private readonly object _lifecycleLock = new object();
        private Thread _thread;
        private bool _stopRequested;
        private ulong _sequence = 1;
        private int _failureCount;

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
            _logs.Enqueue("handshake thread started");
            while (!_stopRequested)
            {
                TryHandshakeOnce();

                int waitedMs = 0;
                while (!_stopRequested && waitedMs < RetryDelayMs)
                {
                    Thread.Sleep(100);
                    waitedMs += 100;
                }
            }

            _logs.Enqueue("handshake thread stopped");
        }

        private void TryHandshakeOnce()
        {
            try
            {
                using (TcpClient client = new TcpClient())
                {
                    IAsyncResult connectResult = client.BeginConnect(_endpoint.Host, _endpoint.Port, null, null);
                    if (!connectResult.AsyncWaitHandle.WaitOne(ConnectTimeoutMs))
                    {
                        client.Close();
                        LogConnectionFailure("host handshake endpoint timed out");
                        return;
                    }

                    client.EndConnect(connectResult);
                    client.NoDelay = true;
                    _failureCount = 0;

                    using (NetworkStream stream = client.GetStream())
                    {
                        stream.ReadTimeout = ReadTimeoutMs;
                        byte[] hello = MetalXRQuestProtocol.CreateHelloPacket(_sequence++);
                        stream.Write(hello, 0, hello.Length);
                        _logs.Enqueue("sent HELLO to " + _endpoint.Host + ":" + _endpoint.Port);

                        MetalXRQuestProtocol.PacketHeader header;
                        if (TryReadHeader(stream, out header))
                        {
                            if (header.Type == MetalXRQuestProtocol.PacketHelloAck)
                            {
                                _logs.Enqueue("received HELLO_ACK from host");
                            }
                            else if (header.Type == MetalXRQuestProtocol.PacketError)
                            {
                                _logs.Enqueue("received ERROR packet from host");
                            }
                            else
                            {
                                _logs.Enqueue("received unexpected packet type " + header.Type);
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

        private static bool TryReadHeader(Stream stream, out MetalXRQuestProtocol.PacketHeader header)
        {
            header = default;
            byte[] buffer = new byte[MetalXRQuestProtocol.HeaderSize];
            int offset = 0;
            while (offset < buffer.Length)
            {
                int read = stream.Read(buffer, offset, buffer.Length - offset);
                if (read <= 0)
                {
                    return false;
                }

                offset += read;
            }

            return MetalXRQuestProtocol.TryParseHeader(buffer, out header);
        }

        private void LogConnectionFailure(string reason)
        {
            _failureCount++;
            if (_failureCount <= 3 || (_failureCount % 10) == 0)
            {
                _logs.Enqueue("host handshake unavailable at " + _endpoint.Host + ":" + _endpoint.Port + " (" + reason + ")");
            }
        }
    }
}
