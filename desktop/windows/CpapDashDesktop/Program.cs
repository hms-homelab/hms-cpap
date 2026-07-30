namespace CpapDashDesktop;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        // A second shell would spawn a second hms_cpap, which would fail to bind
        // 8893 and pop the port-conflict dialog at a user who did nothing wrong.
        // The mutex is per-user (Local\) because the install is per-user.
        using var single = new Mutex(initiallyOwned: true,
                                     name: @"Local\CpapDashDesktop", out var isFirst);
        if (!isFirst)
        {
            MessageBox.Show("CpapDash Desktop is already running. Look for it in the "
                            + "notification area, next to the clock.",
                            "CpapDash Desktop", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        ApplicationConfiguration.Initialize();
        Application.Run(new TrayApplication());
    }
}
