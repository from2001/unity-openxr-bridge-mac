using UnityEngine;

namespace MetalXR.QuestClient
{
    public static class MetalXRQuestClientBootstrap
    {
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.AfterSceneLoad)]
        private static void CreateClient()
        {
#if UNITY_ANDROID && !UNITY_EDITOR
            MetalXRQuestClientDisplay existing = Object.FindFirstObjectByType<MetalXRQuestClientDisplay>();
            if (existing != null)
            {
                return;
            }

            GameObject clientObject = new GameObject("MetalXR Quest Client");
            Object.DontDestroyOnLoad(clientObject);
            clientObject.AddComponent<MetalXRQuestClientDisplay>();
#endif
        }
    }
}
