using System.Diagnostics;
using System.Text;

namespace CpapDashDesktop;

/// <summary>
/// Runs hms_cpap.exe and reports what happened to it.
///
/// NOT a retry loop, deliberately. An earlier version restarted the child with
/// escalating backoff and inferred the cause of failure from how quickly it
/// died, which meant a busy port was "discovered" by a timing heuristic and the
/// user was shown a guess. Configuration errors are deterministic: a taken
/// port, a wrong password and an unwritable folder do not become correct on the
/// second attempt, so retrying only delays and obscures the real message.
///
/// The order is now: validate, then run, and if it exits, say why and stop.
/// Validation lives in hms_cpap itself (--preflight), so this shell, the
/// installer, systemd and launchd all get the same verdict from one
/// implementation rather than four drifting copies.
/// </summary>
public sealed class Supervisor : IDisposable
{
    private readonly string _exePath;
    private readonly Action<string> _log;
    private Process? _child;
    private volatile bool _stopping;
    private Thread? _runner;

    public Supervisor(string exePath, Action<string> log)
    {
        _exePath = exePath;
        _log = log;
    }

    /// <summary>The configuration is not usable. Carries the preflight report.</summary>
    /// Raised INSTEAD of starting, so the user sees the specific failing check
    /// and its remedy rather than a generic "could not start".
    public event Action<string>? PreflightFailed;

    /// <summary>The child exited. Carries the exit code and its last output.</summary>
    /// Not followed by a restart. If it stopped, something happened, and the
    /// honest response is to say so once and let the user decide.
    public event Action<int, string>? Exited;

    public bool IsRunning => _child is { HasExited: false };

    public void Start()
    {
        _stopping = false;
        _runner = new Thread(Run) { IsBackground = true, Name = "hms-cpap-runner" };
        _runner.Start();
    }

    private void Run()
    {
        // 1. Validate. Cheap, deterministic, and it produces a report written
        //    for the person who has to fix it.
        var (preflightOk, report) = RunPreflight();
        if (!preflightOk)
        {
            _log("preflight failed:\n" + report);
            PreflightFailed?.Invoke(report);
            return;
        }

        // 2. Run. One attempt.
        try
        {
            var (exitCode, output) = RunChild();
            if (_stopping) return;
            _log($"hms_cpap exited with code {exitCode}");
            Exited?.Invoke(exitCode, output);
        }
        catch (Exception ex)
        {
            _log($"could not start hms_cpap: {ex.Message}");
            Exited?.Invoke(-1, ex.Message);
        }
    }

    /// <summary>Run `hms_cpap --preflight` and capture its report.</summary>
    private (bool ok, string report) RunPreflight()
    {
        var psi = new ProcessStartInfo
        {
            FileName = _exePath,
            Arguments = "--preflight",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(_exePath) ?? Environment.CurrentDirectory
        };

        using var p = Process.Start(psi);
        if (p is null) return (false, "Could not run the configuration check.");

        var stdout = p.StandardOutput.ReadToEnd();
        var stderr = p.StandardError.ReadToEnd();
        // Bounded rather than indefinite: a check that cannot answer in half a
        // minute is itself a failure, and hanging here would reproduce exactly
        // the "stuck with no explanation" state this design removes.
        if (!p.WaitForExit(30_000))
        {
            try { p.Kill(entireProcessTree: true); } catch { /* already gone */ }
            return (false, "The configuration check did not finish in 30 seconds.");
        }

        var report = string.IsNullOrWhiteSpace(stdout) ? stderr : stdout;
        return (p.ExitCode == 0, report.Trim());
    }

    private (int exitCode, string output) RunChild()
    {
        var psi = new ProcessStartInfo
        {
            FileName = _exePath,
            // The shell owns presentation; the child must not open a browser.
            Arguments = "--no-browser",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(_exePath) ?? Environment.CurrentDirectory
        };

        // The contract with hms_cpap: seeing this, it exits 0 on "apply
        // settings" rather than re-execing itself, and declines to install its
        // own autostart entry, because this shell owns both.
        psi.Environment["HMS_CPAP_SUPERVISED"] = "1";

        _child = Process.Start(psi) ?? throw new InvalidOperationException("Process.Start returned null");

        // Keep only the tail. The interesting part of a crash is the last thing
        // it said, and buffering a long-running service's entire stderr would
        // grow without bound on a machine left on for weeks.
        var tail = new Queue<string>();
        _child.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is null) return;
            tail.Enqueue(e.Data);
            while (tail.Count > 40) tail.Dequeue();
        };
        _child.BeginErrorReadLine();

        _child.WaitForExit();

        var sb = new StringBuilder();
        foreach (var line in tail) sb.AppendLine(line);
        return (_child.ExitCode, sb.ToString().Trim());
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
                // Whole tree: a child that outlives the shell keeps port 8893
                // bound, and the next launch then fails preflight with a port
                // conflict we caused ourselves.
                child.Kill(entireProcessTree: true);
                child.WaitForExit(5000);
            }
        }
        catch (Exception ex)
        {
            _log($"error stopping hms_cpap: {ex.Message}");
        }
    }

    /// <summary>Validate and start again, after the user says they fixed something.</summary>
    /// Explicitly user-driven. The difference between this and the old backoff
    /// is consent: a person who has just edited config.json is asking for
    /// another attempt, which is not the same as the program deciding on its own
    /// that the twelfth try might behave differently from the eleventh.
    public void Retry()
    {
        Stop();
        _child = null;
        Start();
    }

    public void Dispose()
    {
        Stop();
        _child?.Dispose();
    }
}
