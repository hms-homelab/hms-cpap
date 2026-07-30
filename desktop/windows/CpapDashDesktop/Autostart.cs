using Microsoft.Win32;

namespace CpapDashDesktop;

/// <summary>
/// The Start at Login checkbox, backed by the per-user Run key.
///
/// HKCU rather than HKLM, and a Run key rather than a scheduled task or a
/// service, for one reason each:
///   HKCU  needs no elevation, so the installer and this checkbox never prompt.
///   Run   the shell already supervises the child, so restart-on-failure buys
///         nothing, and a scheduled task is markedly harder to remove cleanly
///         on uninstall.
///
/// The value NAME is distinct from the one the zip-install path writes, so a
/// user who tried the zip first and then installed properly ends up with two
/// clearly separate entries rather than one silently overwriting the other.
/// </summary>
public static class Autostart
{
    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "CpapDashDesktop";

    public static bool IsEnabled()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey);
            return key?.GetValue(ValueName) is string s && s.Length > 0;
        }
        catch
        {
            // A readable registry is not worth crashing the tray over; report
            // "off" and let the user try to switch it on, which will surface a
            // real error if there is one.
            return false;
        }
    }

    public static void Set(bool enabled, string shellExePath)
    {
        using var key = Registry.CurrentUser.CreateSubKey(RunKey, writable: true)
                        ?? throw new InvalidOperationException("cannot open the Run key");

        if (!enabled)
        {
            key.DeleteValue(ValueName, throwOnMissingValue: false);
            return;
        }

        // Quoted: the install path is under %LOCALAPPDATA%\Programs\..., and any
        // user whose account name contains a space gets an unquoted path that
        // launches the wrong thing or nothing at all.
        key.SetValue(ValueName, $"\"{shellExePath}\"", RegistryValueKind.String);
    }
}
