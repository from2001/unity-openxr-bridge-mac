using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Xml;
using UnityEditor;
using UnityEditor.Android;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEditor.XR.Management;
using UnityEditor.XR.Management.Metadata;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.XR.Management;
using UnityEngine.XR.OpenXR;
using UnityEngine.XR.OpenXR.Features.Interactions;
using UnityEngine.XR.OpenXR.Features.MetaQuestSupport;

public static class MetalXRQuestClientBuild
{
    private const string DefaultOutputPath = "Builds/MetalXRQuestClient.apk";
    private const string PackageName = "com.metalxr.questclient";
    private const string ProductName = "MetalXR Quest Client";
    private const string OpenXRLoaderTypeName = "UnityEngine.XR.OpenXR.OpenXRLoader";

    public static void BuildAndroidApk()
    {
        string outputPath = GetArgument("-metalxrBuildOutput");
        string androidGraphicsApi = GetArgument("-metalxrAndroidGraphicsApi");
        if (string.IsNullOrWhiteSpace(outputPath))
        {
            outputPath = Path.Combine(GetProjectRoot(), DefaultOutputPath);
        }

        outputPath = Path.GetFullPath(outputPath);
        string outputDirectory = Path.GetDirectoryName(outputPath);
        if (!string.IsNullOrWhiteSpace(outputDirectory))
        {
            Directory.CreateDirectory(outputDirectory);
        }

        AndroidGraphicsApiOverride graphicsApiOverride = AndroidGraphicsApiOverride.Apply(androidGraphicsApi);
        try
        {
            ConfigureAndroidPlayerSettings();
            EnsureAndroidOpenXRLoader();
            EnsureAndroidOpenXRFeatures();

            EditorUserBuildSettings.SwitchActiveBuildTarget(BuildTargetGroup.Android, BuildTarget.Android);

            BuildPlayerOptions options = new BuildPlayerOptions
            {
                scenes = EnabledScenes(),
                locationPathName = outputPath,
                target = BuildTarget.Android,
                targetGroup = BuildTargetGroup.Android,
                options = BuildOptions.Development | BuildOptions.CompressWithLz4
            };

            BuildReport report = BuildPipeline.BuildPlayer(options);
            if (report.summary.result != BuildResult.Succeeded)
            {
                throw new InvalidOperationException("MetalXR Quest client APK build failed: " + report.summary.result);
            }

            Debug.Log("MetalXR Quest client APK built at " + outputPath);
        }
        finally
        {
            graphicsApiOverride.Dispose();
        }
    }

    private static void ConfigureAndroidPlayerSettings()
    {
        PlayerSettings.companyName = "MetalXR";
        PlayerSettings.productName = ProductName;
        PlayerSettings.SetApplicationIdentifier(NamedBuildTarget.Android, PackageName);
        PlayerSettings.SetScriptingBackend(NamedBuildTarget.Android, ScriptingImplementation.IL2CPP);
        PlayerSettings.Android.targetArchitectures = AndroidArchitecture.ARM64;
        PlayerSettings.Android.forceInternetPermission = true;
    }

    private static void EnsureAndroidOpenXRLoader()
    {
        XRGeneralSettingsPerBuildTarget settingsPerBuildTarget;
        if (!EditorBuildSettings.TryGetConfigObject(XRGeneralSettings.k_SettingsKey, out settingsPerBuildTarget) || settingsPerBuildTarget == null)
        {
            settingsPerBuildTarget = FindXRSettingsPerBuildTarget();
            if (settingsPerBuildTarget == null)
            {
                throw new InvalidOperationException("XRGeneralSettingsPerBuildTarget asset was not found.");
            }

            EditorBuildSettings.AddConfigObject(XRGeneralSettings.k_SettingsKey, settingsPerBuildTarget, true);
        }

        if (!settingsPerBuildTarget.HasSettingsForBuildTarget(BuildTargetGroup.Android))
        {
            settingsPerBuildTarget.CreateDefaultSettingsForBuildTarget(BuildTargetGroup.Android);
        }

        if (!settingsPerBuildTarget.HasManagerSettingsForBuildTarget(BuildTargetGroup.Android))
        {
            settingsPerBuildTarget.CreateDefaultManagerSettingsForBuildTarget(BuildTargetGroup.Android);
        }

        XRManagerSettings managerSettings = settingsPerBuildTarget.ManagerSettingsForBuildTarget(BuildTargetGroup.Android);
        if (managerSettings == null)
        {
            throw new InvalidOperationException("Android XRManagerSettings could not be created.");
        }

        bool hasOpenXRLoader = false;
        foreach (XRLoader loader in managerSettings.activeLoaders)
        {
            if (loader != null && loader.GetType().FullName == OpenXRLoaderTypeName)
            {
                hasOpenXRLoader = true;
                break;
            }
        }

        if (!hasOpenXRLoader)
        {
            bool assigned = XRPackageMetadataStore.AssignLoader(managerSettings, OpenXRLoaderTypeName, BuildTargetGroup.Android);
            if (!assigned)
            {
                throw new InvalidOperationException("Could not assign OpenXRLoader for Android.");
            }
        }

        managerSettings.automaticLoading = true;
        managerSettings.automaticRunning = true;
        XRGeneralSettings androidSettings = settingsPerBuildTarget.SettingsForBuildTarget(BuildTargetGroup.Android);
        if (androidSettings != null)
        {
            androidSettings.InitManagerOnStart = true;
            EditorUtility.SetDirty(androidSettings);
        }

        EditorUtility.SetDirty(managerSettings);
        EditorUtility.SetDirty(settingsPerBuildTarget);
        AssetDatabase.SaveAssets();
    }

    private static void EnsureAndroidOpenXRFeatures()
    {
        OpenXRSettings openXrSettings = OpenXRSettings.GetSettingsForBuildTargetGroup(BuildTargetGroup.Android);
        if (openXrSettings == null)
        {
            throw new InvalidOperationException("Android OpenXRSettings asset was not found.");
        }

        EnableRequiredFeature(openXrSettings.GetFeature<MetaQuestFeature>(), "Meta Quest Support");
        EnableRequiredFeature(openXrSettings.GetFeature<HandInteractionProfile>(), "Hand Interaction Profile");
        EnableRequiredFeature(openXrSettings.GetFeature<OculusTouchControllerProfile>(), "Oculus Touch Controller Profile");
        EnableRequiredFeature(openXrSettings.GetFeature<MetaQuestTouchPlusControllerProfile>(), "Meta Quest Touch Plus Controller Profile");

        EditorUtility.SetDirty(openXrSettings);
        AssetDatabase.SaveAssets();
    }

    private static void EnableRequiredFeature(UnityEngine.XR.OpenXR.Features.OpenXRFeature feature, string featureName)
    {
        if (feature == null)
        {
            throw new InvalidOperationException("Android OpenXR feature was not found: " + featureName);
        }

        feature.enabled = true;
        EditorUtility.SetDirty(feature);
    }

    private static XRGeneralSettingsPerBuildTarget FindXRSettingsPerBuildTarget()
    {
        string[] guids = AssetDatabase.FindAssets("t:XRGeneralSettingsPerBuildTarget");
        for (int i = 0; i < guids.Length; i++)
        {
            string path = AssetDatabase.GUIDToAssetPath(guids[i]);
            XRGeneralSettingsPerBuildTarget settings = AssetDatabase.LoadAssetAtPath<XRGeneralSettingsPerBuildTarget>(path);
            if (settings != null)
            {
                return settings;
            }
        }

        return null;
    }

    private static string[] EnabledScenes()
    {
        string[] scenes = EditorBuildSettings.scenes
            .Where(scene => scene.enabled)
            .Select(scene => scene.path)
            .ToArray();

        if (scenes.Length == 0)
        {
            return new[] { "Assets/Scenes/SampleScene.unity" };
        }

        return scenes;
    }

    private static string GetArgument(string name)
    {
        string[] args = Environment.GetCommandLineArgs();
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == name && i + 1 < args.Length)
            {
                return args[i + 1];
            }
        }

        return string.Empty;
    }

    private static string GetProjectRoot()
    {
        DirectoryInfo dataDirectory = new DirectoryInfo(Application.dataPath);
        DirectoryInfo projectDirectory = dataDirectory.Parent;
        if (projectDirectory == null)
        {
            return Directory.GetCurrentDirectory();
        }

        return projectDirectory.FullName;
    }

    private sealed class AndroidGraphicsApiOverride : IDisposable
    {
        private readonly bool _active;
        private readonly bool _previousUseDefaultGraphicsApis;
        private readonly GraphicsDeviceType[] _previousGraphicsApis;

        private AndroidGraphicsApiOverride(
            bool active,
            bool previousUseDefaultGraphicsApis,
            GraphicsDeviceType[] previousGraphicsApis)
        {
            _active = active;
            _previousUseDefaultGraphicsApis = previousUseDefaultGraphicsApis;
            _previousGraphicsApis = previousGraphicsApis;
        }

        public static AndroidGraphicsApiOverride Apply(string argument)
        {
            GraphicsDeviceType[] requestedApis;
            if (!TryParseAndroidGraphicsApis(argument, out requestedApis))
            {
                return new AndroidGraphicsApiOverride(false, false, null);
            }

            bool previousUseDefault = PlayerSettings.GetUseDefaultGraphicsAPIs(BuildTarget.Android);
            GraphicsDeviceType[] previousApis = PlayerSettings.GetGraphicsAPIs(BuildTarget.Android);
            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, false);
            PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, requestedApis);
            Debug.Log("MetalXR Quest client Android graphics APIs: " + string.Join(",", requestedApis));
            return new AndroidGraphicsApiOverride(true, previousUseDefault, previousApis);
        }

        public void Dispose()
        {
            if (!_active)
            {
                return;
            }

            if (_previousGraphicsApis != null && _previousGraphicsApis.Length > 0)
            {
                PlayerSettings.SetGraphicsAPIs(BuildTarget.Android, _previousGraphicsApis);
            }

            PlayerSettings.SetUseDefaultGraphicsAPIs(BuildTarget.Android, _previousUseDefaultGraphicsApis);
        }

        private static bool TryParseAndroidGraphicsApis(string argument, out GraphicsDeviceType[] graphicsApis)
        {
            graphicsApis = null;
            if (string.IsNullOrWhiteSpace(argument) ||
                string.Equals(argument, "default", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(argument, "auto", StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            string[] parts = argument.Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0)
            {
                return false;
            }

            graphicsApis = new GraphicsDeviceType[parts.Length];
            for (int i = 0; i < parts.Length; i++)
            {
                graphicsApis[i] = ParseAndroidGraphicsApi(parts[i].Trim());
            }

            return true;
        }

        private static GraphicsDeviceType ParseAndroidGraphicsApi(string value)
        {
            if (string.Equals(value, "gles3", StringComparison.OrdinalIgnoreCase) ||
                string.Equals(value, "opengles3", StringComparison.OrdinalIgnoreCase))
            {
                return GraphicsDeviceType.OpenGLES3;
            }

            if (string.Equals(value, "vulkan", StringComparison.OrdinalIgnoreCase))
            {
                return GraphicsDeviceType.Vulkan;
            }

            throw new InvalidOperationException("Unsupported MetalXR Android graphics API override: " + value);
        }
    }
}

public sealed class MetalXRQuestClientAndroidManifestPostprocessor : IPostGenerateGradleAndroidProject
{
    private const string AndroidNamespace = "http://schemas.android.com/apk/res/android";

    public int callbackOrder
    {
        get { return 1000; }
    }

    public void OnPostGenerateGradleAndroidProject(string path)
    {
        string manifestPath = FindManifestPath(path);
        if (string.IsNullOrEmpty(manifestPath))
        {
            Debug.LogWarning("MetalXR Quest client Android manifest was not found under " + path);
            return;
        }

        XmlDocument document = new XmlDocument();
        document.PreserveWhitespace = true;
        document.Load(manifestPath);

        XmlElement manifest = document.DocumentElement;
        if (manifest == null)
        {
            Debug.LogWarning("MetalXR Quest client Android manifest is empty: " + manifestPath);
            return;
        }

        XmlElement application = manifest.SelectSingleNode("application") as XmlElement;
        if (application == null)
        {
            Debug.LogWarning("MetalXR Quest client Android manifest has no application node: " + manifestPath);
            return;
        }

        UpsertManifestElement(document, manifest, "uses-permission", "com.oculus.permission.HAND_TRACKING");
        UpsertManifestElement(
            document,
            manifest,
            "uses-feature",
            "oculus.software.handtracking",
            new ManifestAttribute("required", "false"));
        UpsertManifestElement(
            document,
            application,
            "meta-data",
            "com.oculus.handtracking.version",
            new ManifestAttribute("value", "V2.0"));
        UpsertManifestElement(
            document,
            application,
            "meta-data",
            "com.oculus.handtracking.frequency",
            new ManifestAttribute("value", "LOW"));

        using (XmlTextWriter writer = new XmlTextWriter(manifestPath, new UTF8Encoding(false)))
        {
            writer.Formatting = Formatting.Indented;
            document.Save(writer);
        }

        Debug.Log("MetalXR Quest client Android manifest updated at " + manifestPath);
    }

    private static string FindManifestPath(string path)
    {
        string directManifest = Path.Combine(path, "src", "main", "AndroidManifest.xml");
        if (File.Exists(directManifest))
        {
            return directManifest;
        }

        string unityLibraryManifest = Path.Combine(path, "unityLibrary", "src", "main", "AndroidManifest.xml");
        if (File.Exists(unityLibraryManifest))
        {
            return unityLibraryManifest;
        }

        return string.Empty;
    }

    private static void UpsertManifestElement(
        XmlDocument document,
        XmlElement parent,
        string elementName,
        string androidName,
        params ManifestAttribute[] attributes)
    {
        XmlElement element = FindChildElement(parent, elementName, androidName);
        if (element == null)
        {
            element = document.CreateElement(elementName);
            parent.AppendChild(element);
        }

        SetAndroidAttribute(document, element, "name", androidName);
        for (int i = 0; i < attributes.Length; i++)
        {
            SetAndroidAttribute(document, element, attributes[i].Name, attributes[i].Value);
        }
    }

    private static XmlElement FindChildElement(XmlElement parent, string elementName, string androidName)
    {
        XmlNodeList nodes = parent.SelectNodes(elementName);
        for (int i = 0; i < nodes.Count; i++)
        {
            XmlElement element = nodes[i] as XmlElement;
            if (element == null)
            {
                continue;
            }

            string name = element.GetAttribute("name", AndroidNamespace);
            if (string.Equals(name, androidName, StringComparison.Ordinal))
            {
                return element;
            }
        }

        return null;
    }

    private static void SetAndroidAttribute(XmlDocument document, XmlElement element, string name, string value)
    {
        XmlAttribute attribute = element.GetAttributeNode(name, AndroidNamespace);
        if (attribute == null)
        {
            attribute = document.CreateAttribute("android", name, AndroidNamespace);
            element.Attributes.Append(attribute);
        }

        attribute.Value = value;
    }

    private readonly struct ManifestAttribute
    {
        public ManifestAttribute(string name, string value)
        {
            Name = name;
            Value = value;
        }

        public string Name { get; }
        public string Value { get; }
    }
}
