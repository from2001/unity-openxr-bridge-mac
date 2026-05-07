using System;
using UnityEngine;
using UnityEngine.XR;

#if ENABLE_INPUT_SYSTEM
using UnityEngine.InputSystem;
#endif

namespace MetalXR.Smoke
{
    public sealed class MetalXRControllerInputVisualizer : MonoBehaviour
    {
        private const string LogPrefix = "MetalXRControllerInputVisualizer";
        private const float StatusDistance = 2.0f;
        private const float LogIntervalSeconds = 2.0f;

        private Transform _statusRoot;
        private TextMesh _statusText;
        private HandView _leftView;
        private HandView _rightView;
        private Camera _mainCamera;
        private float _nextLogTime;

#if ENABLE_INPUT_SYSTEM
        private ControllerActionProbe _leftActions;
        private ControllerActionProbe _rightActions;
#endif

#if UNITY_EDITOR || UNITY_STANDALONE_OSX
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void Bootstrap()
        {
            MetalXRControllerInputVisualizer existing = FindFirstObjectByType<MetalXRControllerInputVisualizer>();
            if (existing != null)
            {
                return;
            }

            GameObject instance = new GameObject("MetalXR Controller Input Visualizer");
            DontDestroyOnLoad(instance);
            instance.AddComponent<MetalXRControllerInputVisualizer>();
        }
#endif

        private void Awake()
        {
            _statusRoot = new GameObject("MetalXR Controller Status Panel").transform;
            _statusRoot.SetParent(transform, false);

            _statusText = CreateText("MetalXR Controller Status Text", _statusRoot);
            _statusText.transform.localPosition = new Vector3(0.0f, 0.0f, 0.0f);

            _leftView = CreateHandView("Left Controller", new Color(0.25f, 0.75f, 1.0f, 1.0f));
            _rightView = CreateHandView("Right Controller", new Color(1.0f, 0.55f, 0.15f, 1.0f));

#if ENABLE_INPUT_SYSTEM
            _leftActions = new ControllerActionProbe("left", true);
            _rightActions = new ControllerActionProbe("right", false);
#endif
        }

        private void Update()
        {
            EnsureCameraAnchor();

            ControllerSample leftSample = ReadController(true);
            ControllerSample rightSample = ReadController(false);

#if ENABLE_INPUT_SYSTEM
            ControllerActionSample leftActionSample = _leftActions.Read();
            ControllerActionSample rightActionSample = _rightActions.Read();
#else
            ControllerActionSample leftActionSample = default;
            ControllerActionSample rightActionSample = default;
#endif

            UpdateHandView(_leftView, leftSample, true);
            UpdateHandView(_rightView, rightSample, false);
            UpdateStatusText(leftSample, rightSample, leftActionSample, rightActionSample);

            if (Time.unscaledTime >= _nextLogTime)
            {
                _nextLogTime = Time.unscaledTime + LogIntervalSeconds;
                Debug.Log(LogPrefix + " " + FormatLogLine("L", leftSample, leftActionSample) + " | " + FormatLogLine("R", rightSample, rightActionSample));
            }
        }

        private void OnDestroy()
        {
#if ENABLE_INPUT_SYSTEM
            if (_leftActions != null)
            {
                _leftActions.Dispose();
                _leftActions = null;
            }
            if (_rightActions != null)
            {
                _rightActions.Dispose();
                _rightActions = null;
            }
#endif

            DestroyHandView(_leftView);
            DestroyHandView(_rightView);
        }

        private void EnsureCameraAnchor()
        {
            if (_mainCamera == null)
            {
                _mainCamera = Camera.main;
            }

            if (_mainCamera == null || _statusRoot == null)
            {
                return;
            }

            Transform cameraTransform = _mainCamera.transform;
            if (_statusRoot.parent != cameraTransform)
            {
                _statusRoot.SetParent(cameraTransform, false);
            }

            _statusRoot.localPosition = new Vector3(0.0f, 0.22f, StatusDistance);
            _statusRoot.localRotation = Quaternion.identity;
            _statusRoot.localScale = Vector3.one;
        }

        private static TextMesh CreateText(string name, Transform parent)
        {
            GameObject textObject = new GameObject(name);
            textObject.transform.SetParent(parent, false);
            TextMesh text = textObject.AddComponent<TextMesh>();
            text.anchor = TextAnchor.MiddleCenter;
            text.alignment = TextAlignment.Center;
            text.characterSize = 0.0065f;
            text.fontSize = 56;
            text.color = Color.white;
            text.text = "MetalXR Controller Input";
            return text;
        }

        private static HandView CreateHandView(string name, Color color)
        {
            GameObject root = new GameObject(name + " Visualizer");

            GameObject body = GameObject.CreatePrimitive(PrimitiveType.Cube);
            body.name = name + " Body";
            body.transform.SetParent(root.transform, false);
            body.transform.localScale = new Vector3(0.075f, 0.045f, 0.13f);
            Renderer bodyRenderer = body.GetComponent<Renderer>();
            Material bodyMaterial = CreateUnlitMaterial(color);
            if (bodyRenderer != null)
            {
                bodyRenderer.sharedMaterial = bodyMaterial;
            }

            LineRenderer ray = root.AddComponent<LineRenderer>();
            Material rayMaterial = CreateUnlitMaterial(color);
            ray.sharedMaterial = rayMaterial;
            ray.positionCount = 2;
            ray.useWorldSpace = false;
            ray.startWidth = 0.012f;
            ray.endWidth = 0.002f;
            ray.SetPosition(0, Vector3.zero);
            ray.SetPosition(1, Vector3.forward * 0.55f);

            Transform triggerBar = CreateBar(name + " Trigger Bar", root.transform, new Vector3(-0.06f, -0.05f, 0.0f), color);
            Transform gripBar = CreateBar(name + " Grip Bar", root.transform, new Vector3(0.06f, -0.05f, 0.0f), color);
            Transform stickDot = CreateStickDot(name + " Thumbstick Dot", root.transform, color);

            TextMesh label = CreateText(name + " Label", root.transform);
            label.characterSize = 0.008f;
            label.fontSize = 48;
            label.transform.localPosition = new Vector3(0.0f, 0.1f, 0.0f);
            label.text = name;

            return new HandView
            {
                Root = root.transform,
                BodyRenderer = bodyRenderer,
                BodyMaterial = bodyMaterial,
                Ray = ray,
                RayMaterial = rayMaterial,
                TriggerBar = triggerBar,
                GripBar = gripBar,
                StickDot = stickDot,
                Label = label,
                BaseColor = color
            };
        }

        private static Transform CreateBar(string name, Transform parent, Vector3 localPosition, Color color)
        {
            GameObject bar = GameObject.CreatePrimitive(PrimitiveType.Cube);
            bar.name = name;
            bar.transform.SetParent(parent, false);
            bar.transform.localPosition = localPosition;
            bar.transform.localScale = new Vector3(0.035f, 0.018f, 0.035f);
            Renderer renderer = bar.GetComponent<Renderer>();
            if (renderer != null)
            {
                renderer.sharedMaterial = CreateUnlitMaterial(color);
            }
            return bar.transform;
        }

        private static Transform CreateStickDot(string name, Transform parent, Color color)
        {
            GameObject dot = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            dot.name = name;
            dot.transform.SetParent(parent, false);
            dot.transform.localPosition = new Vector3(0.0f, 0.06f, 0.0f);
            dot.transform.localScale = Vector3.one * 0.028f;
            Renderer renderer = dot.GetComponent<Renderer>();
            if (renderer != null)
            {
                renderer.sharedMaterial = CreateUnlitMaterial(color);
            }
            return dot.transform;
        }

        private static Material CreateUnlitMaterial(Color color)
        {
            Shader shader = Shader.Find("Unlit/Color");
            if (shader == null)
            {
                shader = Shader.Find("Universal Render Pipeline/Unlit");
            }

            Material material = new Material(shader);
            material.color = color;
            return material;
        }

        private static ControllerSample ReadController(bool leftHand)
        {
            XRNode node = leftHand ? XRNode.LeftHand : XRNode.RightHand;
            UnityEngine.XR.InputDevice device = InputDevices.GetDeviceAtXRNode(node);
            ControllerSample sample = new ControllerSample
            {
                DeviceValid = device.isValid
            };

            if (!device.isValid)
            {
                return sample;
            }

            bool tracked;
            if (device.TryGetFeatureValue(UnityEngine.XR.CommonUsages.isTracked, out tracked))
            {
                sample.IsTracked = tracked;
            }

            Vector3 position;
            if (device.TryGetFeatureValue(UnityEngine.XR.CommonUsages.devicePosition, out position))
            {
                sample.HasPosition = true;
                sample.Position = position;
            }

            Quaternion rotation;
            if (device.TryGetFeatureValue(UnityEngine.XR.CommonUsages.deviceRotation, out rotation))
            {
                sample.HasRotation = true;
                sample.Rotation = rotation;
            }

            sample.Trigger = ReadFloat(device, UnityEngine.XR.CommonUsages.trigger);
            sample.Grip = ReadFloat(device, UnityEngine.XR.CommonUsages.grip);
            sample.Thumbstick = ReadVector2(device, UnityEngine.XR.CommonUsages.primary2DAxis);
            sample.PrimaryButton = ReadButton(device, UnityEngine.XR.CommonUsages.primaryButton);
            sample.SecondaryButton = ReadButton(device, UnityEngine.XR.CommonUsages.secondaryButton);
            sample.MenuButton = ReadButton(device, UnityEngine.XR.CommonUsages.menuButton);
            sample.ThumbstickClick = ReadButton(device, UnityEngine.XR.CommonUsages.primary2DAxisClick);
            return sample;
        }

        private static float ReadFloat(UnityEngine.XR.InputDevice device, InputFeatureUsage<float> usage)
        {
            float value;
            if (device.TryGetFeatureValue(usage, out value))
            {
                return value;
            }
            return 0.0f;
        }

        private static Vector2 ReadVector2(UnityEngine.XR.InputDevice device, InputFeatureUsage<Vector2> usage)
        {
            Vector2 value;
            if (device.TryGetFeatureValue(usage, out value))
            {
                return value;
            }
            return Vector2.zero;
        }

        private static bool ReadButton(UnityEngine.XR.InputDevice device, InputFeatureUsage<bool> usage)
        {
            bool value;
            if (device.TryGetFeatureValue(usage, out value))
            {
                return value;
            }
            return false;
        }

        private void UpdateHandView(HandView view, ControllerSample sample, bool leftHand)
        {
            if (view == null || view.Root == null)
            {
                return;
            }

            bool poseValid = sample.DeviceValid && sample.IsTracked && sample.HasPosition && sample.HasRotation;
            if (poseValid)
            {
                view.Root.position = sample.Position;
                view.Root.rotation = sample.Rotation;
            }
            else
            {
                Vector3 fallbackLocal = leftHand ? new Vector3(-0.28f, -0.12f, StatusDistance) : new Vector3(0.28f, -0.12f, StatusDistance);
                if (_mainCamera != null)
                {
                    view.Root.position = _mainCamera.transform.TransformPoint(fallbackLocal);
                    view.Root.rotation = _mainCamera.transform.rotation;
                }
                else
                {
                    view.Root.position = fallbackLocal;
                    view.Root.rotation = Quaternion.identity;
                }
            }

            float activity = Mathf.Clamp01(Mathf.Max(sample.Trigger, sample.Grip));
            if (sample.PrimaryButton || sample.SecondaryButton || sample.MenuButton || sample.ThumbstickClick)
            {
                activity = 1.0f;
            }

            Color bodyColor = Color.Lerp(view.BaseColor * 0.35f, view.BaseColor, poseValid ? 0.65f + 0.35f * activity : 0.25f);
            bodyColor.a = 1.0f;
            if (view.BodyMaterial != null)
            {
                view.BodyMaterial.color = bodyColor;
            }

            if (view.TriggerBar != null)
            {
                view.TriggerBar.localScale = new Vector3(0.035f, 0.018f + sample.Trigger * 0.085f, 0.035f);
            }

            if (view.GripBar != null)
            {
                view.GripBar.localScale = new Vector3(0.035f, 0.018f + sample.Grip * 0.085f, 0.035f);
            }

            if (view.StickDot != null)
            {
                view.StickDot.localPosition = new Vector3(sample.Thumbstick.x * 0.065f, 0.06f + sample.Thumbstick.y * 0.065f, 0.0f);
            }

            if (view.Label != null)
            {
                view.Label.text = leftHand ? "L" : "R";
                view.Label.color = poseValid ? Color.white : new Color(0.55f, 0.55f, 0.55f, 1.0f);
            }
        }

        private void UpdateStatusText(
            ControllerSample leftSample,
            ControllerSample rightSample,
            ControllerActionSample leftActionSample,
            ControllerActionSample rightActionSample)
        {
            if (_statusText == null)
            {
                return;
            }

            _statusText.text =
                "MetalXR Controller Input\n" +
                FormatStatusLine("L", leftSample, leftActionSample) + "\n" +
                FormatStatusLine("R", rightSample, rightActionSample);
        }

        private static string FormatStatusLine(string hand, ControllerSample sample, ControllerActionSample actionSample)
        {
            string pose = sample.DeviceValid && sample.IsTracked ? "tracked" : "not tracked";
            return string.Format(
                "{0} {1} trig {2:0.00}/{3:0.00} grip {4:0.00}/{5:0.00} stick {6:0.00},{7:0.00} buttons {8}{9}{10}{11}",
                hand,
                pose,
                sample.Trigger,
                actionSample.Trigger,
                sample.Grip,
                actionSample.Grip,
                sample.Thumbstick.x,
                sample.Thumbstick.y,
                sample.PrimaryButton || actionSample.PrimaryButton ? "A" : "-",
                sample.SecondaryButton || actionSample.SecondaryButton ? "B" : "-",
                sample.MenuButton || actionSample.MenuButton ? "M" : "-",
                sample.ThumbstickClick || actionSample.ThumbstickClick ? "S" : "-");
        }

        private static string FormatLogLine(string hand, ControllerSample sample, ControllerActionSample actionSample)
        {
            return string.Format(
                "{0}:device={1} tracked={2} pos={3} rot={4} trigger={5:0.000} action_trigger={6:0.000} grip={7:0.000} action_grip={8:0.000} stick=({9:0.000},{10:0.000}) buttons=0x{11:x2}",
                hand,
                sample.DeviceValid,
                sample.IsTracked,
                sample.HasPosition,
                sample.HasRotation,
                sample.Trigger,
                actionSample.Trigger,
                sample.Grip,
                actionSample.Grip,
                sample.Thumbstick.x,
                sample.Thumbstick.y,
                sample.ButtonMask);
        }

        private static void DestroyHandView(HandView view)
        {
            if (view == null)
            {
                return;
            }

            DestroyUnityObject(view.BodyMaterial);
            DestroyUnityObject(view.RayMaterial);
            if (view.Root != null)
            {
                DestroyUnityObject(view.Root.gameObject);
            }
        }

        private static void DestroyUnityObject(UnityEngine.Object target)
        {
            if (target == null)
            {
                return;
            }

            if (Application.isPlaying)
            {
                Destroy(target);
            }
            else
            {
                DestroyImmediate(target);
            }
        }

        private sealed class HandView
        {
            public Transform Root;
            public Renderer BodyRenderer;
            public Material BodyMaterial;
            public LineRenderer Ray;
            public Material RayMaterial;
            public Transform TriggerBar;
            public Transform GripBar;
            public Transform StickDot;
            public TextMesh Label;
            public Color BaseColor;
        }

        private struct ControllerSample
        {
            public bool DeviceValid;
            public bool IsTracked;
            public bool HasPosition;
            public bool HasRotation;
            public Vector3 Position;
            public Quaternion Rotation;
            public float Trigger;
            public float Grip;
            public Vector2 Thumbstick;
            public bool PrimaryButton;
            public bool SecondaryButton;
            public bool MenuButton;
            public bool ThumbstickClick;

            public int ButtonMask
            {
                get
                {
                    int mask = 0;
                    if (PrimaryButton)
                    {
                        mask |= 1;
                    }
                    if (SecondaryButton)
                    {
                        mask |= 2;
                    }
                    if (MenuButton)
                    {
                        mask |= 4;
                    }
                    if (ThumbstickClick)
                    {
                        mask |= 8;
                    }
                    return mask;
                }
            }
        }

        private struct ControllerActionSample
        {
            public float Trigger;
            public float Grip;
            public Vector2 Thumbstick;
            public bool PrimaryButton;
            public bool SecondaryButton;
            public bool MenuButton;
            public bool ThumbstickClick;
        }

#if ENABLE_INPUT_SYSTEM
        private sealed class ControllerActionProbe : IDisposable
        {
            private readonly InputAction _trigger;
            private readonly InputAction _grip;
            private readonly InputAction _thumbstick;
            private readonly InputAction _primaryButton;
            private readonly InputAction _secondaryButton;
            private readonly InputAction _menuButton;
            private readonly InputAction _thumbstickClick;

            public ControllerActionProbe(string namePrefix, bool leftHand)
            {
                string handUsage = leftHand ? "{LeftHand}" : "{RightHand}";
                _trigger = CreateAction(namePrefix + "_trigger", InputActionType.Value, "<XRController>" + handUsage + "/trigger");
                _grip = CreateAction(namePrefix + "_grip", InputActionType.Value, "<XRController>" + handUsage + "/grip");
                _thumbstick = CreateAction(namePrefix + "_thumbstick", InputActionType.Value, "<XRController>" + handUsage + "/thumbstick");
                _primaryButton = CreateAction(namePrefix + "_primary", InputActionType.Button, "<XRController>" + handUsage + "/primaryButton");
                _secondaryButton = CreateAction(namePrefix + "_secondary", InputActionType.Button, "<XRController>" + handUsage + "/secondaryButton");
                _menuButton = CreateAction(namePrefix + "_menu", InputActionType.Button, "<XRController>" + handUsage + "/menuButton");
                _thumbstickClick = CreateAction(namePrefix + "_thumbstick_click", InputActionType.Button, "<XRController>" + handUsage + "/thumbstickClicked");

                EnableAction(_trigger);
                EnableAction(_grip);
                EnableAction(_thumbstick);
                EnableAction(_primaryButton);
                EnableAction(_secondaryButton);
                EnableAction(_menuButton);
                EnableAction(_thumbstickClick);
            }

            public ControllerActionSample Read()
            {
                return new ControllerActionSample
                {
                    Trigger = _trigger.ReadValue<float>(),
                    Grip = _grip.ReadValue<float>(),
                    Thumbstick = _thumbstick.ReadValue<Vector2>(),
                    PrimaryButton = _primaryButton.IsPressed(),
                    SecondaryButton = _secondaryButton.IsPressed(),
                    MenuButton = _menuButton.IsPressed(),
                    ThumbstickClick = _thumbstickClick.IsPressed()
                };
            }

            public void Dispose()
            {
                DisposeAction(_trigger);
                DisposeAction(_grip);
                DisposeAction(_thumbstick);
                DisposeAction(_primaryButton);
                DisposeAction(_secondaryButton);
                DisposeAction(_menuButton);
                DisposeAction(_thumbstickClick);
            }

            private static InputAction CreateAction(string name, InputActionType type, string binding)
            {
                return new InputAction(name, type, binding);
            }

            private static void EnableAction(InputAction action)
            {
                action.Enable();
            }

            private static void DisposeAction(InputAction action)
            {
                action.Disable();
                action.Dispose();
            }
        }
#endif
    }
}
