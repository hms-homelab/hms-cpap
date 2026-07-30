using System.Diagnostics;

namespace CpapDashDesktop;

/// <summary>
/// Owns the hms_cpap.exe child process: starts it, restarts it when it dies,
/// and gives up when it is clearly never going to work.
///
/// Deliberately dumb. It parses no CPAP data and holds no state beyond the
/// child handle, because everything that matters already lives in hms_cpap and
/// duplicating any of it here creates a second source of truth on the one
/// platform we can least easily debug.
/// </summary>
public sealed class Supervisor : IDisposable
{
    /// <summary>Restart backoff, in seconds, one entry per consecutive failure.</summary>
    ///
    /// Escalating rather than fixed: a transient crash should come back almost
    /// at once, but a binary that dies instantly every time must not be
    /// relaunched in a tight loop for the rest of the session, burning CPU on a
    /// laptop and filling the event log.
    private static readonly int[] BackoffSeconds = { 1, 2, 5, 10, 30, 60 };

    /// <summary>
    /// A child that exits this fast never really started. Anything quicker than
    /// this counts toward the failure streak; anything longer resets it, so a
    /// service that ran for hours and then crashed restarts promptly.
    /// </summary>
    private static readonly TimeSpan HealthyRunThreshold = TimeSpan.FromSeconds(20);

    private readonly string _exePath;
    private readonly Action<string> _log;
    private Process? _child;
    private int _consecutiveFailures;
    private volatile bool _stopping;
    private Thread? _watcher;

    public Supervisor(string exePath, Action<string> log)
    {
        _exePath = exePath;
        _log = log;
    }

    /// <summary>Raised when the child has failed repeatedly and fast.</summary>
    /// The argument is the child's last exit code. Surfaced rather than retried
    /// forever, because the common cause is a port conflict and the user has to
    /// act; silently retrying would leave them staring at a tray icon that
    /// never turns green with nothing explaining why.
    public event Action<int>? GaveUp;

    public bool IsRunning => _child is { HasExited: false };

    public void Start()
    {
        _stopping = false;
        _watcher = new Thread(WatchLoop) { IsBackground = true, Name = "hms-cpap-supervisor" };
        _watcher.Start();
    }

    private void WatchLoop()
    {
        while (!_stopping)
        {
            var startedAt = DateTime.UtcNow;
            int exitCode;

            try
            {
                exitCode = RunOnce();
            }
            catch (Exception ex)
            {
                _log($"could not start hms_cpap: {ex.Message}");
                exitCode = -1;
            }

            if (_stopping) return;

            var ranFor = DateTime.UtcNow - startedAt;
            if (ranFor >= HealthyRunThreshold)
            {
                // It worked for a while, so whatever happened is not a
                // configuration problem. Treat the next restart as the first.
                _consecutiveFailures = 0;
            }
            else
            {
                _consecutiveFailures++;
            }

            if (_consecutiveFailures >= BackoffSeconds.Length)
            {
                _log($"hms_cpap exited {exitCode} repeatedly; giving up");
                GaveUp?.Invoke(exitCode);
                return;
            }

            var wait = BackoffSeconds[Math.Min(_consecutiveFailures, BackoffSeconds.Length - 1)];
            _log($"hms_cpap exited {exitCode}; restarting in {wait}s");

            // Slept in one-second slices so Quit does not have to wait out a
            // sixty-second backoff before the tray icon disappears.
            for (var i = 0; i < wait && !_stopping; i++) Thread.Sleep(1000);
        }
    }

    private int RunOnce()
    {
        var psi = new ProcessStartInfo
        {
            FileName = _exePath,
            // The shell owns presentation, so the child must not race us to open
            // a browser tab on every restart.
            Arguments = "--no-browser",
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(_exePath) ?? Environment.CurrentDirectory
        };

        // This is the contract with hms_cpap: seeing this variable, it exits 0
        // on "apply settings" instead of re-execing itself, and it declines to
        // install its own autostart entry, because this shell owns both.
        psi.Environment["HMS_CPAP_SUPERVISED"] = "1";

        _child = Process.Start(psi);
        if (_child is null) throw new InvalidOperationException("Process.Start returned null");

        _child.WaitForExit();
        return _child.ExitCode;
    }

    /// <summary>Stop supervising and end the child.</summary>
    public void Stop()
    {
        _stopping = true;
        try
        {
            var child = _child;
            if (child is { HasExited: false })
            {
                // CloseMainWindow is useless here: hms_cpap has no window. Kill
                // the whole tree so a mid-burst child cannot outlive the shell
                // and keep port 8893 bound, which would make the next launch
                // fail with the one error we most want to avoid.
                child.Kill(entireProcessTree: true);
                child.WaitForExit(5000);
            }
        }
        catch (Exception ex)
        {
            _log($"error stopping hms_cpap: {ex.Message}");
        }
    }

    public void Dispose()
    {
        Stop();
        _child?.Dispose();
    }
}
