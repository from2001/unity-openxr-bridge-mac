using System;
using UnityEngine;

namespace MetalXR.QuestClient
{
    public sealed class MetalXRQuestClientDisplay : MonoBehaviour
    {
        private const string LogPrefix = "MetalXRQuestClient";
        private const int EyeLayerLeft = 28;
        private const int EyeLayerRight = 29;
        private const int TextureWidth = 1024;
        private const int TextureHeight = 576;
        private const float FrameUpdateIntervalSeconds = 1.0f / 12.0f;

        private Texture2D _leftTexture;
        private Texture2D _rightTexture;
        private Material _leftMaterial;
        private Material _rightMaterial;
        private Mesh _quadMesh;
        private Color32[] _leftPixels;
        private Color32[] _rightPixels;
        private MetalXRQuestHandshakeClient _handshakeClient;
        private float _nextFrameTime;
        private int _frameIndex;

        private void Start()
        {
            Application.targetFrameRate = 72;
            Screen.sleepTimeout = SleepTimeout.NeverSleep;

            CreateDiagnosticRig();
            UpdateDiagnosticTexture(true, 0);
            UpdateDiagnosticTexture(false, 0);

            MetalXRQuestEndpoint endpoint = MetalXRQuestEndpoint.FromCommandLine();
            _handshakeClient = new MetalXRQuestHandshakeClient(endpoint);
            _handshakeClient.Start();

            Debug.Log(LogPrefix + " started; diagnostic stereo frame source active; host=" + endpoint.Host + ":" + endpoint.Port);
        }

        private void Update()
        {
            if (Time.unscaledTime >= _nextFrameTime)
            {
                _nextFrameTime = Time.unscaledTime + FrameUpdateIntervalSeconds;
                _frameIndex++;
                UpdateDiagnosticTexture(true, _frameIndex);
                UpdateDiagnosticTexture(false, _frameIndex);
            }

            if (_handshakeClient != null)
            {
                _handshakeClient.DrainLogs(LogClientMessage);
            }
        }

        private void OnDestroy()
        {
            if (_handshakeClient != null)
            {
                _handshakeClient.Dispose();
                _handshakeClient = null;
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
}
