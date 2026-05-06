using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.Rendering;
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
        private const float InputSampleIntervalSeconds = 1.0f / 72.0f;
        private const float PresentationPanelDistance = 2.0f;
        private const float DefaultVerticalFovDegrees = 62.0f;
        private const float MinVerticalFovDegrees = 20.0f;
        private const float MaxVerticalFovDegrees = 140.0f;
        private const int MaxStreamFrameSetsPerUpdate = 4;
        private const int MaxDecodedFrameSets = 8;
        private const int MaxHapticCommandsPerUpdate = 4;
        private static readonly Vector3 DefaultPresentationPanelScale = new Vector3(2.2f, 1.2375f, 1.0f);

        private Texture2D _leftTexture;
        private Texture2D _rightTexture;
        private Texture2D _leftExternalTexture;
        private Texture2D _rightExternalTexture;
        private Material _leftMaterial;
        private Material _rightMaterial;
        private Mesh _quadMesh;
        private Camera _leftPresentationCamera;
        private Camera _rightPresentationCamera;
        private Transform _leftPresentationPanel;
        private Transform _rightPresentationPanel;
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
        private MetalXRQuestDecodedVideoFrame _lastFreshLeftFrame;
        private MetalXRQuestDecodedVideoFrame _lastFreshRightFrame;
        private bool _reusedDecodedFrameLogged;
        private readonly Dictionary<ulong, DecodedStereoFrameSet> _decodedFrameSets = new Dictionary<ulong, DecodedStereoFrameSet>();
        private ulong _displayedFrameSetId;
        private ulong _displayedStereoFrameSets;

        private void Start()
        {
            Application.targetFrameRate = 72;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;

            CreatePresentationRig();
            UpdateDiagnosticTexture(true, 0);
            UpdateDiagnosticTexture(false, 0);

            MetalXRQuestEndpoint endpoint = MetalXRQuestEndpoint.FromCommandLine();
            MetalXRQuestDeviceProfile deviceProfile = CreateDeviceProfile();
            bool experimentalSurfaceDecode = ExperimentalSurfaceDecodeEnabled();
            bool surfaceDecodeGraphicsSupported = SurfaceDecodeGraphicsSupported();
            bool experimentalSurfacePresent = experimentalSurfaceDecode && ExperimentalSurfacePresentEnabled();
            if (experimentalSurfaceDecode && !surfaceDecodeGraphicsSupported)
            {
                Debug.Log(
                    LogPrefix +
                    " experimental Surface decode requires an OpenGLES graphics device; current_graphics=" +
                    SystemInfo.graphicsDeviceType +
                    "; falling back to MediaCodec Image-plane decode");
                experimentalSurfaceDecode = false;
                experimentalSurfacePresent = false;
            }

            _videoDecoder = new MetalXRQuestVideoDecoder(
                LogClientMessage,
                experimentalSurfaceDecode,
                experimentalSurfacePresent);
            _handshakeClient = new MetalXRQuestHandshakeClient(endpoint, deviceProfile);
            _handshakeClient.Start();

            Debug.Log(
                LogPrefix + " started; stream client active with diagnostic fallback; host=" +
                endpoint.Host + ":" + endpoint.Port +
                " graphics=" + SystemInfo.graphicsDeviceType +
                " experimental_surface_decode=" + experimentalSurfaceDecode +
                " experimental_surface_present=" + experimentalSurfacePresent);
        }

        private void Update()
        {
            if (_handshakeClient != null)
            {
                _handshakeClient.DrainLogs(LogClientMessage);
                _handshakeClient.DrainFrameSets(HandleEncodedFrameSet, MaxStreamFrameSetsPerUpdate);
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
            DestroyUnityObject(_leftExternalTexture);
            DestroyUnityObject(_rightExternalTexture);
            DestroyUnityObject(_quadMesh);
        }

        private static MetalXRQuestDeviceProfile CreateDeviceProfile()
        {
            int eyeTextureWidth = XRSettings.eyeTextureWidth;
            int eyeTextureHeight = XRSettings.eyeTextureHeight;
            if (eyeTextureWidth <= 0)
            {
                eyeTextureWidth = (int)MetalXRQuestDeviceProfile.Default.MaxVideoWidth;
            }
            if (eyeTextureHeight <= 0)
            {
                eyeTextureHeight = (int)MetalXRQuestDeviceProfile.Default.MaxVideoHeight;
            }

            float refreshRate = 0.0f;
            List<XRDisplaySubsystem> displays = new List<XRDisplaySubsystem>();
            SubsystemManager.GetSubsystems(displays);
            for (int i = 0; i < displays.Count; i++)
            {
                XRDisplaySubsystem display = displays[i];
                if (display != null && display.running && display.TryGetDisplayRefreshRate(out refreshRate))
                {
                    break;
                }
            }

            int preferredFps = refreshRate > 0.0f ?
                Mathf.RoundToInt(refreshRate) :
                (int)MetalXRQuestDeviceProfile.Default.PreferredFps;

            string deviceName =
                string.IsNullOrEmpty(SystemInfo.deviceModel) ?
                    MetalXRQuestDeviceProfile.Default.DeviceName :
                    SystemInfo.deviceModel;
            return new MetalXRQuestDeviceProfile(
                (uint)Mathf.Max(1, eyeTextureWidth),
                (uint)Mathf.Max(1, eyeTextureHeight),
                (uint)Mathf.Max(1, preferredFps),
                deviceName);
        }

        private static bool ExperimentalSurfaceDecodeEnabled()
        {
            return ReadExperimentalBooleanFlag(
                "--metalxr-surface-decode",
                "metalxr_surface_decode",
                false);
        }

        private static bool ExperimentalSurfacePresentEnabled()
        {
            return ReadExperimentalBooleanFlag(
                "--metalxr-surface-present",
                "metalxr_surface_present",
                false);
        }

        private static bool SurfaceDecodeGraphicsSupported()
        {
            return SystemInfo.graphicsDeviceType == GraphicsDeviceType.OpenGLES3 ||
                   SystemInfo.graphicsDeviceType == GraphicsDeviceType.OpenGLES2;
        }

        private static bool ReadExperimentalBooleanFlag(string commandLineFlag, string intentExtraName, bool defaultValue)
        {
            bool enabled = defaultValue;
            string[] args = Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length; i++)
            {
                string arg = args[i];
                if (string.Equals(arg, commandLineFlag, StringComparison.Ordinal))
                {
                    enabled = true;
                    continue;
                }

                string prefix = commandLineFlag + "=";
                if (arg.StartsWith(prefix, StringComparison.Ordinal))
                {
                    string value = arg.Substring(prefix.Length);
                    enabled = value == "1" ||
                              string.Equals(value, "true", StringComparison.OrdinalIgnoreCase) ||
                              string.Equals(value, "yes", StringComparison.OrdinalIgnoreCase);
                }
            }

#if UNITY_ANDROID && !UNITY_EDITOR
            try
            {
                using (AndroidJavaClass unityPlayer = new AndroidJavaClass("com.unity3d.player.UnityPlayer"))
                {
                    using (AndroidJavaObject activity = unityPlayer.GetStatic<AndroidJavaObject>("currentActivity"))
                    {
                        if (activity == null)
                        {
                            return enabled;
                        }

                        using (AndroidJavaObject intent = activity.Call<AndroidJavaObject>("getIntent"))
                        {
                            if (intent == null)
                            {
                                return enabled;
                            }

                            enabled = intent.Call<bool>("getBooleanExtra", intentExtraName, enabled);
                        }
                    }
                }
            }
            catch (Exception exception)
            {
                Debug.Log(LogPrefix + " could not read Android intent extra " + intentExtraName + ": " + exception.Message);
            }
#endif

            return enabled;
        }

        private static void LogClientMessage(string message)
        {
            Debug.Log(LogPrefix + " " + message);
        }

        private void HandleEncodedFrameSet(MetalXRQuestEncodedStereoFrameSet frameSet)
        {
            if (frameSet == null || !frameSet.IsComplete)
            {
                return;
            }

            MetalXRQuestDecodedVideoFrame leftFrame;
            MetalXRQuestDecodedVideoFrame rightFrame;
            if (TryDecodeFrame(frameSet.Left, out leftFrame))
            {
                StoreDecodedFrame(leftFrame);
            }

            if (TryDecodeFrame(frameSet.Right, out rightFrame))
            {
                StoreDecodedFrame(rightFrame);
            }

            DisplayLatestCompleteDecodedFrameSet();
        }

        private bool TryDecodeFrame(MetalXRQuestEncodedVideoFrame encodedFrame, out MetalXRQuestDecodedVideoFrame decodedFrame)
        {
            decodedFrame = null;
            if (_videoDecoder == null)
            {
                return false;
            }

            ulong decodeStartTimeNs = MetalXRQuestProtocol.NowNs();
            if (_videoDecoder.TryDecode(encodedFrame, out decodedFrame) && decodedFrame != null)
            {
                ulong decodeEndTimeNs = MetalXRQuestProtocol.NowNs();
                if (ShouldReuseFallbackFrame(decodedFrame))
                {
                    MetalXRQuestDecodedVideoFrame reusedFrame = CreateReusedDecodedFrame(encodedFrame);
                    if (reusedFrame != null)
                    {
                        decodedFrame = reusedFrame;
                    }
                }

                decodedFrame.SetDecodeTiming(decodeStartTimeNs, decodeEndTimeNs);
                if (IsFreshMediaCodecFrame(decodedFrame))
                {
                    RememberFreshMediaCodecFrame(decodedFrame);
                }

                return true;
            }

            return false;
        }

        private void StoreDecodedFrame(MetalXRQuestDecodedVideoFrame decodedFrame)
        {
            if (decodedFrame == null)
            {
                return;
            }

            DecodedStereoFrameSet frameSet;
            if (!_decodedFrameSets.TryGetValue(decodedFrame.FrameId, out frameSet))
            {
                frameSet = new DecodedStereoFrameSet(decodedFrame.FrameId);
                _decodedFrameSets[decodedFrame.FrameId] = frameSet;
            }

            if (decodedFrame.IsLeftEye)
            {
                frameSet.Left = decodedFrame;
            }
            else
            {
                frameSet.Right = decodedFrame;
            }

            DropStaleDecodedFrameSets();
        }

        private void DisplayLatestCompleteDecodedFrameSet()
        {
            DecodedStereoFrameSet latestComplete = null;
            foreach (DecodedStereoFrameSet frameSet in _decodedFrameSets.Values)
            {
                if (!frameSet.IsComplete || frameSet.FrameId <= _displayedFrameSetId)
                {
                    continue;
                }

                if (latestComplete == null || frameSet.FrameId > latestComplete.FrameId)
                {
                    latestComplete = frameSet;
                }
            }

            if (latestComplete == null)
            {
                return;
            }

            bool updatedLeft = UpdateStreamTexture(latestComplete.Left);
            bool updatedRight = UpdateStreamTexture(latestComplete.Right);
            if (updatedLeft && updatedRight)
            {
                ApplyPresentationMetadata(latestComplete.Left, latestComplete.Right);
                _displayedFrameSetId = latestComplete.FrameId;
                _displayedStereoFrameSets++;
                if (_displayedStereoFrameSets <= 4 || (_displayedStereoFrameSets % 120) == 0)
                {
                    LogClientMessage(
                        "displayed stereo FrameSet frame=" + latestComplete.FrameId +
                        " left_decoder=" + latestComplete.Left.DecoderMode +
                        " right_decoder=" + latestComplete.Right.DecoderMode);
                }
            }

            RemoveDecodedFrameSetsAtOrBefore(latestComplete.FrameId);
        }

        private void DropStaleDecodedFrameSets()
        {
            while (_decodedFrameSets.Count > MaxDecodedFrameSets)
            {
                ulong oldestFrameId = ulong.MaxValue;
                foreach (ulong frameId in _decodedFrameSets.Keys)
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

                _decodedFrameSets.Remove(oldestFrameId);
            }
        }

        private void RemoveDecodedFrameSetsAtOrBefore(ulong frameId)
        {
            List<ulong> frameIdsToRemove = new List<ulong>();
            foreach (ulong pendingFrameId in _decodedFrameSets.Keys)
            {
                if (pendingFrameId <= frameId)
                {
                    frameIdsToRemove.Add(pendingFrameId);
                }
            }

            for (int i = 0; i < frameIdsToRemove.Count; i++)
            {
                _decodedFrameSets.Remove(frameIdsToRemove[i]);
            }
        }

        private static bool IsFreshMediaCodecFrame(MetalXRQuestDecodedVideoFrame frame)
        {
            return frame != null && frame.DecodedByMediaCodec && frame.DecoderMode == "mediacodec-yuv420";
        }

        private static bool ShouldReuseFallbackFrame(MetalXRQuestDecodedVideoFrame frame)
        {
            return frame != null && !frame.UsesExternalTexture && !IsFreshMediaCodecFrame(frame);
        }

        private void RememberFreshMediaCodecFrame(MetalXRQuestDecodedVideoFrame frame)
        {
            if (frame == null)
            {
                return;
            }

            if (frame.IsLeftEye)
            {
                _lastFreshLeftFrame = frame;
            }
            else
            {
                _lastFreshRightFrame = frame;
            }
        }

        private MetalXRQuestDecodedVideoFrame CreateReusedDecodedFrame(MetalXRQuestEncodedVideoFrame encodedFrame)
        {
            if (encodedFrame == null)
            {
                return null;
            }

            MetalXRQuestDecodedVideoFrame source = encodedFrame.IsLeftEye ? _lastFreshLeftFrame : _lastFreshRightFrame;
            source = IsCompatibleReuseFrame(source, encodedFrame) ? source : null;

            if (source == null)
            {
                return null;
            }

            if (!_reusedDecodedFrameLogged)
            {
                _reusedDecodedFrameLogged = true;
                string sourceEye = source.IsLeftEye ? "left" : "right";
                LogClientMessage("reusing last MediaCodec " + sourceEye + " eye frame when preview fallback would be shown");
            }

            string reuseMode = source.IsLeftEye ? "mediacodec-reused-left" : "mediacodec-reused-right";
            return new MetalXRQuestDecodedVideoFrame(
                encodedFrame,
                source.RgbaBytes,
                source.RgbaStrideBytes,
                false,
                reuseMode,
                source.OutputFormat);
        }

        private static bool IsCompatibleReuseFrame(MetalXRQuestDecodedVideoFrame frame, MetalXRQuestEncodedVideoFrame encodedFrame)
        {
            return frame != null &&
                   encodedFrame != null &&
                   frame.RgbaBytes != null &&
                   frame.Width == encodedFrame.Width &&
                   frame.Height == encodedFrame.Height &&
                   frame.RgbaStrideBytes >= encodedFrame.Width * 4;
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
                0,
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

        private void CreatePresentationRig()
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

            _leftPresentationPanel = CreateEyePanel(rigRoot.transform, "MetalXR Left Presentation Panel", EyeLayerLeft, _leftMaterial);
            _rightPresentationPanel = CreateEyePanel(rigRoot.transform, "MetalXR Right Presentation Panel", EyeLayerRight, _rightMaterial);
            _leftPresentationCamera = CreateEyeCamera(rigRoot.transform, "MetalXR Left Presentation Camera", EyeLayerLeft, StereoTargetEyeMask.Left);
            _rightPresentationCamera = CreateEyeCamera(rigRoot.transform, "MetalXR Right Presentation Camera", EyeLayerRight, StereoTargetEyeMask.Right);
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
            SetMaterialTexture(material, texture);

            if (material.HasProperty("_BaseColor"))
            {
                material.SetColor("_BaseColor", Color.white);
            }

            return material;
        }

        private static void SetMaterialTexture(Material material, Texture texture)
        {
            if (material == null || texture == null)
            {
                return;
            }

            if (material.HasProperty("_BaseMap"))
            {
                material.SetTexture("_BaseMap", texture);
            }
            else
            {
                material.mainTexture = texture;
            }
        }

        private GameObject CreateRigRoot()
        {
            GameObject root = new GameObject("MetalXR Presentation Rig");
            root.transform.SetParent(transform, false);
            return root;
        }

        private static Camera CreateEyeCamera(
            Transform parent,
            string name,
            int layer,
            StereoTargetEyeMask stereoTargetEye)
        {
            GameObject cameraObject = new GameObject(name);
            cameraObject.transform.SetParent(parent, false);
            cameraObject.transform.localPosition = Vector3.zero;
            cameraObject.transform.localRotation = Quaternion.identity;
            Camera camera = cameraObject.AddComponent<Camera>();
            camera.clearFlags = CameraClearFlags.SolidColor;
            camera.backgroundColor = new Color(0.015f, 0.017f, 0.02f, 1.0f);
            camera.nearClipPlane = 0.01f;
            camera.farClipPlane = 20.0f;
            camera.fieldOfView = 62.0f;
            camera.depth = 10.0f;
            camera.cullingMask = 1 << layer;
            camera.stereoTargetEye = stereoTargetEye;
            camera.useOcclusionCulling = false;
            camera.allowHDR = false;
            camera.allowMSAA = true;
            return camera;
        }

        private Transform CreateEyePanel(Transform parent, string name, int layer, Material material)
        {
            GameObject panel = new GameObject(name);
            panel.name = name;
            panel.layer = layer;
            panel.transform.SetParent(parent, false);
            panel.transform.localPosition = new Vector3(0.0f, 0.0f, PresentationPanelDistance);
            panel.transform.localRotation = Quaternion.identity;
            panel.transform.localScale = DefaultPresentationPanelScale;

            MeshFilter meshFilter = panel.AddComponent<MeshFilter>();
            meshFilter.sharedMesh = _quadMesh;

            MeshRenderer meshRenderer = panel.AddComponent<MeshRenderer>();
            meshRenderer.sharedMaterial = material;
            return panel.transform;
        }

        private void ApplyPresentationMetadata(MetalXRQuestDecodedVideoFrame leftFrame, MetalXRQuestDecodedVideoFrame rightFrame)
        {
            ApplyEyePresentationMetadata(_leftPresentationCamera, _leftPresentationPanel, leftFrame);
            ApplyEyePresentationMetadata(_rightPresentationCamera, _rightPresentationPanel, rightFrame);
        }

        private static void ApplyEyePresentationMetadata(Camera camera, Transform panel, MetalXRQuestDecodedVideoFrame frame)
        {
            if (camera == null || panel == null || frame == null)
            {
                return;
            }

            float angleLeft;
            float angleRight;
            float angleUp;
            float angleDown;
            if (!TryReadFov(frame.Fov, out angleLeft, out angleRight, out angleUp, out angleDown))
            {
                camera.fieldOfView = DefaultVerticalFovDegrees;
                camera.aspect = DefaultPresentationPanelScale.x / DefaultPresentationPanelScale.y;
                panel.localPosition = new Vector3(0.0f, 0.0f, PresentationPanelDistance);
                panel.localScale = DefaultPresentationPanelScale;
                return;
            }

            float leftTangent = Mathf.Tan(angleLeft);
            float rightTangent = Mathf.Tan(angleRight);
            float upTangent = Mathf.Tan(angleUp);
            float downTangent = Mathf.Tan(angleDown);
            float width = Mathf.Max(0.1f, PresentationPanelDistance * (rightTangent - leftTangent));
            float height = Mathf.Max(0.1f, PresentationPanelDistance * (upTangent - downTangent));
            float centerX = PresentationPanelDistance * (rightTangent + leftTangent) * 0.5f;
            float centerY = PresentationPanelDistance * (upTangent + downTangent) * 0.5f;
            float verticalFovDegrees = Mathf.Clamp(
                (angleUp - angleDown) * Mathf.Rad2Deg,
                MinVerticalFovDegrees,
                MaxVerticalFovDegrees);

            camera.fieldOfView = verticalFovDegrees;
            camera.aspect = width / height;
            panel.localPosition = new Vector3(centerX, centerY, PresentationPanelDistance);
            panel.localScale = new Vector3(width, height, 1.0f);
        }

        private static bool TryReadFov(
            float[] fov,
            out float angleLeft,
            out float angleRight,
            out float angleUp,
            out float angleDown)
        {
            angleLeft = 0.0f;
            angleRight = 0.0f;
            angleUp = 0.0f;
            angleDown = 0.0f;

            if (fov == null || fov.Length < 4)
            {
                return false;
            }

            angleLeft = fov[0];
            angleRight = fov[1];
            angleUp = fov[2];
            angleDown = fov[3];
            if (angleLeft >= 0.0f || angleRight <= 0.0f || angleUp <= 0.0f || angleDown >= 0.0f)
            {
                return false;
            }

            float horizontalFov = angleRight - angleLeft;
            float verticalFov = angleUp - angleDown;
            return horizontalFov > 0.1f && verticalFov > 0.1f;
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

        private bool UpdateStreamTexture(MetalXRQuestDecodedVideoFrame frame)
        {
            if (frame == null)
            {
                return false;
            }

            bool updated = frame.UsesExternalTexture ?
                UpdateExternalStreamTexture(frame) :
                UpdateCpuStreamTexture(frame);
            if (!updated)
            {
                return false;
            }

            ulong displayTimeNs = MetalXRQuestProtocol.NowNs();
            MarkFrameDisplayed(frame, displayTimeNs);
            return true;
        }

        private bool UpdateExternalStreamTexture(MetalXRQuestDecodedVideoFrame frame)
        {
            if (frame.ExternalTexturePtr == IntPtr.Zero || frame.Width <= 0 || frame.Height <= 0)
            {
                return false;
            }

            Texture2D texture = frame.IsLeftEye ? _leftExternalTexture : _rightExternalTexture;
            if (texture == null || texture.width != frame.Width || texture.height != frame.Height)
            {
                texture = Texture2D.CreateExternalTexture(
                    frame.Width,
                    frame.Height,
                    TextureFormat.RGBA32,
                    false,
                    false,
                    frame.ExternalTexturePtr);
                texture.name = frame.IsLeftEye ?
                    "MetalXR Left SurfaceTexture External" :
                    "MetalXR Right SurfaceTexture External";
                texture.wrapMode = TextureWrapMode.Clamp;
                texture.filterMode = FilterMode.Bilinear;

                if (frame.IsLeftEye)
                {
                    DestroyUnityObject(_leftExternalTexture);
                    _leftExternalTexture = texture;
                }
                else
                {
                    DestroyUnityObject(_rightExternalTexture);
                    _rightExternalTexture = texture;
                }
            }
            else
            {
                texture.UpdateExternalTexture(frame.ExternalTexturePtr);
            }

            SetEyeMaterialTexture(frame.IsLeftEye, texture);
            return true;
        }

        private bool UpdateCpuStreamTexture(MetalXRQuestDecodedVideoFrame frame)
        {
            Color32[] pixels = frame.IsLeftEye ? _leftPixels : _rightPixels;
            Texture2D texture = frame.IsLeftEye ? _leftTexture : _rightTexture;
            if (pixels == null || texture == null || frame.RgbaBytes == null)
            {
                return false;
            }

            int sourceWidth = frame.Width > 0 ? frame.Width : 1;
            int sourceHeight = frame.Height > 0 ? frame.Height : 1;
            int sourceRowBytes = sourceWidth * 4;
            int strideBytes = frame.RgbaStrideBytes >= sourceRowBytes ? frame.RgbaStrideBytes : sourceRowBytes;
            int requiredBytes = ((sourceHeight - 1) * strideBytes) + sourceRowBytes;
            if (frame.RgbaBytes.Length < requiredBytes)
            {
                return false;
            }

            for (int y = 0; y < TextureHeight; y++)
            {
                int sourceY = sourceHeight - 1 - ((y * sourceHeight) / TextureHeight);
                int sourceRow = sourceY * strideBytes;
                for (int x = 0; x < TextureWidth; x++)
                {
                    int sourceX = (x * sourceWidth) / TextureWidth;
                    int sourceIndex = sourceRow + (sourceX * 4);
                    pixels[y * TextureWidth + x] = new Color32(
                        frame.RgbaBytes[sourceIndex],
                        frame.RgbaBytes[sourceIndex + 1],
                        frame.RgbaBytes[sourceIndex + 2],
                        frame.RgbaBytes[sourceIndex + 3]);
                }
            }

            texture.SetPixels32(pixels);
            texture.Apply(false);
            SetEyeMaterialTexture(frame.IsLeftEye, texture);
            return true;
        }

        private void SetEyeMaterialTexture(bool leftEye, Texture texture)
        {
            Material material = leftEye ? _leftMaterial : _rightMaterial;
            SetMaterialTexture(material, texture);
        }

        private void MarkFrameDisplayed(MetalXRQuestDecodedVideoFrame frame, ulong displayTimeNs)
        {
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
                string eyeName = frame.IsLeftEye ? "left" : "right";
                Debug.Log(
                    LogPrefix +
                    " displayed VIDEO_FRAME frame=" + frame.FrameId +
                    " eye=" + eyeName +
                    " decoder=" + frame.DecoderMode +
                    " output_format=" + frame.OutputFormat +
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

        private sealed class DecodedStereoFrameSet
        {
            public DecodedStereoFrameSet(ulong frameId)
            {
                FrameId = frameId;
            }

            public ulong FrameId { get; private set; }
            public MetalXRQuestDecodedVideoFrame Left { get; set; }
            public MetalXRQuestDecodedVideoFrame Right { get; set; }
            public bool IsComplete { get { return Left != null && Right != null; } }
        }
    }

    public sealed class MetalXRQuestDecodedVideoFrame
    {
        public MetalXRQuestDecodedVideoFrame(
            MetalXRQuestEncodedVideoFrame source,
            byte[] rgbaBytes,
            int rgbaStrideBytes,
            bool decodedByMediaCodec,
            string decoderMode,
            string outputFormat)
            : this(
                source,
                rgbaBytes,
                rgbaStrideBytes,
                decodedByMediaCodec,
                decoderMode,
                outputFormat,
                IntPtr.Zero)
        {
        }

        public MetalXRQuestDecodedVideoFrame(
            MetalXRQuestEncodedVideoFrame source,
            byte[] rgbaBytes,
            int rgbaStrideBytes,
            bool decodedByMediaCodec,
            string decoderMode,
            string outputFormat,
            IntPtr externalTexturePtr)
        {
            FrameId = source.FrameId;
            IsLeftEye = source.IsLeftEye;
            Width = source.Width;
            Height = source.Height;
            HostCaptureTimeNs = source.TimestampNs;
            PredictedDisplayTimeNs = source.PredictedDisplayTimeNs;
            PacketTimestampNs = source.PacketTimestampNs;
            EncoderLatencyUs = source.EncoderLatencyUs;
            ImageRectX = source.ImageRectX;
            ImageRectY = source.ImageRectY;
            ImageRectWidth = source.ImageRectWidth;
            ImageRectHeight = source.ImageRectHeight;
            ImageArrayIndex = source.ImageArrayIndex;
            ProjectionFlags = source.ProjectionFlags;
            ReferenceSpaceId = source.ReferenceSpaceId;
            PosePosition = source.PosePosition;
            PoseOrientation = source.PoseOrientation;
            Fov = source.Fov;
            ReceiveTimeNs = source.ReceiveTimeNs;
            EncodedBytes = source.EncodedBytes.Length;
            RgbaBytes = rgbaBytes;
            RgbaStrideBytes = rgbaStrideBytes;
            DecodedByMediaCodec = decodedByMediaCodec;
            DecoderMode = string.IsNullOrEmpty(decoderMode) ? "unknown" : decoderMode;
            OutputFormat = string.IsNullOrEmpty(outputFormat) ? "unknown" : outputFormat;
            ExternalTexturePtr = externalTexturePtr;
        }

        public ulong FrameId { get; }
        public bool IsLeftEye { get; }
        public int Width { get; }
        public int Height { get; }
        public ulong HostCaptureTimeNs { get; }
        public ulong PredictedDisplayTimeNs { get; }
        public ulong PacketTimestampNs { get; }
        public ulong EncoderLatencyUs { get; }
        public int ImageRectX { get; }
        public int ImageRectY { get; }
        public uint ImageRectWidth { get; }
        public uint ImageRectHeight { get; }
        public uint ImageArrayIndex { get; }
        public uint ProjectionFlags { get; }
        public ulong ReferenceSpaceId { get; }
        public float[] PosePosition { get; }
        public float[] PoseOrientation { get; }
        public float[] Fov { get; }
        public ulong ReceiveTimeNs { get; }
        public ulong DecodeStartTimeNs { get; private set; }
        public ulong DecodeEndTimeNs { get; private set; }
        public int EncodedBytes { get; }
        public byte[] RgbaBytes { get; }
        public int RgbaStrideBytes { get; }
        public bool DecodedByMediaCodec { get; }
        public string DecoderMode { get; }
        public string OutputFormat { get; }
        public IntPtr ExternalTexturePtr { get; }
        public bool UsesExternalTexture { get { return ExternalTexturePtr != IntPtr.Zero; } }

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

        public MetalXRQuestVideoDecoder(
            Action<string> log,
            bool enableExperimentalSurfaceDecode,
            bool enableExperimentalSurfacePresent)
        {
            _leftDecoder = new MetalXRQuestEyeDecoder(
                true,
                log,
                enableExperimentalSurfaceDecode,
                enableExperimentalSurfacePresent);
            _rightDecoder = new MetalXRQuestEyeDecoder(
                false,
                log,
                enableExperimentalSurfaceDecode,
                enableExperimentalSurfacePresent);
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
        private readonly bool _enableExperimentalSurfaceDecode;
        private readonly bool _enableExperimentalSurfacePresent;
        private readonly Dictionary<long, MetalXRQuestEncodedVideoFrame> _pendingFrames = new Dictionary<long, MetalXRQuestEncodedVideoFrame>();
        private bool _fallbackLogged;

#if UNITY_ANDROID && !UNITY_EDITOR
        private struct ImagePlaneData
        {
            public byte[] Bytes;
            public int RowStride;
            public int PixelStride;
        }

        private AndroidJavaObject _codec;
        private AndroidJavaObject _bufferInfo;
        private int _configuredWidth;
        private int _configuredHeight;
        private int _outputStride;
        private int _outputSliceHeight;
        private int _outputColorFormat;
        private bool _nearBlackOutputLogged;
        private bool _outputImageFallbackLogged;
        private bool _zeroPlaneOutputLogged;
        private AndroidJavaObject _surfaceDecoder;
        private int _surfaceConfiguredWidth;
        private int _surfaceConfiguredHeight;
        private int _surfaceTextureName;
        private bool _surfaceDecoderLogged;
        private bool _surfaceDecoderFallbackLogged;
        private bool _surfaceDecoderUnavailable;
        private readonly Dictionary<long, MetalXRQuestEncodedVideoFrame> _surfacePendingFrames = new Dictionary<long, MetalXRQuestEncodedVideoFrame>();
#endif

        public MetalXRQuestEyeDecoder(
            bool leftEye,
            Action<string> log,
            bool enableExperimentalSurfaceDecode,
            bool enableExperimentalSurfacePresent)
        {
            _leftEye = leftEye;
            _eyeName = leftEye ? "left" : "right";
            _log = log;
            _enableExperimentalSurfaceDecode = enableExperimentalSurfaceDecode;
            _enableExperimentalSurfacePresent = enableExperimentalSurfacePresent;
        }

        public bool TryDecode(MetalXRQuestEncodedVideoFrame frame, out MetalXRQuestDecodedVideoFrame decodedFrame)
        {
            decodedFrame = null;

#if UNITY_ANDROID && !UNITY_EDITOR
            if (_enableExperimentalSurfaceDecode &&
                TryDecodeWithSurfaceDecoder(frame, out decodedFrame))
            {
                return true;
            }

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
            DisposeSurfaceDecoder();
            DisposeCodec();
#endif
        }

#if UNITY_ANDROID && !UNITY_EDITOR
        private bool TryDecodeWithSurfaceDecoder(MetalXRQuestEncodedVideoFrame frame, out MetalXRQuestDecodedVideoFrame decodedFrame)
        {
            decodedFrame = null;
            if (!EnsureSurfaceDecoder(frame))
            {
                return false;
            }

            try
            {
                long presentationTimeUs = PresentationTimeUs(frame);
                sbyte[] signedInput = ToSignedBytes(frame.EncodedBytes);
                bool queued = _surfaceDecoder.Call<bool>("queueFrame", signedInput, presentationTimeUs);
                if (!queued)
                {
                    LogSurfaceDecoderErrorOnce("Surface decoder could not queue input for " + _eyeName + " eye");
                    return false;
                }

                _surfacePendingFrames[presentationTimeUs] = frame;
                long decodedPresentationTimeUs = _surfaceDecoder.Call<long>("drainOne", CodecTimeoutUs);
                if (decodedPresentationTimeUs == -2L)
                {
                    decodedPresentationTimeUs = _surfaceDecoder.Call<long>("drainOne", CodecTimeoutUs);
                }

                if (decodedPresentationTimeUs < 0L)
                {
                    return false;
                }

                MetalXRQuestEncodedVideoFrame sourceFrame;
                bool hasSourceFrame = _surfacePendingFrames.TryGetValue(decodedPresentationTimeUs, out sourceFrame);
                if (!hasSourceFrame)
                {
                    return false;
                }

                _surfacePendingFrames.Remove(decodedPresentationTimeUs);
                bool textureUpdated = _surfaceDecoder.Call<bool>("updateTexImage");
                string outputFormat =
                    "external-oes-texture=" + _surfaceTextureName +
                    ";updated=" + textureUpdated;
                if (_enableExperimentalSurfacePresent && textureUpdated)
                {
                    decodedFrame = new MetalXRQuestDecodedVideoFrame(
                        sourceFrame,
                        null,
                        0,
                        true,
                        "surface-external-texture",
                        outputFormat,
                        new IntPtr(_surfaceTextureName));
                    return true;
                }

                decodedFrame = CreateCompressedPreview(
                    sourceFrame,
                    "surface-experimental-preview",
                    outputFormat);
                return true;
            }
            catch (Exception exception)
            {
                Log("experimental Surface decoder failed for " + _eyeName + " eye: " + exception.Message);
                DisposeSurfaceDecoder();
                return false;
            }
        }

        private bool EnsureSurfaceDecoder(MetalXRQuestEncodedVideoFrame frame)
        {
            if (_surfaceDecoderUnavailable)
            {
                return false;
            }

            if (_surfaceDecoder != null &&
                _surfaceConfiguredWidth == frame.Width &&
                _surfaceConfiguredHeight == frame.Height)
            {
                return true;
            }

            DisposeSurfaceDecoder();

            try
            {
                _surfaceDecoder = new AndroidJavaObject("com.metalxr.questclient.MetalXRSurfaceDecoder", _eyeName);
                _surfaceTextureName = _surfaceDecoder.Call<int>("createExternalTexture");
                if (_surfaceTextureName == 0)
                {
                    LogSurfaceDecoderErrorOnce("experimental Surface decoder could not create an external texture for " + _eyeName + " eye");
                    _surfaceDecoderUnavailable = true;
                    DisposeSurfaceDecoder();
                    return false;
                }

                bool configured = _surfaceDecoder.Call<bool>("configure", frame.Width, frame.Height, _surfaceTextureName);
                if (!configured)
                {
                    LogSurfaceDecoderErrorOnce("experimental Surface decoder could not configure for " + _eyeName + " eye");
                    _surfaceDecoderUnavailable = true;
                    DisposeSurfaceDecoder();
                    return false;
                }

                _surfaceConfiguredWidth = frame.Width;
                _surfaceConfiguredHeight = frame.Height;
                _surfaceDecoderUnavailable = false;
                _surfaceDecoderFallbackLogged = false;
                if (!_surfaceDecoderLogged)
                {
                    _surfaceDecoderLogged = true;
                    Log(
                        "experimental Surface decoder initialized for " + _eyeName +
                        " eye " + frame.Width + "x" + frame.Height +
                        " texture=" + _surfaceTextureName +
                        "; native texture presentation is not enabled yet");
                }

                return true;
            }
            catch (Exception exception)
            {
                Log("experimental Surface decoder could not start for " + _eyeName + " eye: " + exception.Message);
                _surfaceDecoderUnavailable = true;
                DisposeSurfaceDecoder();
                return false;
            }
        }

        private void LogSurfaceDecoderErrorOnce(string fallbackMessage)
        {
            if (_surfaceDecoderFallbackLogged)
            {
                return;
            }

            _surfaceDecoderFallbackLogged = true;
            string error = "";
            if (_surfaceDecoder != null)
            {
                try
                {
                    error = _surfaceDecoder.Call<string>("getLastError");
                }
                catch (Exception)
                {
                    error = "";
                }
            }

            Log(string.IsNullOrEmpty(error) ? fallbackMessage : error);
        }

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

            byte[] rgbaBytes;
            int rgbaStrideBytes;
            string outputFormat;
            bool readImage = TryReadOutputImage(outputIndex, sourceFrame, out rgbaBytes, out rgbaStrideBytes, out outputFormat);

            _codec.Call("releaseOutputBuffer", outputIndex, false);

            if (!readImage)
            {
                if (!_outputImageFallbackLogged)
                {
                    _outputImageFallbackLogged = true;
                    Log("MediaCodec output image unavailable for " + _eyeName + " eye; using compressed payload preview");
                }

                decodedFrame = CreateCompressedPreview(sourceFrame);
                return true;
            }

            int requiredBytes = ((sourceFrame.Height - 1) * rgbaStrideBytes) + (sourceFrame.Width * 4);
            if (rgbaBytes == null || rgbaBytes.Length < requiredBytes)
            {
                decodedFrame = CreateCompressedPreview(sourceFrame);
                return true;
            }

            if (!HasVisibleColor(rgbaBytes, rgbaStrideBytes, sourceFrame.Width, sourceFrame.Height))
            {
                if (!_nearBlackOutputLogged)
                {
                    _nearBlackOutputLogged = true;
                    Log("MediaCodec color output for " + _eyeName + " eye was near-black; using compressed payload preview");
                }

                decodedFrame = CreateCompressedPreview(sourceFrame);
                return true;
            }

            decodedFrame = new MetalXRQuestDecodedVideoFrame(
                sourceFrame,
                rgbaBytes,
                rgbaStrideBytes,
                true,
                "mediacodec-yuv420",
                outputFormat);
            return true;
        }

        private bool TryReadOutputImage(
            int outputIndex,
            MetalXRQuestEncodedVideoFrame sourceFrame,
            out byte[] rgbaBytes,
            out int rgbaStrideBytes,
            out string outputFormat)
        {
            rgbaBytes = null;
            rgbaStrideBytes = 0;
            outputFormat = "unknown";

            AndroidJavaObject image = null;
            AndroidJavaObject[] planes = null;
            try
            {
                image = _codec.Call<AndroidJavaObject>("getOutputImage", outputIndex);
                if (image == null)
                {
                    return false;
                }

                planes = image.Call<AndroidJavaObject[]>("getPlanes");
                if (planes == null || planes.Length < 3 ||
                    planes[0] == null || planes[1] == null || planes[2] == null)
                {
                    return false;
                }

                ImagePlaneData yPlane;
                ImagePlaneData uPlane;
                ImagePlaneData vPlane;
                if (!TryReadImagePlane(planes[0], out yPlane) ||
                    !TryReadImagePlane(planes[1], out uPlane) ||
                    !TryReadImagePlane(planes[2], out vPlane))
                {
                    return false;
                }

                if (ImagePlanesAreAllZero(yPlane, uPlane, vPlane))
                {
                    if (!_zeroPlaneOutputLogged)
                    {
                        _zeroPlaneOutputLogged = true;
                        Log("MediaCodec output image planes were all zero for " + _eyeName + " eye; using compressed payload preview");
                    }

                    return false;
                }

                rgbaStrideBytes = sourceFrame.Width * 4;
                rgbaBytes = new byte[rgbaStrideBytes * sourceFrame.Height];
                for (int y = 0; y < sourceFrame.Height; y++)
                {
                    int targetRow = y * rgbaStrideBytes;
                    int chromaY = y / 2;
                    for (int x = 0; x < sourceFrame.Width; x++)
                    {
                        int chromaX = x / 2;
                        byte yValue = ReadPlaneSample(yPlane, x, y, 16);
                        byte uValue = ReadPlaneSample(uPlane, chromaX, chromaY, 128);
                        byte vValue = ReadPlaneSample(vPlane, chromaX, chromaY, 128);
                        int targetIndex = targetRow + (x * 4);
                        WriteYuvAsRgba(rgbaBytes, targetIndex, yValue, uValue, vValue);
                    }
                }

                outputFormat =
                    "YUV_420_888" +
                    ":y_stride=" + yPlane.RowStride +
                    ":u_stride=" + uPlane.RowStride +
                    ":v_stride=" + vPlane.RowStride +
                    ":u_pixel_stride=" + uPlane.PixelStride +
                    ":v_pixel_stride=" + vPlane.PixelStride;
                return true;
            }
            catch (Exception exception)
            {
                Log("MediaCodec output image read failed for " + _eyeName + " eye: " + exception.Message);
                return false;
            }
            finally
            {
                if (planes != null)
                {
                    for (int i = 0; i < planes.Length; i++)
                    {
                        if (planes[i] != null)
                        {
                            planes[i].Dispose();
                        }
                    }
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

        private bool TryReadImagePlane(AndroidJavaObject plane, out ImagePlaneData planeData)
        {
            planeData = new ImagePlaneData();
            AndroidJavaObject buffer = null;
            try
            {
                buffer = plane.Call<AndroidJavaObject>("getBuffer");
                if (buffer == null)
                {
                    return false;
                }

                int rowStride = plane.Call<int>("getRowStride");
                int pixelStride = plane.Call<int>("getPixelStride");
                buffer.Call<AndroidJavaObject>("position", 0);
                int remaining = buffer.Call<int>("remaining");
                if (rowStride <= 0 || pixelStride <= 0 || remaining <= 0)
                {
                    return false;
                }

                byte[] bytes;
                if (!TryCopyDirectByteBuffer(buffer, remaining, out bytes))
                {
                    sbyte[] signedBytes = new sbyte[remaining];
                    buffer.Call<AndroidJavaObject>("position", 0);
                    buffer.Call<AndroidJavaObject>("get", signedBytes, 0, remaining);
                    bytes = new byte[remaining];
                    Buffer.BlockCopy(signedBytes, 0, bytes, 0, remaining);
                }

                planeData.Bytes = bytes;
                planeData.RowStride = rowStride;
                planeData.PixelStride = pixelStride;
                return true;
            }
            catch (Exception exception)
            {
                Log("MediaCodec plane read failed for " + _eyeName + " eye: " + exception.Message);
                return false;
            }
            finally
            {
                if (buffer != null)
                {
                    buffer.Dispose();
                }
            }
        }

        private static unsafe bool TryCopyDirectByteBuffer(AndroidJavaObject buffer, int byteCount, out byte[] bytes)
        {
            bytes = null;
            if (buffer == null || byteCount <= 0)
            {
                return false;
            }

            IntPtr rawBuffer = buffer.GetRawObject();
            if (rawBuffer == IntPtr.Zero)
            {
                return false;
            }

            sbyte* address = AndroidJNI.GetDirectBufferAddress(rawBuffer);
            long capacity = AndroidJNI.GetDirectBufferCapacity(rawBuffer);
            if (address == null || capacity < byteCount)
            {
                return false;
            }

            bytes = new byte[byteCount];
            Marshal.Copy((IntPtr)address, bytes, 0, byteCount);
            return true;
        }

        private static byte ReadPlaneSample(ImagePlaneData plane, int x, int y, byte fallback)
        {
            if (plane.Bytes == null)
            {
                return fallback;
            }

            int index = (y * plane.RowStride) + (x * plane.PixelStride);
            if (index < 0 || index >= plane.Bytes.Length)
            {
                return fallback;
            }

            return plane.Bytes[index];
        }

        private static bool ImagePlanesAreAllZero(ImagePlaneData yPlane, ImagePlaneData uPlane, ImagePlaneData vPlane)
        {
            return !PlaneHasNonZeroByte(yPlane) &&
                   !PlaneHasNonZeroByte(uPlane) &&
                   !PlaneHasNonZeroByte(vPlane);
        }

        private static bool PlaneHasNonZeroByte(ImagePlaneData plane)
        {
            if (plane.Bytes == null || plane.Bytes.Length == 0)
            {
                return false;
            }

            int sampleStep = Math.Max(1, plane.Bytes.Length / 4096);
            for (int index = 0; index < plane.Bytes.Length; index += sampleStep)
            {
                if (plane.Bytes[index] != 0)
                {
                    return true;
                }
            }

            return false;
        }

        private static void WriteYuvAsRgba(byte[] rgbaBytes, int targetIndex, byte yValue, byte uValue, byte vValue)
        {
            int c = Math.Max(0, yValue - 16);
            int d = uValue - 128;
            int e = vValue - 128;
            int red = (298 * c) + (409 * e) + 128;
            int green = (298 * c) - (100 * d) - (208 * e) + 128;
            int blue = (298 * c) + (516 * d) + 128;

            rgbaBytes[targetIndex] = ClampToByte(red >> 8);
            rgbaBytes[targetIndex + 1] = ClampToByte(green >> 8);
            rgbaBytes[targetIndex + 2] = ClampToByte(blue >> 8);
            rgbaBytes[targetIndex + 3] = 255;
        }

        private static byte ClampToByte(int value)
        {
            if (value <= 0)
            {
                return 0;
            }

            return value >= 255 ? (byte)255 : (byte)value;
        }

        private static bool HasVisibleColor(byte[] rgbaBytes, int strideBytes, int width, int height)
        {
            const int SampleStep = 16;
            const int VisibleThreshold = 12;
            const int ColorVarianceThreshold = 6;
            int brightSamples = 0;
            int colorSamples = 0;

            for (int y = 0; y < height; y += SampleStep)
            {
                int row = y * strideBytes;
                for (int x = 0; x < width; x += SampleStep)
                {
                    int index = row + (x * 4);
                    if (index + 2 >= rgbaBytes.Length)
                    {
                        continue;
                    }

                    int red = rgbaBytes[index];
                    int green = rgbaBytes[index + 1];
                    int blue = rgbaBytes[index + 2];
                    if (red > VisibleThreshold || green > VisibleThreshold || blue > VisibleThreshold)
                    {
                        brightSamples++;
                    }

                    int max = Math.Max(red, Math.Max(green, blue));
                    int min = Math.Min(red, Math.Min(green, blue));
                    if (max - min > ColorVarianceThreshold)
                    {
                        colorSamples++;
                    }
                }
            }

            return brightSamples > 0 && colorSamples > 0;
        }

        private void UpdateOutputFormat()
        {
            AndroidJavaObject outputFormat = _codec.Call<AndroidJavaObject>("getOutputFormat");
            if (outputFormat == null)
            {
                return;
            }

            _outputStride = GetFormatInt(outputFormat, "stride", _configuredWidth);
            _outputSliceHeight = GetFormatInt(outputFormat, "slice-height", _configuredHeight);
            _outputColorFormat = GetFormatInt(outputFormat, "color-format", 0);
            Log(
                "MediaCodec output format for " + _eyeName +
                " eye color_format=" + _outputColorFormat +
                " stride=" + _outputStride +
                " slice_height=" + _outputSliceHeight);
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
            _outputSliceHeight = 0;
            _outputColorFormat = 0;
            _outputImageFallbackLogged = false;
            _zeroPlaneOutputLogged = false;
        }

        private void DisposeSurfaceDecoder()
        {
            if (_surfaceDecoder != null)
            {
                try
                {
                    _surfaceDecoder.Call("release");
                }
                catch (Exception)
                {
                }

                _surfaceDecoder.Dispose();
                _surfaceDecoder = null;
            }

            _surfacePendingFrames.Clear();
            _surfaceConfiguredWidth = 0;
            _surfaceConfiguredHeight = 0;
            _surfaceTextureName = 0;
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
            return CreateCompressedPreview(frame, "compressed-preview", "generated-rgba");
        }

        private MetalXRQuestDecodedVideoFrame CreateCompressedPreview(
            MetalXRQuestEncodedVideoFrame frame,
            string decoderMode,
            string outputFormat)
        {
            int strideBytes = frame.Width * 4;
            byte[] rgba = new byte[strideBytes * frame.Height];
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
                    int targetIndex = (y * strideBytes) + (x * 4);
                    if (frame.IsLeftEye)
                    {
                        rgba[targetIndex] = (byte)(visible / 3);
                        rgba[targetIndex + 1] = (byte)((visible * 2) / 3);
                        rgba[targetIndex + 2] = (byte)visible;
                    }
                    else
                    {
                        rgba[targetIndex] = (byte)visible;
                        rgba[targetIndex + 1] = (byte)((visible * 2) / 3);
                        rgba[targetIndex + 2] = (byte)(visible / 3);
                    }

                    rgba[targetIndex + 3] = 255;
                }
            }

            return new MetalXRQuestDecodedVideoFrame(
                frame,
                rgba,
                strideBytes,
                false,
                decoderMode,
                outputFormat);
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
