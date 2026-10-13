using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;

internal static class ResponseBufferTests
{
    private delegate byte[] BuildResponse(string cors, int status, string message, string body, bool plain);
    private static int checks;

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
        checks++;
    }

    private static void CheckResponse(BuildResponse build, string body, string cors, int status,
                                      string message, bool plain)
    {
        byte[] response = build(cors, status, message, body, plain);
        int split = -1;
        for (int i = 0; i + 3 < response.Length; i++) {
            if (response[i] == 13 && response[i + 1] == 10 &&
                response[i + 2] == 13 && response[i + 3] == 10) {
                split = i + 4;
                break;
            }
        }
        Require(split >= 0, "HTTP header terminator missing");
        string header = Encoding.UTF8.GetString(response, 0, split);
        string contentType = plain ? "text/plain" : "application/json";
        byte[] expectedBody = Encoding.UTF8.GetBytes(body);
        string expectedHeader = "HTTP/1.1 " + status.ToString(CultureInfo.InvariantCulture) + " " + message + "\r\n"
            + "Content-Type: " + contentType + "; charset=utf-8\r\n"
            + "Content-Length: " + expectedBody.Length.ToString(CultureInfo.InvariantCulture) + "\r\n"
            + "Access-Control-Allow-Origin: " + cors + "\r\n"
            + "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            + "Access-Control-Allow-Headers: Content-Type\r\n"
            + "Connection: close\r\n\r\n";
        Require(header == expectedHeader, "HTTP header contract changed");
        Require(response.Length - split == expectedBody.Length, "Content-Length differs from encoded body");
        bool equal = true;
        for (int i = 0; i < expectedBody.Length; i++) {
            if (response[split + i] != expectedBody[i]) { equal = false; break; }
        }
        Require(equal, "Response body bytes changed");
    }

    private static int Main(string[] args)
    {
        try {
            if (args.Length < 1) throw new ArgumentException("Expected the built C# server path");
            Assembly server = Assembly.LoadFrom(Path.GetFullPath(args[0]));
            MethodInfo method = server.GetType("DstServerCs.Program", throwOnError: true)
                .GetMethod("BuildResponse", BindingFlags.NonPublic | BindingFlags.Static);
            if (method == null) throw new MissingMethodException("DstServerCs.Program.BuildResponse");
            var build = (BuildResponse)Delegate.CreateDelegate(typeof(BuildResponse), method);

            if (args.Length > 1 && (args[1] == "--bench" || args[1] == "--bench-small")) {
                bool small = args[1] == "--bench-small";
                string body = small ? "{\"translation\":\"\u4e2d\u6587\",\"source\":\"cache\"}" : new string('\u4e2d', 1024 * 1024);
                int iterations = small ? 200000 : 40;
                build("null", 200, "OK", body, false);
                GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
                int gen2 = GC.CollectionCount(2);
                var watch = Stopwatch.StartNew();
                long bytes = 0;
                for (int i = 0; i < iterations; i++) bytes += build("null", 200, "OK", body, false).Length;
                watch.Stop();
                Console.WriteLine("response benchmark: iterations={0}; body_chars={1}; elapsed_ms={2}; gen2_collections={3}; bytes={4}",
                    iterations, body.Length, watch.Elapsed.TotalMilliseconds.ToString("F2", CultureInfo.InvariantCulture),
                    GC.CollectionCount(2) - gen2, bytes);
                return 0;
            }

            CheckResponse(build, "", "null", 204, "No Content", false);
            CheckResponse(build, "{\"status\":\"ok\"}", "http://localhost", 200, "OK", false);
            CheckResponse(build, "missing_text", "null", 400, "Bad Request", true);
            CheckResponse(build, "\u4f60\u597d\ud83d\ude00", "file://", 200, "OK", true);
            CheckResponse(build, "\ud800x\udc00", "null", 200, "OK", true);
            CheckResponse(build, "\0\r\n\t\"\\/", "null", 200, "OK", true);
            CheckResponse(build, "[player] {b}text{/b} %s \\c[2] \\v[1] <color=#fff>text</color> [color=red]text[/color]",
                "http://localhost:19999", 200, "OK", true);
            foreach (int length in new[] { 255, 256, 257, 4095, 4096, 4097, 32767, 32768, 32769, 1024 * 1024 }) {
                CheckResponse(build, new string('\u4e2d', length) + "\ud83d\ude00", "null", 200, "OK", false);
            }
            // 大响应不得污染后续短响应的内容或消息边界。
            CheckResponse(build, "{\"entries\":[],\"count\":0}", "null", 200, "OK", false);
            Console.WriteLine("C# response buffer checks PASS: " + checks);
            return 0;
        } catch (Exception ex) {
            Console.Error.WriteLine("C# response buffer checks FAIL: " + ex);
            return 1;
        }
    }
}
