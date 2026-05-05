#if UNITY_EDITOR
using System;
using UnityEditor;
using UnityEngine;

namespace MetalXR.Editor
{
    public static class MetalXRWorkflowAutomation
    {
        [InitializeOnLoadMethod]
        private static void StartUnityCliLoopServerFromEnvironment()
        {
            string shouldStartServer = Environment.GetEnvironmentVariable("METALXR_START_ULOOP_SERVER");
            if (shouldStartServer == "1")
            {
                EditorApplication.delayCall += StartUnityCliLoopServer;
            }
        }

        public static void StartUnityCliLoopServer()
        {
            if (!io.github.hatayama.uLoopMCP.McpServerController.IsServerRunning)
            {
                Debug.Log("[MetalXRWorkflowAutomation] Starting Unity CLI Loop server.");
                io.github.hatayama.uLoopMCP.McpServerController.StartServer();
            }
        }
    }
}
#endif
