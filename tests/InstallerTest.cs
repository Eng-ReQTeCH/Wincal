using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

internal static class InstallerTest
{
    static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    // Windows registers this SID for Microsoft.Windows.ShellExperienceHost_cw5n1h2txyewy.
    static readonly SecurityIdentifier ShellSid = new SecurityIdentifier(
        "S-1-15-2-155514346-2573954481-755741238-1654018636-1233331829-3075935687-2861478708");

    static void CheckShellAccess(string module, string settings)
    {
        bool canLoad = false;
        foreach (FileSystemAccessRule rule in File.GetAccessControl(module).GetAccessRules(true, false, typeof(SecurityIdentifier)))
            if (rule.IdentityReference.Equals(ShellSid) && rule.AccessControlType == AccessControlType.Allow &&
                (rule.FileSystemRights & FileSystemRights.ReadAndExecute) == FileSystemRights.ReadAndExecute)
                canLoad = true;
        Require(canLoad, "ShellExperienceHost needs read and execute access to the module.");
        foreach (FileSystemAccessRule rule in File.GetAccessControl(settings).GetAccessRules(true, true, typeof(SecurityIdentifier)))
            Require(!rule.IdentityReference.Equals(ShellSid), "Setup granted the shell access to the settings.");
    }

    static void CheckHttp(string launcher, string settings)
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        try
        {
            int port = ((IPEndPoint)listener.LocalEndpoint).Port;
            const string fixtureUser = "loopback-test";
            string fixturePassword = Guid.NewGuid().ToString("N");
            string authorization = "Authorization: Basic " + Convert.ToBase64String(
                Encoding.ASCII.GetBytes(fixtureUser + ":" + fixturePassword));
            Setup.WriteSettings(settings, "http://127.0.0.1:" + port + "/me/personal", fixtureUser, fixturePassword, false);
            var accept = listener.AcceptTcpClientAsync();
            DateTime localDay = DateTime.Today;
            using (Process process = Process.Start(new ProcessStartInfo(launcher, "--check " + Setup.Quote(settings))
            { UseShellExecute = false, RedirectStandardError = true }))
            {
                try
                {
                    if (!accept.Wait(10000)) throw new InvalidOperationException("The launcher did not connect over HTTP. " +
                        (process.HasExited ? process.StandardError.ReadToEnd() : "The request is still running."));
                    using (TcpClient client = accept.Result)
                    using (NetworkStream stream = client.GetStream())
                    using (var reader = new StreamReader(stream, Encoding.ASCII))
                    {
                        stream.ReadTimeout = 4000;
                        Require(reader.ReadLine() == "REPORT /me/personal/ HTTP/1.1", "Expected a plain HTTP CalDAV REPORT.");
                        bool authorized = false;
                        int length = 0;
                        string header;
                        while (!string.IsNullOrEmpty(header = reader.ReadLine()))
                        {
                            authorized |= string.Equals(header, authorization, StringComparison.Ordinal);
                            const string lengthHeader = "Content-Length:";
                            if (header.StartsWith(lengthHeader, StringComparison.OrdinalIgnoreCase))
                                length = int.Parse(header.Substring(lengthHeader.Length), CultureInfo.InvariantCulture);
                        }
                        Require(authorized, "The CalDAV request did not include the configured login.");
                        Require(length > 0 && length < 4096, "Expected a calendar query body.");
                        char[] buffer = new char[length];
                        Require(reader.ReadBlock(buffer, 0, length) == length, "The calendar query was incomplete.");
                        string report = new string(buffer);
                        Require(report.Contains(CalendarQueryRange(localDay)) || report.Contains(CalendarQueryRange(DateTime.Today)),
                            "The calendar query must include zero-duration events at local midnight.");
                        const string body = "<d:multistatus xmlns:d=\"DAV:\"/>";
                        byte[] response = Encoding.ASCII.GetBytes("HTTP/1.1 207 Multi-Status\r\nContent-Length: " + body.Length +
                            "\r\nContent-Type: application/xml\r\nConnection: close\r\n\r\n" + body);
                        stream.Write(response, 0, response.Length);
                    }
                    var verify = listener.AcceptTcpClientAsync();
                    Require(verify.Wait(10000), "An empty REPORT should trigger a collection check.");
                    using (TcpClient client = verify.Result)
                    using (NetworkStream stream = client.GetStream())
                    using (var reader = new StreamReader(stream, Encoding.ASCII))
                    {
                        stream.ReadTimeout = 4000;
                        Require(reader.ReadLine() == "PROPFIND /me/personal/ HTTP/1.1", "Expected a collection check over HTTP.");
                        bool depthZero = false;
                        string header;
                        while (!string.IsNullOrEmpty(header = reader.ReadLine()))
                            depthZero |= string.Equals(header, "Depth: 0", StringComparison.OrdinalIgnoreCase);
                        Require(depthZero, "Expected a Depth: 0 collection check.");
                        const string body = "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">" +
                            "<d:response><d:propstat><d:prop><d:resourcetype><d:collection/><c:calendar/></d:resourcetype>" +
                            "</d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response></d:multistatus>";
                        byte[] response = Encoding.ASCII.GetBytes("HTTP/1.1 207 Multi-Status\r\nContent-Length: " + body.Length +
                            "\r\nContent-Type: application/xml\r\nConnection: close\r\n\r\n" + body);
                        stream.Write(response, 0, response.Length);
                    }
                    Require(process.WaitForExit(10000) && process.ExitCode == 0, "The launcher did not verify the empty calendar.");
                }
                finally
                {
                    if (!process.HasExited) { process.Kill(); process.WaitForExit(); }
                }
            }
        }
        finally { listener.Stop(); }
        Console.WriteLine("Plain HTTP CalDAV connection and Basic authentication passed.");
    }

    static string CalendarQueryRange(DateTime day)
    {
        const string format = "yyyyMMdd'T'HHmmss'Z'";
        string start = day.ToUniversalTime().AddSeconds(-1).ToString(format, CultureInfo.InvariantCulture);
        string end = day.AddDays(1).ToUniversalTime().ToString(format, CultureInfo.InvariantCulture);
        return "<C:time-range start=\"" + start + "\" end=\"" + end + "\"";
    }

    static int Main(string[] args)
    {
        string suffix = "wincal-test-" + Guid.NewGuid().ToString("N");
        string root = Path.Combine(Path.GetTempPath(), suffix);
        string registry = @"Software\" + suffix;
        bool registryTest = args.Length == 1 && args[0] == "--registry";
        bool httpTest = args.Length == 1 && args[0] == "--http";
        try
        {
            if (!registryTest) Directory.CreateDirectory(root);
            string launcher = Path.Combine(root, Setup.Launcher);
            string tap = Path.Combine(root, Setup.Tap);
            string settings = Path.Combine(root, Setup.SettingsFile);
            if (registryTest)
            {
                Require(!Setup.Install(root, registry, "", "", "", true), "First install reported itself as an update.");
                CheckShellAccess(Path.Combine(root, Setup.FlyoutModule), settings);
            }
            else
            {
                Setup.Extract(Setup.Launcher, launcher);
                Setup.WriteSettings(settings, "", "", "", true);
            }
            File.WriteAllText(tap, "legacy module test fixture");
            using (Process process = Process.Start(launcher, "--check " + Setup.Quote(settings)))
            {
                Require(process.WaitForExit(10000), "Test launcher did not complete.");
                Require(process.ExitCode == 0, "Launcher could not read the encrypted settings.");
            }
            if (httpTest) CheckHttp(launcher, settings);
            if (registryTest)
            {
                Require(Setup.Install(root, registry, "", "", "", true), "Settings update reported itself as a fresh install.");
                using (RegistryKey startup = Registry.CurrentUser.OpenSubKey(registry + @"\Run", true))
                {
                    Require(startup.GetValue(Setup.InstallId) != null, "Install did not register startup.");
                    startup.SetValue("Unrelated", "keep");
                }
                string saved = Convert.ToBase64String(File.ReadAllBytes(settings));
                string cache = Path.Combine(root, "events.bin");
                File.WriteAllText(cache, "existing cache fixture");
                string installedAppKey = registry + @"\Uninstall\Win11CalendarFix";
                using (RegistryKey entry = Registry.CurrentUser.OpenSubKey(installedAppKey, true))
                    entry.SetValue("DisplayName", "Win11CalendarFix");
                string module = Path.Combine(root, Setup.FlyoutModule);
                var access = File.GetAccessControl(module, AccessControlSections.Access);
                access.PurgeAccessRules(ShellSid);
                File.SetAccessControl(module, access);
                File.WriteAllText(tap, "legacy module test fixture");
                using (FileStream oldTap = File.Open(tap, FileMode.Open, FileAccess.Read, FileShare.Read))
                    Setup.StageUpdate(root, registry);
                CheckShellAccess(module, settings);
                // Simulate an upgrade while the shell still holds the previous
                // version of the DLL. A new filename must carry the new code.
                const string oldModuleName = "CalendarFlyout-043.dll";
                string oldModule = Path.Combine(root, oldModuleName);
                File.WriteAllText(oldModule, "previous version fixture");
                File.Delete(module);
                using (FileStream lockedOldModule = File.Open(oldModule, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    Setup.StageUpdate(root, registry);
                    Require(File.Exists(oldModule), "The update should defer removal of a locked previous module.");
                    Require(Setup.FlyoutModule != oldModuleName, "A changed flyout must use a new versioned filename.");
                    using (Stream expected = typeof(Setup).Assembly.GetManifestResourceStream(Setup.FlyoutModule))
                    using (var bytes = new MemoryStream())
                    {
                        expected.CopyTo(bytes);
                        Require(Convert.ToBase64String(File.ReadAllBytes(module)) == Convert.ToBase64String(bytes.ToArray()),
                            "The upgrade did not install the embedded current flyout module.");
                    }
                    CheckShellAccess(module, settings);
                }
                Setup.StageUpdate(root, registry);
                Require(!File.Exists(oldModule), "A subsequent update should remove the unlocked previous module.");
                Require(Convert.ToBase64String(File.ReadAllBytes(settings)) == saved, "The update touched the saved settings.");
                Require(File.ReadAllText(cache) == "existing cache fixture", "The update touched the saved event cache.");
                using (RegistryKey entry = Registry.CurrentUser.OpenSubKey(installedAppKey))
                    Require((string)entry.GetValue("DisplayName") == "wincal", "The upgrade did not rename the existing app entry.");
                using (RegistryKey startup = Registry.CurrentUser.OpenSubKey(registry + @"\Run"))
                    Require((string)startup.GetValue(Setup.InstallId) == Setup.Quote(launcher) + " --background",
                        "The startup entry did not select the updated launcher.");
            }
            File.WriteAllText(Path.Combine(root, "keep.txt"), "keep");
            File.WriteAllText(tap, "locked legacy module fixture");
            Func<bool> undo = registryTest ? (Func<bool>)(() => Setup.Undo(root, registry)) : (() => Setup.RemoveFiles(root));
            using (FileStream locked = File.Open(tap, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                Require(!undo(), "Undo should defer a locked module.");
                if (registryTest)
                    using (RegistryKey pending = Registry.CurrentUser.OpenSubKey(registry + @"\RunOnce"))
                        Require(pending.GetValue(Setup.InstallId) != null, "Undo did not register its deferred cleanup.");
            }
            Require(undo(), "Deferred cleanup did not complete after the file became available.");
            Require(!File.Exists(tap) && !File.Exists(launcher) && !File.Exists(settings), "Undo left installed files or settings.");
            Require(File.Exists(Path.Combine(root, "keep.txt")), "Undo deleted an unrelated file.");
            if (registryTest)
            {
                using (RegistryKey startup = Registry.CurrentUser.OpenSubKey(registry + @"\Run"))
                    Require(startup.GetValue(Setup.InstallId) == null && (string)startup.GetValue("Unrelated") == "keep", "Undo changed unexpected startup entries.");
                using (RegistryKey entry = Registry.CurrentUser.OpenSubKey(registry + @"\Uninstall\" + Setup.InstallId))
                    Require(entry == null, "Undo left the Installed Apps entry.");
            }
            Console.WriteLine(registryTest ? "Installer registry and undo tests passed." : "Encrypted settings round-trip and file cleanup tests passed.");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
        finally
        {
            if (Directory.Exists(root))
            {
                foreach (string file in Directory.GetFiles(root)) File.Delete(file);
                Directory.Delete(root);
            }
            if (registryTest)
                foreach (string key in new[] { @"\Uninstall\" + Setup.InstallId, @"\Uninstall", @"\Run", @"\RunOnce", "" })
                    Registry.CurrentUser.DeleteSubKey(registry + key, false);
        }
    }
}
