using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.XR;

namespace MetalXR.QuestClient
{
    public sealed class MetalXRQuestClientDisplay : MonoBehaviour
    {
        private const string LogPrefix = "MetalXRQuestClient";
        private const int EyeLayerLeft = 28;
        private const int EyeLayerRight = 29;
        private const int TextureWidth = 640;
        private const int TextureHeight = 360;
        private const float FrameUpdateIntervalSeconds = 1.0f / 12.0f;
        private const float InputSampleIntervalSeconds = 1.0f / 30.0f;
        private const int MaxStreamFramesPerUpdate = 8;
        private const int MaxHapticCommandsPerUpdate = 4;

        private Texture2D _leftTexture;
        private Texture2D _rightTexture;
        private Material _leftMaterial;
        private Material _rightMaterial;
        private Mesh _quadMesh;
        private Color32[] _leftPixels;
        private Color32[] _rightPixels;
        private MetalXRQuestHandshakeClient _handshakeClient;
        private MetalXRQuestVideoDecoder _videoDecoder;
        private float _nextFrameTime;
        private float _nextInputSampleTime;
        private float _lastStreamFrameTime = -10.0f;
        private int _frameIndex;
        private ulong _inputSampleId;
        private ulong _loggedInputSamples;
        private ulong _displayedEyeFrames;
        private ulong _displayedLeftEyeFrames;
        private ulong _displayedRightEyeFrames;

        private void Start()
        {
            Application.targetFrameRate = 72;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;

            CreateDiagnosticRig();
            UpdateDiagnosticTexture(true, 0);
            UpdateDiagnosticTexture(false, 0);

            MetalXRQuestEndpoint endpoint = MetalXRQuestEndpoint.FromCommandLine();
            _videoDecoder = new MetalXRQuestVideoDecoder(LogClientMessage);
            _handshakeClient = new MetalXRQuestHandshakeClient(endpoint);
            _handshakeClient.Start();

            Debug.Log(LogPrefix + " started; stream client active with diagnostic fallback; host=" + endpoint.Host + ":" + endpoint.Port);
        }

        private void Update()
        {
            if (_handshakeClient != null)
            {
                _handshakeClient.DrainLogs(LogClientMessage);
                _handshakeClient.DrainFrames(HandleEncodedFrame, MaxStreamFramesPerUpdate);
                _handshakeClient.DrainHapticCommands(HandleHapticCommand, MaxHapticCommandsPerUpdate);
                EnqueueTrackingSamples();
            }

            if (Time.unscaledTime - _lastStreamFrameTime > 0.5f && Time.unscaledTime >= _nextFrameTime)
            {
                _nextFrameTime = Time.unscaledTime + FrameUpdateIntervalSeconds;
                _frameIndex++;
                UpdateDiagnosticTexture(true, _frameIndex);
                UpdateDiagnosticTexture(false, _frameIndex);
            }
        }

        private void OnDestroy()
        {
            if (_handshakeClient != null)
            {
                _handshakeClient.Dispose();
                _handshakeClient = null;
            }

            if (_videoDecoder != null)
            {
                _videoDecoder.Dispose();
                _videoDecoder = null;
            }

            DestroyUnityObject(_leftMaterial);
            DestroyUnityObject(_rightMaterial);
            DestroyUnityObject(_leftTexture);
            DestroyUnityObject(_rightTexture);
            DestroyUnityObject(_quadMesh);
        }

        private static void LogClientMessage(string message)
        {
            Debug.Log(LogPrefix + " " + message);
        }

        private void HandleEncodedFrame(MetalXRQuestEncodedVideoFrame encodedFrame)
        {
            if (_videoDecoder == null)
            {
                return;
            }

            ulong decodeStartTimeNs = MetalXRQuestProtocol.NowNs();
            MetalXRQuestDecodedVideoFrame decodedFrame;
            if (_videoDecoder.TryDecode(encodedFrame, out decodedFrame) && decodedFrame != null)
            {
                ulong decodeEndTimeNs = MetalXRQuestProtocol.NowNs();
                decodedFrame.SetDecodeTiming(decodeStartTimeNs, decodeEndTimeNs);
                UpdateStreamTexture(decodedFrame);
            }
        }

        private void HandleHapticCommand(MetalXRQuestHapticCommand command)
        {
            XRNode node = command.IsLeftHand ? XRNode.LeftHand : XRNode.RightHand;
            InputDevice device = InputDevices.GetDeviceAtXRNode(node);
            if (!device.isValid)
            {
                return;
            }

            float amplitude = Mathf.Clamp01(command.Amplitude);
            float durationSeconds = Mathf.Max(0.0f, command.DurationUs / 1000000.0f);
            device.SendHapticImpulse(0u, amplitude, durationSeconds);
        }

        private void EnqueueTrackingSamples()
        {
            if (Time.unscaledTime < _nextInputSampleTime || _handshakeClient == null)
            {
                return;
            }

            _nextInputSampleTime = Time.unscaledTime + InputSampleIntervalSeconds;
            _inputSampleId++;
            ulong timestampNs = MetalXRQuestProtocol.NowNs();
            SendHeadPose(_inputSampleId, timestampNs);
            SendControllerInput(_inputSampleId, timestampNs, true);
            SendControllerInput(_inputSampleId, timestampNs, false);
        }

        private void SendHeadPose(ulong sampleId, ulong timestampNs)
        {
            InputDevice head = InputDevices.GetDeviceAtXRNode(XRNode.Head);
            Vector3 position;
            Quaternion rotation;
            uint trackingFlags = ReadPose(head, out position, out rotation);
            _handshakeClient.EnqueuePoseSample(
                sampleId,
                timestampNs,
                timestampNs,
                VectorToArray(position),
                QuaternionToArray(rotation),
                trackingFlags);
            LogInputSample("hmd", sampleId, trackingFlags);
        }

        private void SendControllerInput(ulong sampleId, ulong timestampNs, bool leftHand)
        {
            XRNode node = leftHand ? XRNode.LeftHand : XRNode.RightHand;
            InputDevice device = InputDevices.GetDeviceAtXRNode(node);
            Vector3 position;
            Quaternion rotation;
            uint trackingFlags = ReadPose(device, out position, out rotation);

            float trigger = ReadFloatFeature(device, CommonUsages.trigger);
            float grip = ReadFloatFeature(device, CommonUsages.grip);
            Vector2 thumbstick = ReadVector2Feature(device, CommonUsages.primary2DAxis);
            uint buttons = ReadButtonFlags(device);
            uint hand = leftHand ? MetalXRQuestProtocol.ControllerHandLeft : MetalXRQuestProtocol.ControllerHandRight;

            _handshakeClient.EnqueueControllerInput(
                sampleId,
                timestampNs,
                hand,
                buttons,
                trigger,
                grip,
                new[] { thumbstick.x, thumbstick.y },
                trackingFlags,
                VectorToArray(position),
                QuaternionToArray(rotation),
                VectorToArray(position),
                QuaternionToArray(rotation));

            LogInputSample(leftHand ? "left-controller" : "right-controller", sampleId, trackingFlags);
        }

        private static uint ReadPose(InputDevice device, out Vector3 position, out Quaternion rotation)
        {
            position = Vector3.zero;
            rotation = Quaternion.identity;
            if (!device.isValid)
            {
                return 0;
            }

            uint flags = 0;
            bool isTracked;
            if (device.TryGetFeatureValue(CommonUsages.isTracked, out isTracked) && isTracked)
            {
                flags |= MetalXRQuestProtocol.TrackingOrientationTracked | MetalXRQuestProtocol.TrackingPositionTracked;
            }

            Vector3 readPosition;
            if (device.TryGetFeatureValue(CommonUsages.devicePosition, out readPosition))
            {
                position = readPosition;
                flags |= MetalXRQuestProtocol.TrackingPositionValid;
            }

            Quaternion readRotation;
            if (device.TryGetFeatureValue(CommonUsages.deviceRotation, out readRotation))
            {
                rotation = readRotation;
                flags |= MetalXRQuestProtocol.TrackingOrientationValid;
            }

            return flags;
        }

        private static float ReadFloatFeature(InputDevice device, InputFeatureUsage<float> usage)
        {
            if (!device.isValid)
            {
                return 0.0f;
            }

            float value;
            return device.TryGetFeatureValue(usage, out value) ? value : 0.0f;
        }

        private static Vector2 ReadVector2Feature(InputDevice device, InputFeatureUsage<Vector2> usage)
        {
            if (!device.isValid)
            {
                return Vector2.zero;
            }

            Vector2 value;
            return device.TryGetFeatureValue(usage, out value) ? value : Vector2.zero;
        }

        private static uint ReadButtonFlags(InputDevice device)
        {
            if (!device.isValid)
            {
                return 0;
            }

            uint buttons = 0;
            if (ReadButtonFeature(device, CommonUsages.primaryButton))
            {
                buttons |= MetalXRQuestProtocol.ControllerButtonPrimary;
            }

            if (ReadButtonFeature(device, CommonUsages.secondaryButton))
            {
                buttons |= MetalXRQuestProtocol.ControllerButtonSecondary;
            }

            if (ReadButtonFeature(device, CommonUsages.menuButton))
            {
                buttons |= MetalXRQuestProtocol.ControllerButtonMenu;
            }

            if (ReadButtonFeature(device, CommonUsages.primary2DAxisClick))
            {
                buttons |= MetalXRQuestProtocol.ControllerButtonThumbstick;
            }

            return buttons;
        }

        private static bool ReadButtonFeature(InputDevice device, InputFeatureUsage<bool> usage)
        {
            bool value;
            return device.TryGetFeatureValue(usage, out value) && value;
        }

        private static float[] VectorToArray(Vector3 value)
        {
            return new[] { value.x, value.y, value.z };
        }

        private static float[] QuaternionToArray(Quaternion value)
        {
            return new[] { value.x, value.y, value.z, value.w };
        }

        private void LogInputSample(string source, ulong sampleId, uint trackingFlags)
        {
            _loggedInputSamples++;
            if (_loggedInputSamples <= 6 || (_loggedInputSamples % 300) == 0)
            {
                Debug.Log(LogPrefix + " sent input sample source=" + source + " id=" + sampleId + " flags=0x" + trackingFlags.ToString("x8"));
            }
        }

        private void CreateDiagnosticRig()
        {
            Shader unlitShader = Shader.Find("Universal Render Pipeline/Unlit");
            if (unlitShader == null)
            {
                unlitShader = Shader.Find("Unlit/Texture");
            }

            _leftTexture = CreateTexture("MetalXR Left Diagnostic");
            _rightTexture = CreateTexture("MetalXR Right Diagnostic");
            _quadMesh = CreateQuadMesh();
            _leftPixels = new Color32[TextureWidth * TextureHeight];
            _rightPixels = new Color32[TextureWidth * TextureHeight];

            _leftMaterial = CreateMaterial(unlitShader, _leftTexture, "MetalXR Left Material");
            _rightMaterial = CreateMaterial(unlitShader, _rightTexture, "MetalXR Right Material");

            GameObject rigRoot = CreateRigRoot();

            CreateEyePanel(rigRoot.transform, "MetalXR Left Diagnostic Panel", EyeLayerLeft, _leftMaterial, -0.55f);
            CreateEyePanel(rigRoot.transform, "MetalXR Right Diagnostic Panel", EyeLayerRight, _rightMaterial, 0.55f);
        }

        private static Texture2D CreateTexture(string name)
        {
            Texture2D texture = new Texture2D(TextureWidth, TextureHeight, TextureFormat.RGBA32, false);
            texture.name = name;
            texture.wrapMode = TextureWrapMode.Clamp;
            texture.filterMode = FilterMode.Point;
            return texture;
        }

        private static Material CreateMaterial(Shader shader, Texture texture, string name)
        {
            Material material;
            if (shader != null)
            {
                material = new Material(shader);
            }
            else
            {
                material = new Material(Shader.Find("Sprites/Default"));
            }

            material.name = name;
            if (material.HasProperty("_BaseMap"))
            {
                material.SetTexture("_BaseMap", texture);
            }
            else
            {
                material.mainTexture = texture;
            }

            if (material.HasProperty("_BaseColor"))
            {
                material.SetColor("_BaseColor", Color.white);
            }

            return material;
        }

        private GameObject CreateRigRoot()
        {
            GameObject root = new GameObject("MetalXR Diagnostic Rig");
            root.transform.SetParent(transform, false);

            Camera camera = root.AddComponent<Camera>();
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color(0.015f, 0.017f, 0.02f, 1.0f);
            camera.nearClipPlane = 0.01f;
            camera.farClipPlane = 20.0f;
            camera.fieldOfView = 62.0f;
            camera.depth = 10.0f;
            camera.cullingMask = (1 << EyeLayerLeft) | (1 << EyeLayerRight);
            camera.useOcclusionCulling = false;
            camera.allowHDR = false;
            camera.allowMSAA = true;

            return root;
        }

        private void CreateEyePanel(Transform parent, string name, int layer, Material material, float localX)
        {
            GameObject panel = new GameObject(name);
            panel.name = name;
            panel.layer = layer;
            panel.transform.SetParent(parent, false);
            panel.transform.localPosition = new Vector3(localX, 0.0f, 2.0f);
            panel.transform.localRotation = Quaternion.identity;
            panel.transform.localScale = new Vector3(0.98f, 0.55f, 1.0f);

            MeshFilter meshFilter = panel.AddComponent<MeshFilter>();
            meshFilter.sharedMesh = _quadMesh;

            MeshRenderer meshRenderer = panel.AddComponent<MeshRenderer>();
            meshRenderer.sharedMaterial = material;
        }

        private static Mesh CreateQuadMesh()
        {
            Mesh mesh = new Mesh();
            mesh.name = "MetalXR Diagnostic Quad";
            mesh.vertices = new[]
            {
                new Vector3(-0.5f, -0.5f, 0.0f),
                new Vector3(0.5f, -0.5f, 0.0f),
                new Vector3(-0.5f, 0.5f, 0.0f),
                new Vector3(0.5f, 0.5f, 0.0f)
            };
            mesh.uv = new[]
            {
                new Vector2(0.0f, 0.0f),
                new Vector2(1.0f, 0.0f),
                new Vector2(0.0f, 1.0f),
                new Vector2(1.0f, 1.0f)
            };
            mesh.triangles = new[] { 0, 2, 1, 2, 3, 1 };
            mesh.RecalculateBounds();
            return mesh;
        }

        private void UpdateDiagnosticTexture(bool leftEye, int frameIndex)
        {
            Color32[] pixels = leftEye ? _leftPixels : _rightPixels;
            Texture2D texture = leftEye ? _leftTexture : _rightTexture;
            if (pixels == null || texture == null)
            {
                return;
            }

            for (int y = 0; y < TextureHeight; y++)
            {
                for (int x = 0; x < TextureWidth; x++)
                {
                    int index = y * TextureWidth + x;
                    pixels[index] = DiagnosticColor(leftEye, x, y, frameIndex);
                }
            }

            texture.SetPixels32(pixels);
            texture.Apply(false);
        }

        private void UpdateStreamTexture(MetalXRQuestDecodedVideoFrame frame)
        {
            Color32[] pixels = frame.IsLeftEye ? _leftPixels : _rightPixels;
            Texture2D texture = frame.IsLeftEye ? _leftTexture : _rightTexture;
            if (pixels == null || texture == null || frame.LumaBytes == null)
            {
                return;
            }

            int sourceWidth = frame.Width > 0 ? frame.Width : 1;
            int sourceHeight = frame.Height > 0 ? frame.Height : 1;
            int stride = frame.Stride >= sourceWidth ? frame.Stride : sourceWidth;
            int requiredBytes = ((sourceHeight - 1) * stride) + sourceWidth;
            if (frame.LumaBytes.Length < requiredBytes)
            {
                return;
            }

            for (int y = 0; y < TextureHeight; y++)
            {
                int sourceY = (y * sourceHeight) / TextureHeight;
                int sourceRow = sourceY * stride;
                for (int x = 0; x < TextureWidth; x++)
                {
                    int sourceX = (x * sourceWidth) / TextureWidth;
                    byte luma = frame.LumaBytes[sourceRow + sourceX];
                    pixels[y * TextureWidth + x] = StreamColor(frame.IsLeftEye, luma);
                }
            }

            texture.SetPixels32(pixels);
            texture.Apply(false);
            ulong displayTimeNs = MetalXRQuestProtocol.NowNs();
            _lastStreamFrameTime = Time.unscaledTime;
            _displayedEyeFrames++;
            ulong displayedThisEye;
            if (frame.IsLeftEye)
            {
                _displayedLeftEyeFrames++;
                displayedThisEye = _displayedLeftEyeFrames;
            }
            else
            {
                _displayedRightEyeFrames++;
                displayedThisEye = _displayedRightEyeFrames;
            }

            SendFrameTimingSample(frame, displayTimeNs);

            if (displayedThisEye <= 4 || (_displayedEyeFrames % 120) == 0)
            {
                ulong receiveToDisplayUs =
                    displayTimeNs >= frame.ReceiveTimeNs ?
                        (displayTimeNs - frame.ReceiveTimeNs) / 1000ul :
                        0ul;
                ulong decodeUs =
                    frame.DecodeEndTimeNs >= frame.DecodeStartTimeNs ?
                        (frame.DecodeEndTimeNs - frame.DecodeStartTimeNs) / 1000ul :
                        0ul;
                ulong submitUs =
                    displayTimeNs >= frame.DecodeEndTimeNs ?
                        (displayTimeNs - frame.DecodeEndTimeNs) / 1000ul :
                        0ul;
                string decoderName = frame.DecodedByMediaCodec ? "MediaCodec" : "compressed-preview";
                string eyeName = frame.IsLeftEye ? "left" : "right";
                Debug.Log(
                    LogPrefix +
                    " displayed VIDEO_FRAME frame=" + frame.FrameId +
                    " eye=" + eyeName +
                    " decoder=" + decoderName +
                    " host_encoder_us=" + frame.EncoderLatencyUs +
                    " client_receive_to_display_us=" + receiveToDisplayUs +
                    " client_decode_us=" + decodeUs +
                    " client_submit_us=" + submitUs +
                    " queue_depth=" + CurrentQueuedFrameCount() +
                    " bytes=" + frame.EncodedBytes);
            }
        }

        private void SendFrameTimingSample(MetalXRQuestDecodedVideoFrame frame, ulong displayTimeNs)
        {
            if (_handshakeClient == null)
            {
                return;
            }

            ulong encodeLatencyNs = frame.EncoderLatencyUs * 1000ul;
            ulong encodeEndTimeNs = frame.PacketTimestampNs;
            ulong encodeStartTimeNs =
                encodeEndTimeNs >= encodeLatencyNs ?
                    encodeEndTimeNs - encodeLatencyNs :
                    0ul;

            _handshakeClient.EnqueueTimingSample(new MetalXRQuestTimingSample(
                frame.FrameId,
                frame.HostCaptureTimeNs,
                frame.PredictedDisplayTimeNs,
                encodeStartTimeNs,
                encodeEndTimeNs,
                frame.ReceiveTimeNs,
                displayTimeNs,
                frame.DecodeStartTimeNs,
                frame.DecodeEndTimeNs,
                displayTimeNs,
                (uint)CurrentQueuedFrameCount(),
                MetalXRQuestProtocol.TimingFlagFrameDisplay));
        }

        private int CurrentQueuedFrameCount()
        {
            return _handshakeClient != null ? _handshakeClient.QueuedFrameCount : 0;
        }

        private static Color32 StreamColor(bool leftEye, byte luma)
        {
            if (leftEye)
            {
                return new Color32((byte)(luma / 3), (byte)((luma * 2) / 3), luma, 255);
            }

            return new Color32(luma, (byte)((luma * 2) / 3), (byte)(luma / 3), 255);
        }

        private static Color32 DiagnosticColor(bool leftEye, int x, int y, int frameIndex)
        {
            int band = (x * 6) / TextureWidth;
            int phase = (x + frameIndex * 13) & 255;
            bool grid = (x % 128) < 4 || (y % 96) < 4;
            bool movingLine = ((x + frameIndex * 19) % 256) < 10;
            bool centerCross = Math.Abs(x - TextureWidth / 2) < 5 || Math.Abs(y - TextureHeight / 2) < 5;

            byte r;
            byte g;
            byte b;

            if (leftEye)
            {
                r = (byte)(20 + band * 18);
                g = (byte)(80 + (phase / 3));
                b = (byte)(180 + band * 8);
            }
            else
            {
                r = (byte)(180 + band * 8);
                g = (byte)(70 + (255 - phase) / 4);
                b = (byte)(35 + band * 20);
            }

            if (grid)
            {
                r = 240;
                g = 240;
                b = 240;
            }

            if (movingLine)
            {
                r = leftEye ? (byte)80 : (byte)255;
                g = leftEye ? (byte)255 : (byte)180;
                b = leftEye ? (byte)220 : (byte)70;
            }

            if (centerCross)
            {
                r = 255;
                g = 255;
                b = 255;
            }

            return new Color32(r, g, b, 255);
        }

        private static void DestroyUnityObject(UnityEngine.Object unityObject)
        {
            if (unityObject == null)
            {
                return;
            }

            Destroy(unityObject);
        }
    }

    public sealed class MetalXRQuestDecodedVideoFrame
    {
        public MetalXRQuestDecodedVideoFrame(
            MetalXRQuestEncodedVideoFrame source,
            byte[] lumaBytes,
            int stride,
            bool decodedByMediaCodec)
        {
            FrameId = source.FrameId;
            IsLeftEye = source.IsLeftEye;
            Width = source.Width;
            Height = source.Height;
            HostCaptureTimeNs = source.TimestampNs;
            PredictedDisplayTimeNs = source.PredictedDisplayTimeNs;
            PacketTimestampNs = source.PacketTimestampNs;
            EncoderLatencyUs = source.EncoderLatencyUs;
            ReceiveTimeNs = source.ReceiveTimeNs;
            EncodedBytes = source.EncodedBytes.Length;
            LumaBytes = lumaBytes;
            Stride = stride;
            DecodedByMediaCodec = decodedByMediaCodec;
        }

        public ulong FrameId { get; }
        public bool IsLeftEye { get; }
        public int Width { get; }
        public int Height { get; }
        public ulong HostCaptureTimeNs { get; }
        public ulong PredictedDisplayTimeNs { get; }
        public ulong PacketTimestampNs { get; }
        public ulong EncoderLatencyUs { get; }
        public ulong ReceiveTimeNs { get; }
        public ulong DecodeStartTimeNs { get; private set; }
        public ulong DecodeEndTimeNs { get; private set; }
        public int EncodedBytes { get; }
        public byte[] LumaBytes { get; }
        public int Stride { get; }
        public bool DecodedByMediaCodec { get; }

        public void SetDecodeTiming(ulong decodeStartTimeNs, ulong decodeEndTimeNs)
        {
            DecodeStartTimeNs = decodeStartTimeNs;
            DecodeEndTimeNs = decodeEndTimeNs;
        }
    }

    public sealed class MetalXRQuestVideoDecoder : IDisposable
    {
        private readonly MetalXRQuestEyeDecoder _leftDecoder;
        private readonly MetalXRQuestEyeDecoder _rightDecoder;

        public MetalXRQuestVideoDecoder(Action<string> log)
        {
            _leftDecoder = new MetalXRQuestEyeDecoder(true, log);
            _rightDecoder = new MetalXRQuestEyeDecoder(false, log);
        }

        public bool TryDecode(MetalXRQuestEncodedVideoFrame frame, out MetalXRQuestDecodedVideoFrame decodedFrame)
        {
            decodedFrame = null;
            if (frame == null)
            {
                return false;
            }

            MetalXRQuestEyeDecoder decoder = frame.IsLeftEye ? _leftDecoder : _rightDecoder;
            return decoder.TryDecode(frame, out decodedFrame);
        }

        public void Dispose()
        {
            _leftDecoder.Dispose();
            _rightDecoder.Dispose();
        }
    }

    public sealed class MetalXRQuestEyeDecoder : IDisposable
    {
        private const int InfoTryAgainLater = -1;
        private const int InfoOutputFormatChanged = -2;
        private const long CodecTimeoutUs = 0;
        private readonly bool _leftEye;
        private readonly string _eyeName;
        private readonly Action<string> _log;
        private readonly Dictionary<long, MetalXRQuestEncodedVideoFrame> _pendingFrames = new Dictionary<long, MetalXRQuestEncodedVideoFrame>();
        private bool _fallbackLogged;

#if UNITY_ANDROID && !UNITY_EDITOR
        private AndroidJavaObject _codec;
        private AndroidJavaObject _bufferInfo;
        private int _configuredWidth;
        private int _configuredHeight;
        private int _outputStride;
        private bool _nearBlackOutputLogged;
#endif

        public MetalXRQuestEyeDecoder(bool leftEye, Action<string> log)
        {
            _leftEye = leftEye;
            _eyeName = leftEye ? "left" : "right";
            _log = log;
        }

        public bool TryDecode(MetalXRQuestEncodedVideoFrame frame, out MetalXRQuestDecodedVideoFrame decodedFrame)
        {
            decodedFrame = null;

#if UNITY_ANDROID && !UNITY_EDITOR
            if (TryDecodeWithMediaCodec(frame, out decodedFrame))
            {
                return true;
            }

            if (_codec != null)
            {
                return false;
            }
#endif

            decodedFrame = CreateCompressedPreview(frame);
            if (!_fallbackLogged)
            {
                _fallbackLogged = true;
                Log("using compressed payload preview for " + _eyeName + " eye until MediaCodec output is available");
            }

            return true;
        }

        public void Dispose()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            DisposeCodec();
#endif
        }

#if UNITY_ANDROID && !UNITY_EDITOR
        private bool TryDecodeWithMediaCodec(MetalXRQuestEncodedVideoFrame frame, out MetalXRQuestDecodedVideoFrame decodedFrame)
        {
            decodedFrame = null;
            if (!EnsureCodec(frame))
            {
                return false;
            }

            try
            {
                QueueInput(frame);
                return DrainOutput(out decodedFrame);
            }
            catch (Exception exception)
            {
                Log("MediaCodec failed for " + _eyeName + " eye: " + exception.Message);
                DisposeCodec();
                return false;
            }
        }

        private bool EnsureCodec(MetalXRQuestEncodedVideoFrame frame)
        {
            if (_codec != null && _configuredWidth == frame.Width && _configuredHeight == frame.Height)
            {
                return true;
            }

            DisposeCodec();

            try
            {
                using (AndroidJavaClass codecClass = new AndroidJavaClass("android.media.MediaCodec"))
                using (AndroidJavaClass formatClass = new AndroidJavaClass("android.media.MediaFormat"))
                {
                    _codec = codecClass.CallStatic<AndroidJavaObject>("createDecoderByType", "video/avc");
                    AndroidJavaObject format = formatClass.CallStatic<AndroidJavaObject>(
                        "createVideoFormat",
                        "video/avc",
                        frame.Width,
                        frame.Height);
                    format.Call("setInteger", "low-latency", 1);
                    _codec.Call("configure", format, null, null, 0);
                    format.Dispose();
                }

                _codec.Call("start");
                _bufferInfo = new AndroidJavaObject("android.media.MediaCodec$BufferInfo");
                _configuredWidth = frame.Width;
                _configuredHeight = frame.Height;
                _outputStride = frame.Width;
                Log("MediaCodec H.264 decoder started for " + _eyeName + " eye " + frame.Width + "x" + frame.Height);
                return true;
            }
            catch (Exception exception)
            {
                Log("MediaCodec could not start for " + _eyeName + " eye: " + exception.Message);
                DisposeCodec();
                return false;
            }
        }

        private void QueueInput(MetalXRQuestEncodedVideoFrame frame)
        {
            int inputIndex = _codec.Call<int>("dequeueInputBuffer", CodecTimeoutUs);
            if (inputIndex < 0)
            {
                return;
            }

            AndroidJavaObject inputBuffer = _codec.Call<AndroidJavaObject>("getInputBuffer", inputIndex);
            if (inputBuffer == null)
            {
                return;
            }

            int capacity = inputBuffer.Call<int>("capacity");
            if (frame.EncodedBytes.Length > capacity)
            {
                Log("dropping oversized encoded frame for " + _eyeName + " eye bytes=" + frame.EncodedBytes.Length + " capacity=" + capacity);
                inputBuffer.Dispose();
                return;
            }

            sbyte[] signedInput = ToSignedBytes(frame.EncodedBytes);
            inputBuffer.Call<AndroidJavaObject>("clear");
            inputBuffer.Call<AndroidJavaObject>("put", signedInput, 0, signedInput.Length);
            long presentationTimeUs = PresentationTimeUs(frame);
            _pendingFrames[presentationTimeUs] = frame;
            _codec.Call("queueInputBuffer", inputIndex, 0, frame.EncodedBytes.Length, presentationTimeUs, 0);
            inputBuffer.Dispose();
        }

        private bool DrainOutput(out MetalXRQuestDecodedVideoFrame decodedFrame)
        {
            decodedFrame = null;
            int outputIndex = _codec.Call<int>("dequeueOutputBuffer", _bufferInfo, CodecTimeoutUs);
            if (outputIndex == InfoTryAgainLater)
            {
                return false;
            }

            if (outputIndex == InfoOutputFormatChanged)
            {
                UpdateOutputFormat();
                return false;
            }

            if (outputIndex < 0)
            {
                return false;
            }

            int size = _bufferInfo.Get<int>("size");
            int offset = _bufferInfo.Get<int>("offset");
            long presentationTimeUs = _bufferInfo.Get<long>("presentationTimeUs");
            MetalXRQuestEncodedVideoFrame sourceFrame;
            bool hasSourceFrame = _pendingFrames.TryGetValue(presentationTimeUs, out sourceFrame);
            if (hasSourceFrame)
            {
                _pendingFrames.Remove(presentationTimeUs);
            }

            if (!hasSourceFrame)
            {
                _codec.Call("releaseOutputBuffer", outputIndex, false);
                return false;
            }

            byte[] bytes;
            int stride;
            bool readImage = TryReadOutputImage(outputIndex, sourceFrame, out bytes, out stride);
            if (!readImage)
            {
                bytes = ReadOutputBuffer(outputIndex, offset, size);
                stride = _outputStride >= sourceFrame.Width ? _outputStride : sourceFrame.Width;
            }

            _codec.Call("releaseOutputBuffer", outputIndex, false);

            int requiredBytes = ((sourceFrame.Height - 1) * stride) + sourceFrame.Width;
            if (bytes == null || bytes.Length < requiredBytes)
            {
                decodedFrame = CreateCompressedPreview(sourceFrame);
                return true;
            }

            if (!HasVisibleLuma(bytes, stride, sourceFrame.Width, sourceFrame.Height))
            {
                if (!_nearBlackOutputLogged)
                {
                    _nearBlackOutputLogged = true;
                    Log("MediaCodec output for " + _eyeName + " eye was near-black; using compressed payload preview");
                }

                decodedFrame = CreateCompressedPreview(sourceFrame);
                return true;
            }

            decodedFrame = new MetalXRQuestDecodedVideoFrame(sourceFrame, bytes, stride, true);
            return true;
        }

        private bool TryReadOutputImage(
            int outputIndex,
            MetalXRQuestEncodedVideoFrame sourceFrame,
            out byte[] lumaBytes,
            out int stride)
        {
            lumaBytes = null;
            stride = 0;

            AndroidJavaObject image = null;
            AndroidJavaObject lumaPlane = null;
            AndroidJavaObject buffer = null;
            try
            {
                image = _codec.Call<AndroidJavaObject>("getOutputImage", outputIndex);
                if (image == null)
                {
                    return false;
                }

                AndroidJavaObject[] planes = image.Call<AndroidJavaObject[]>("getPlanes");
                if (planes == null || planes.Length == 0 || planes[0] == null)
                {
                    return false;
                }

                lumaPlane = planes[0];
                buffer = lumaPlane.Call<AndroidJavaObject>("getBuffer");
                if (buffer == null)
                {
                    return false;
                }

                int rowStride = lumaPlane.Call<int>("getRowStride");
                int pixelStride = lumaPlane.Call<int>("getPixelStride");
                if (rowStride < sourceFrame.Width || pixelStride <= 0)
                {
                    return false;
                }

                int remaining = buffer.Call<int>("remaining");
                if (remaining <= 0)
                {
                    return false;
                }

                sbyte[] signedBytes = new sbyte[remaining];
                buffer.Call<AndroidJavaObject>("position", 0);
                buffer.Call<AndroidJavaObject>("get", signedBytes, 0, remaining);
                byte[] bytes = new byte[remaining];
                Buffer.BlockCopy(signedBytes, 0, bytes, 0, remaining);

                if (pixelStride == 1)
                {
                    lumaBytes = bytes;
                    stride = rowStride;
                    return true;
                }

                lumaBytes = new byte[sourceFrame.Width * sourceFrame.Height];
                stride = sourceFrame.Width;
                for (int y = 0; y < sourceFrame.Height; y++)
                {
                    int sourceRow = y * rowStride;
                    int targetRow = y * sourceFrame.Width;
                    for (int x = 0; x < sourceFrame.Width; x++)
                    {
                        int sourceIndex = sourceRow + (x * pixelStride);
                        if (sourceIndex < bytes.Length)
                        {
                            lumaBytes[targetRow + x] = bytes[sourceIndex];
                        }
                    }
                }

                return true;
            }
            catch (Exception exception)
            {
                Log("MediaCodec output image read failed for " + _eyeName + " eye: " + exception.Message);
                return false;
            }
            finally
            {
                if (buffer != null)
                {
                    buffer.Dispose();
                }

                if (lumaPlane != null)
                {
                    lumaPlane.Dispose();
                }

                if (image != null)
                {
                    try
                    {
                        image.Call("close");
                    }
                    catch (Exception)
                    {
                    }

                    image.Dispose();
                }
            }
        }

        private byte[] ReadOutputBuffer(int outputIndex, int offset, int size)
        {
            byte[] bytes = new byte[size];
            AndroidJavaObject outputBuffer = _codec.Call<AndroidJavaObject>("getOutputBuffer", outputIndex);
            if (outputBuffer == null || size <= 0)
            {
                return bytes;
            }

            sbyte[] signedBytes = new sbyte[size];
            outputBuffer.Call<AndroidJavaObject>("position", offset);
            outputBuffer.Call<AndroidJavaObject>("limit", offset + size);
            outputBuffer.Call<AndroidJavaObject>("get", signedBytes, 0, size);
            Buffer.BlockCopy(signedBytes, 0, bytes, 0, size);
            outputBuffer.Dispose();
            return bytes;
        }

        private static bool HasVisibleLuma(byte[] bytes, int stride, int width, int height)
        {
            const int SampleStep = 16;
            const int VisibleThreshold = 12;

            for (int y = 0; y < height; y += SampleStep)
            {
                int row = y * stride;
                for (int x = 0; x < width; x += SampleStep)
                {
                    int index = row + x;
                    if (index < bytes.Length && bytes[index] > VisibleThreshold)
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        private void UpdateOutputFormat()
        {
            AndroidJavaObject outputFormat = _codec.Call<AndroidJavaObject>("getOutputFormat");
            if (outputFormat == null)
            {
                return;
            }

            _outputStride = GetFormatInt(outputFormat, "stride", _configuredWidth);
            Log("MediaCodec output format for " + _eyeName + " eye stride=" + _outputStride);
            outputFormat.Dispose();
        }

        private static int GetFormatInt(AndroidJavaObject format, string key, int fallback)
        {
            bool containsKey = format.Call<bool>("containsKey", key);
            if (!containsKey)
            {
                return fallback;
            }

            return format.Call<int>("getInteger", key);
        }

        private void DisposeCodec()
        {
            _pendingFrames.Clear();

            if (_codec != null)
            {
                try
                {
                    _codec.Call("stop");
                }
                catch (Exception)
                {
                }

                try
                {
                    _codec.Call("release");
                }
                catch (Exception)
                {
                }

                _codec.Dispose();
                _codec = null;
            }

            if (_bufferInfo != null)
            {
                _bufferInfo.Dispose();
                _bufferInfo = null;
            }

            _configuredWidth = 0;
            _configuredHeight = 0;
            _outputStride = 0;
        }
#endif

        private static long PresentationTimeUs(MetalXRQuestEncodedVideoFrame frame)
        {
            long baseTimeUs = (long)(frame.FrameId * 1000ul);
            return frame.IsLeftEye ? baseTimeUs : baseTimeUs + 1L;
        }

        private static sbyte[] ToSignedBytes(byte[] bytes)
        {
            sbyte[] signedBytes = new sbyte[bytes.Length];
            Buffer.BlockCopy(bytes, 0, signedBytes, 0, bytes.Length);
            return signedBytes;
        }

        private MetalXRQuestDecodedVideoFrame CreateCompressedPreview(MetalXRQuestEncodedVideoFrame frame)
        {
            byte[] luma = new byte[frame.Width * frame.Height];
            byte[] encoded = frame.EncodedBytes;
            int encodedLength = encoded.Length > 0 ? encoded.Length : 1;
            for (int y = 0; y < frame.Height; y++)
            {
                for (int x = 0; x < frame.Width; x++)
                {
                    int sampleIndex = ((x * 31) + (y * 17) + (int)(frame.FrameId * 13ul)) % encodedLength;
                    byte value = encoded.Length > 0 ? encoded[sampleIndex] : (byte)0;
                    int mixed = value ^ (frame.IsLeftEye ? 0x33 : 0x99);
                    bool grid = (x % 96) < 4 || (y % 72) < 4;
                    bool movingLine = ((x + (int)(frame.FrameId * 11ul)) % 192) < 8;
                    int visible = grid || movingLine ? 240 : 96 + (mixed & 0x7f);
                    luma[(y * frame.Width) + x] = (byte)visible;
                }
            }

            return new MetalXRQuestDecodedVideoFrame(frame, luma, frame.Width, false);
        }

        private void Log(string message)
        {
            if (_log != null)
            {
                _log(message);
            }
        }
    }
}
