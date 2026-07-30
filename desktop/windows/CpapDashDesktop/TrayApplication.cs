using System.Diagnostics;
using System.Net.Http;
using System.Text.Json;

namespace CpapDashDesktop;

/// <summary>
/// The tray icon and its menu. Owns no state and parses no CPAP data: it starts
/// hms_cpap, watches /health, and opens a browser.
/// </summary>
public sealed class TrayApplication : ApplicationContext
{
    private const int DefaultPort = 8893;

    private readonly NotifyIcon _icon;
    private readonly Supervisor _supervisor;
    private readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(3) };
    private readonly System.Windows.Forms.Timer _poll;
    private readonly string _shellExe;
    private readonly ToolStripMenuItem _statusItem;
    private readonly ToolStripMenuItem _autostartItem;
    private readonly ToolStripMenuItem _retryItem;

    private bool _healthy;
    private string _version = "";

    public TrayApplication()
    {
        _shellExe = Environment.ProcessPath ?? Application.ExecutablePath;
        var dir = Path.GetDirectoryName(_shellExe)!;
        var childExe = Path.Combine(dir, "hms_cpap.exe");

        if (!File.Exists(childExe))
        {
            // Naming the path beats "something went wrong": the usual cause is
            // running the shell out of a half-extracted zip.
            MessageBox.Show(
                $"CpapDash Desktop could not find hms_cpap.exe.\n\nExpected it here:\n{childExe}",
                "CpapDash Desktop", MessageBoxButtons.OK, MessageBoxIcon.Error);
            Environment.Exit(1);
        }

        _statusItem = new ToolStripMenuItem("Status: starting...") { Enabled = false };
        _autostartItem = new ToolStripMenuItem("Start at Login", null, OnToggleAutostart)
        {
            CheckOnClick = true,
            Checked = Autostart.IsEnabled()
        };

        _retryItem = new ToolStripMenuItem("Retry", null, (_, _) =>
        {
            _retryItem!.Visible = false;
            _statusItem.Text = "Status: starting...";
            _supervisor!.Retry();
        }) { Visible = false };

        var menu = new ContextMenuStrip();
        menu.Items.Add(new ToolStripMenuItem("CpapDash Desktop") { Enabled = false });
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(_statusItem);
        menu.Items.Add(new ToolStripMenuItem("Open Dashboard", null, (_, _) => OpenDashboard()));
        menu.Items.Add(new ToolStripMenuItem("Sync Now", null, async (_, _) => await SyncNowAsync()));
        menu.Items.Add(_retryItem);
        menu.Items.Add(new ToolStripMenuItem("Open config folder", null, (_, _) => OpenConfigFolder()));
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(_autostartItem);
        menu.Items.Add(new ToolStripMenuItem("Quit", null, (_, _) => Quit()));

        _icon = new NotifyIcon
        {
            Icon = SystemIcons.Application,
            Text = "CpapDash Desktop",
            Visible = true,
            ContextMenuStrip = menu
        };
        _icon.DoubleClick += (_, _) => OpenDashboard();

        _supervisor = new Supervisor(childExe, Log);
        _supervisor.PreflightFailed += OnPreflightFailed;
        _supervisor.Exited += OnChildExited;
        _supervisor.Start();

        _poll = new System.Windows.Forms.Timer { Interval = 5000 };
        _poll.Tick += async (_, _) => await PollHealthAsync();
        _poll.Start();
    }

    private static void Log(string message) => Debug.WriteLine($"[CpapDash] {message}");

    private static string BaseUrl => $"http://localhost:{DefaultPort}";

    private async Task PollHealthAsync()
    {
        try
        {
            using var res = await _http.GetAsync($"{BaseUrl}/health");
            if (res.IsSuccessStatusCode)
            {
                var body = await res.Content.ReadAsStringAsync();
                using var doc = JsonDocument.Parse(body);
                _version = doc.RootElement.TryGetProperty("version", out var v)
                    ? v.GetString() ?? "" : "";
                SetHealthy(true);
                return;
            }
        }
        catch
        {
            // Expected while the child is starting or restarting. Not logged on
            // every tick, or the debug output becomes useless noise.
        }
        SetHealthy(false);
    }

    private void SetHealthy(bool healthy)
    {
        _healthy = healthy;
        _statusItem.Text = healthy
            ? $"Status: running{(string.IsNullOrEmpty(_version) ? "" : $" ({_version})")}"
            : "Status: starting...";
        _icon.Text = healthy ? $"CpapDash Desktop - running" : "CpapDash Desktop - starting";
    }

    private void OpenDashboard()
    {
        // UseShellExecute so this goes to the user's default browser rather than
        // trying to execute a URL as a program.
        try
        {
            Process.Start(new ProcessStartInfo(BaseUrl) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Could not open a browser:\n{ex.Message}", "CpapDash Desktop",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private async Task SyncNowAsync()
    {
        if (!_healthy)
        {
            MessageBox.Show("CpapDash is not running yet. Give it a moment and try again.",
                "CpapDash Desktop", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        try
        {
            using var content = new StringContent("{}", System.Text.Encoding.UTF8, "application/json");
            using var res = await _http.PostAsync($"{BaseUrl}/api/sync/now", content);
            var body = await res.Content.ReadAsStringAsync();
            // The endpoint reports whether a cycle was already in flight, and
            // saying so beats a generic "done" that is sometimes a lie.
            _icon.ShowBalloonTip(3000, "CpapDash",
                body.Contains("already") ? "A sync is already running." : "Sync requested.",
                ToolTipIcon.Info);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Could not reach CpapDash:\n{ex.Message}", "CpapDash Desktop",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void OnToggleAutostart(object? sender, EventArgs e)
    {
        try
        {
            Autostart.Set(_autostartItem.Checked, _shellExe);
        }
        catch (Exception ex)
        {
            _autostartItem.Checked = !_autostartItem.Checked;   // put it back
            MessageBox.Show($"Could not change the Start at Login setting:\n{ex.Message}",
                "CpapDash Desktop", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    /// <summary>The configuration is unusable. Show exactly which check failed.</summary>
    ///
    /// This is the whole point of preflighting: the dialog quotes the real
    /// report, naming the failing item and its remedy, instead of guessing that
    /// a port is "most likely" in use.
    private void OnPreflightFailed(string report)
    {
        if (InvokeOnUi(() => OnPreflightFailed(report))) return;

        SetHealthy(false);
        _statusItem.Text = "Status: configuration problem";
        _retryItem.Visible = true;

        var result = MessageBox.Show(
            "CpapDash Desktop cannot start yet.\n\n" +
            report + "\n\n" +
            "Fix the item marked FAIL in config.json, then choose Retry.\n\n" +
            "Open the config folder now?",
            "CpapDash Desktop", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);

        if (result == DialogResult.Yes) OpenConfigFolder();
    }

    /// <summary>The child stopped. Report it; do not silently restart.</summary>
    private void OnChildExited(int exitCode, string lastOutput)
    {
        if (InvokeOnUi(() => OnChildExited(exitCode, lastOutput))) return;

        SetHealthy(false);
        _statusItem.Text = $"Status: stopped (exit {exitCode})";
        _retryItem.Visible = true;

        // A balloon rather than a modal box: unlike a preflight failure this can
        // happen hours later while the user is doing something else, and
        // stealing focus for it would be worse than the problem.
        var lines = lastOutput.Split('\n');
        _icon.ShowBalloonTip(10000, "CpapDash stopped",
            string.IsNullOrWhiteSpace(lastOutput)
                ? $"hms_cpap exited with code {exitCode}. Use Retry to start it again."
                : lines[lines.Length - 1].Trim(),
            ToolTipIcon.Error);

        Log($"child exited {exitCode}; last output:\n{lastOutput}");
    }

    /// <summary>Marshal onto the UI thread. Returns true when it re-posted.</summary>
    private bool InvokeOnUi(Action action)
    {
        var target = _icon.ContextMenuStrip;
        if (target is { IsHandleCreated: true, InvokeRequired: true })
        {
            target.BeginInvoke(action);
            return true;
        }
        return false;
    }

    private void OpenConfigFolder()
    {
        var dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".hms-cpap");
        try
        {
            Directory.CreateDirectory(dir);
            Process.Start(new ProcessStartInfo(dir) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Could not open {dir}:\n{ex.Message}",
                "CpapDash Desktop", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void Quit()
    {
        _poll.Stop();
        _supervisor.Stop();
        // Hidden explicitly: a NotifyIcon left visible lingers as a ghost in the
        // tray until the user hovers over it.
        _icon.Visible = false;
        _icon.Dispose();
        ExitThread();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _poll.Dispose();
            _supervisor.Dispose();
            _http.Dispose();
            _icon.Dispose();
        }
        base.Dispose(disposing);
    }
}
