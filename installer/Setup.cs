using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("wincal Setup")]
[assembly: AssemblyProduct("wincal")]
[assembly: AssemblyVersion(Setup.Version + ".0")]

internal static partial class Setup
{
    internal const string Name = "wincal";
    // Keep the storage and registry identity used by previous installers.
    internal const string InstallId = "Win11CalendarFix";
    internal const string Launcher = "Win11CalendarNative.exe";
    internal const string FlyoutModule = "CalendarFlyout-045.dll";
    const string ShellPackage = "Microsoft.Windows.ShellExperienceHost_cw5n1h2txyewy";
    internal const string Tap = "CalendarTap.dll";
    internal const string Version = "0.4.5";
    static readonly string[] LegacyLaunchers = { "Win11CalendarFix.exe", "Win11CalendarFix-016.exe", "Win11CalendarFix-017.exe", "Win11CalendarAgenda.exe" };
    static readonly string[] LegacyFiles = {
        Tap, "CalendarTap-016.dll", "CalendarTap-017.dll", "CalendarFlyout-030.dll", "CalendarFlyout-031.dll",
        "CalendarFlyout-032.dll", "CalendarFlyout-040.dll", "CalendarFlyout-041.dll", "CalendarFlyout-042.dll", "CalendarFlyout-043.dll", "CalendarFlyout-044.dll"
    };
    internal const string SettingsFile = "settings.bin";
    internal const string UninstallDriver = "Uninstall.cmd";
    internal const string InstalledSetup = "Setup.exe";
    internal const string WindowsRegistry = @"Software\Microsoft\Windows\CurrentVersion";
#if arm64
    const ushort Architecture = 0xaa64;
#else
    const ushort Architecture = 0x8664;
#endif

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);

    [DllImport("userenv.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern int DeriveAppContainerSidFromAppContainerName(string name, out IntPtr sid);

    [DllImport("advapi32.dll")]
    static extern IntPtr FreeSid(IntPtr sid);

    [STAThread]
    static int Main(string[] args)
    {
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), InstallId);
        bool uninstall = args.Length > 0 && args[0] == "--uninstall";
        bool cleanup = uninstall && args.Length == 2 && args[1] == "--cleanup";
        bool keepSettings = args.Length == 1 && args[0] == "--keep-settings";
        try
        {
            if (uninstall)
            {
                bool removed = Undo(root, WindowsRegistry);
                if (!cleanup) MessageBox.Show(removed ? Name + " was removed." :
                    Name + " is off and the saved connection was removed. Sign out and back in to finish removing the files.", Name);
                return removed ? 0 : 2;
            }
            if (keepSettings)
            {
                VerifyPlatform();
                StageUpdate(root, WindowsRegistry);
                StartLauncher(root);
                return 0;
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            VerifyPlatform();
            Application.Run(new SetupForm(root));
            return 0;
        }
        catch (Exception error)
        {
            if (keepSettings) Console.Error.WriteLine(error.Message);
            else if (!cleanup) MessageBox.Show(error.Message, Name, MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 1;
        }
    }

    static void VerifyPlatform()
    {
        using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
        {
            int build = Convert.ToInt32(key.GetValue("CurrentBuildNumber"));
            if (build < 22000)
                throw new InvalidOperationException(Name + " requires Windows 11.");
        }
        ushort processMachine, nativeMachine;
        using (Process self = Process.GetCurrentProcess())
        {
            if (!IsWow64Process2(self.Handle, out processMachine, out nativeMachine) || nativeMachine != Architecture)
                throw new InvalidOperationException("Download the installer for your PC's native architecture.");
        }
    }

    internal static void WriteSettings(string path, string url, string user, string password, bool demo)
    {
        url = url.Trim();
        if (demo) url = user = password = "";
        else
        {
            Uri uri;
            if (!Uri.TryCreate(url, UriKind.Absolute, out uri) ||
                (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps) || uri.AbsolutePath == "/" ||
                uri.Query.Length != 0 || uri.Fragment.Length != 0 || uri.UserInfo.Length != 0)
                throw new ArgumentException("Enter the HTTP or HTTPS URL of your CalDAV calendar collection.");
            url = uri.AbsoluteUri;
            if (user.Contains(":") || (user.Length == 0 && password.Length != 0)) throw new ArgumentException("Enter a valid username.");
        }
        if (url.Length >= 2048 || user.Length >= 256 || password.Length >= 512) throw new ArgumentException("The URL or login is too long.");
        byte[] clear = new byte[5636];
        try
        {
            Encoding.Unicode.GetBytes(url, 0, url.Length, clear, 0);
            Encoding.Unicode.GetBytes(user, 0, user.Length, clear, 4096);
            Encoding.Unicode.GetBytes(password, 0, password.Length, clear, 4608);
            clear[5632] = demo ? (byte)1 : (byte)0;
            File.WriteAllBytes(path, ProtectedData.Protect(clear, null, DataProtectionScope.CurrentUser));
        }
        finally { Array.Clear(clear, 0, clear.Length); }
    }

    internal static void Extract(string filename, string destination)
    {
        using (Stream input = Assembly.GetExecutingAssembly().GetManifestResourceStream(filename))
        {
            if (input == null) throw new IOException("Setup is missing " + filename + ".");
            using (FileStream output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None))
                input.CopyTo(output);
        }
    }

    static void VerifyDirectory(string root)
    {
        if (Directory.Exists(root) && (File.GetAttributes(root) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("The installation directory is a junction or symbolic link.");
    }

    static void StartLauncher(string root)
    {
        using (Process process = Process.Start(Path.Combine(root, Launcher))) { }
    }

    static bool StopLauncher(string root, string filename = Launcher)
    {
        string path = Path.Combine(root, filename);
        bool stopped = false;
        foreach (Process process in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(filename)))
        {
            using (process)
            {
                string executable;
                try { executable = process.MainModule.FileName; }
                catch (InvalidOperationException) { continue; }
                catch (System.ComponentModel.Win32Exception) { continue; }
                if (!string.Equals(executable, path, StringComparison.OrdinalIgnoreCase)) continue;
                try
                {
                    process.Kill();
                    if (!process.WaitForExit(4000)) throw new IOException("The calendar launcher did not stop.");
                    stopped = true;
                }
                catch (InvalidOperationException) { }
            }
        }
        return stopped;
    }

    internal static bool Install(string root, string registry, string url, string user, string password, bool demo)
    {
        VerifyDirectory(root);
        using (RegistryKey pending = Registry.CurrentUser.OpenSubKey(registry + @"\RunOnce"))
        {
            if (pending != null && pending.GetValue(InstallId) != null)
                throw new IOException("Sign out and back in to finish the previous uninstall before reinstalling.");
        }

        Directory.CreateDirectory(root);
        string settings = Path.Combine(root, SettingsFile);
        string temporary = Path.Combine(root, Guid.NewGuid().ToString("N") + ".tmp");
        bool existing = File.Exists(settings);
        bool committed = false;
        bool restartOnFailure = false;
        try
        {
            WriteSettings(temporary, url, user, password, demo);
            restartOnFailure = Publish(root, registry);
            ReplaceFile(temporary, settings);
            committed = true;
            return existing;
        }
        finally
        {
            File.Delete(temporary);
            if (restartOnFailure && existing && !committed) StartLauncher(root);
            if (!existing && !committed)
            {
                Undo(root, registry);
                File.Delete(Path.Combine(root, InstalledSetup));
                File.Delete(Path.Combine(root, UninstallDriver));
                if (Directory.GetFileSystemEntries(root).Length == 0) Directory.Delete(root);
            }
        }
    }

    static void AllowShellToLoadFlyout(string path)
    {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("The calendar module is a symbolic link or reparse point.");

        IntPtr sid;
        Marshal.ThrowExceptionForHR(DeriveAppContainerSidFromAppContainerName(ShellPackage, out sid));
        try
        {
            // Only the DLL is loaded by the shell. The launcher owns the settings and network requests.
            var access = File.GetAccessControl(path, AccessControlSections.Access);
            access.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid),
                FileSystemRights.ReadAndExecute, AccessControlType.Allow));
            File.SetAccessControl(path, access);
        }
        finally { FreeSid(sid); }
    }

    static void ReplaceFile(string staged, string destination)
    {
        if (File.Exists(destination)) File.Replace(staged, destination, null);
        else File.Move(staged, destination);
    }

    static bool Publish(string root, string registry)
    {
        string launcher = Path.Combine(root, Launcher);
        string setup = Path.Combine(root, InstalledSetup);
        string staged = Path.Combine(root, Guid.NewGuid().ToString("N") + ".tmp");
        string stagedLauncher = staged + ".exe";
        string module = Path.Combine(root, FlyoutModule);
        bool wasRunning = false;
        try
        {
            Extract(Launcher, stagedLauncher);
            if (File.Exists(module)) AllowShellToLoadFlyout(module);
            else
            {
                Extract(FlyoutModule, staged);
                AllowShellToLoadFlyout(staged);
                File.Move(staged, module);
            }
            if (!string.Equals(Application.ExecutablePath, setup, StringComparison.OrdinalIgnoreCase))
            {
                File.Copy(Application.ExecutablePath, staged);
                ReplaceFile(staged, setup);
            }
            Extract(UninstallDriver, staged);
            ReplaceFile(staged, Path.Combine(root, UninstallDriver));
            using (RegistryKey entry = Registry.CurrentUser.CreateSubKey(registry + @"\Uninstall\" + InstallId))
            {
                entry.SetValue("DisplayName", Name);
                entry.SetValue("DisplayVersion", Version);
                entry.SetValue("DisplayIcon", launcher);
                entry.SetValue("InstallLocation", root);
                entry.SetValue("UninstallString", UndoCommand(root, false));
                entry.SetValue("NoModify", 1);
                entry.SetValue("NoRepair", 1);
            }
            using (RegistryKey startup = Registry.CurrentUser.CreateSubKey(registry + @"\Run"))
                startup.SetValue(InstallId, Quote(launcher) + " --background");
            wasRunning = StopLauncher(root);
            ReplaceFile(stagedLauncher, launcher);
        }
        catch
        {
            if (wasRunning) StartLauncher(root);
            throw;
        }
        finally
        {
            File.Delete(staged);
            File.Delete(stagedLauncher);
        }
        foreach (string file in LegacyLaunchers) StopLauncher(root, file);
        RemoveLegacyFiles(root);
        return wasRunning;
    }

    internal static void StageUpdate(string root, string registry)
    {
        VerifyDirectory(root);
        if (!File.Exists(Path.Combine(root, SettingsFile))) throw new IOException("The existing installation could not be found.");
        Publish(root, registry);
    }

    static bool RemoveLegacyFiles(string root) { return RemoveKnownFiles(root, LegacyLaunchers) & RemoveKnownFiles(root, LegacyFiles); }

    internal static string Quote(string value) { return "\"" + value + "\""; }

    static string UndoCommand(string root, bool cleanup)
    {
        string cmd = Path.Combine(Environment.SystemDirectory, "cmd.exe");
        string script = Quote(Path.Combine(root, UninstallDriver));
        return Quote(cmd) + " /d /c \"" + script + (cleanup ? " --cleanup\"" : "\"");
    }

    internal static bool Undo(string root, string registry)
    {
        VerifyDirectory(root);
        using (RegistryKey startup = Registry.CurrentUser.OpenSubKey(registry + @"\Run", true))
            if (startup != null) startup.DeleteValue(InstallId, false);
        bool removed = RemoveFiles(root);
        if (removed)
        {
            using (RegistryKey retry = Registry.CurrentUser.OpenSubKey(registry + @"\RunOnce", true))
                if (retry != null) retry.DeleteValue(InstallId, false);
        }
        else
        {
            using (RegistryKey retry = Registry.CurrentUser.CreateSubKey(registry + @"\RunOnce"))
                retry.SetValue(InstallId, UndoCommand(root, true));
        }
        using (RegistryKey apps = Registry.CurrentUser.OpenSubKey(registry + @"\Uninstall", true))
            if (apps != null) apps.DeleteSubKey(InstallId, false);
        return removed;
    }

    internal static bool RemoveFiles(string root)
    {
        if (Directory.Exists(root)) File.Delete(Path.Combine(root, SettingsFile));
        StopLauncher(root);
        foreach (string file in LegacyLaunchers) StopLauncher(root, file);
        return RemoveKnownFiles(root, new[] { Launcher, FlyoutModule, "events.bin", "events.bin.tmp", "status.txt" }) & RemoveLegacyFiles(root);
    }

    static bool RemoveKnownFiles(string root, string[] names)
    {
        bool removed = true;
        foreach (string file in names)
        {
            try { if (Directory.Exists(root)) File.Delete(Path.Combine(root, file)); }
            catch (IOException) { removed = false; }
            catch (UnauthorizedAccessException) { removed = false; }
        }
        return removed;
    }

}
