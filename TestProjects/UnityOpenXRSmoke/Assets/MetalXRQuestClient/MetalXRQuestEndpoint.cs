using System;
using UnityEngine;

namespace MetalXR.QuestClient
{
    public readonly struct MetalXRQuestEndpoint
    {
        public const string DefaultHost = "127.0.0.1";
        public const int DefaultPort = 47000;

        public MetalXRQuestEndpoint(string host, int port)
        {
            Host = string.IsNullOrWhiteSpace(host) ? DefaultHost : host;
            Port = port > 0 ? port : DefaultPort;
        }

        public string Host { get; }
        public int Port { get; }

        public static MetalXRQuestEndpoint FromCommandLine()
        {
            string host = DefaultHost;
            int port = DefaultPort;

            string[] args = Environment.GetCommandLineArgs();
            for (int i = 0; i < args.Length; i++)
            {
                string arg = args[i];
                if (arg.StartsWith("--metalxr-host=", StringComparison.Ordinal))
                {
                    host = arg.Substring("--metalxr-host=".Length);
                    continue;
                }

                if (arg == "--metalxr-host" && i + 1 < args.Length)
                {
                    host = args[i + 1];
                    i++;
                    continue;
                }

                if (arg.StartsWith("--metalxr-port=", StringComparison.Ordinal))
                {
                    int parsedPort;
                    if (int.TryParse(arg.Substring("--metalxr-port=".Length), out parsedPort))
                    {
                        port = parsedPort;
                    }

                    continue;
                }

                if (arg == "--metalxr-port" && i + 1 < args.Length)
                {
                    int parsedPort;
                    if (int.TryParse(args[i + 1], out parsedPort))
                    {
                        port = parsedPort;
                    }

                    i++;
                }
            }

#if UNITY_ANDROID && !UNITY_EDITOR
            ApplyAndroidIntentExtras(ref host, ref port);
#endif

            return new MetalXRQuestEndpoint(host, port);
        }

#if UNITY_ANDROID && !UNITY_EDITOR
        private static void ApplyAndroidIntentExtras(ref string host, ref int port)
        {
            try
            {
                using (AndroidJavaClass unityPlayer = new AndroidJavaClass("com.unity3d.player.UnityPlayer"))
                {
                    using (AndroidJavaObject activity = unityPlayer.GetStatic<AndroidJavaObject>("currentActivity"))
                    {
                        if (activity == null)
                        {
                            return;
                        }

                        using (AndroidJavaObject intent = activity.Call<AndroidJavaObject>("getIntent"))
                        {
                            if (intent == null)
                            {
                                return;
                            }

                            string extraHost = intent.Call<string>("getStringExtra", "metalxr_host");
                            if (!string.IsNullOrWhiteSpace(extraHost))
                            {
                                host = extraHost;
                            }

                            int extraPort = intent.Call<int>("getIntExtra", "metalxr_port", port);
                            if (extraPort > 0)
                            {
                                port = extraPort;
                            }
                        }
                    }
                }
            }
            catch (Exception exception)
            {
                Debug.Log("MetalXRQuestClient could not read Android intent extras: " + exception.Message);
            }
        }
#endif
    }
}
