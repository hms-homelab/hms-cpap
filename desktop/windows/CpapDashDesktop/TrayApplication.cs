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

        var menu = new ContextMenuStrip();
        menu.Items.Add(new ToolStripMenuItem("CpapDash Desktop") { Enabled = false });
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add(_statusItem);
        menu.Items.Add(new ToolStripMenuItem("Open Dashboard", null, (_, _) => OpenDashboard()));
        menu.Items.Add(new ToolStripMenuItem("Sync Now", null, async (_, _) => await SyncNowAsync()));
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
        _supervisor.GaveUp += OnChildGaveUp;
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

    private void OnChildGaveUp(int exitCode)
    {
        // Marshalled onto the UI thread: the supervisor raises this from its own
        // watcher thread, and touching a NotifyIcon from there throws.
        if (_icon.ContextMenuStrip?.InvokeRequired == true)
        {
            _icon.ContextMenuStrip.BeginInvoke(() => OnChildGaveUp(exitCode));
            return;
        }

        SetHealthy(false);
        _statusItem.Text = "Status: stopped";

        // Port conflict is a hard failure by decision (SDD-005), so the dialog
        // has to be ACTIONABLE rather than merely truthful. Telling someone the
        // port is busy without telling them where to change it just moves the
        // problem.
        var configDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".hms-cpap");

        var result = MessageBox.Show(
            "CpapDash Desktop could not start.\n\n" +
            $"Port {DefaultPort} is most likely already in use by another program " +
            "on this computer.\n\n" +
            "To use a different port, edit \"web_port\" in config.json and start again.\n\n" +
            "Open the config folder now?",
            "CpapDash Desktop", MessageBoxButtons.YesNo, MessageBoxIcon.Error);

        if (result == DialogResult.Yes)
        {
            try
            {
                Directory.CreateDirectory(configDir);
                Process.Start(new ProcessStartInfo(configDir) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Could not open {configDir}:\n{ex.Message}",
                    "CpapDash Desktop", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
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
