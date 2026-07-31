using System.Runtime.InteropServices;

namespace CpapDashDesktop;

/// <summary>
/// A Windows Job Object that kills the child when this process dies, however it
/// dies.
///
/// WHY THIS IS NOT OPTIONAL. Supervisor.Stop() handles a clean quit, but it only
/// runs if we get to run at all. Task Manager's "End task", a crash, or any
/// TerminateProcess skips every finally block, destructor and shutdown handler
/// in the runtime. The CI runtime test caught exactly that: force-killing the
/// tray left hms_cpap alive and holding port 8893, so the NEXT launch failed
/// preflight with a port conflict WE had caused.
///
/// A job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE moves the guarantee into
/// the kernel. The job handle is owned by this process; when the process ends
/// for any reason the handle closes, and Windows terminates everything assigned
/// to the job. No cooperation from our code is required, which is the point,
/// because the failure case is precisely the one where our code does not run.
/// </summary>
public sealed class ChildJob : IDisposable
{
    private IntPtr _handle = IntPtr.Zero;

    public ChildJob()
    {
        _handle = CreateJobObject(IntPtr.Zero, null);
        if (_handle == IntPtr.Zero) return;   // degrade quietly; Stop() still works

        var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            BasicLimitInformation = new JOBOBJECT_BASIC_LIMIT_INFORMATION
            {
                LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            }
        };

        var size = Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
        var ptr = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(limits, ptr, false);
            SetInformationJobObject(_handle, JobObjectExtendedLimitInformation,
                                    ptr, (uint)size);
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }

    /// <summary>Put a process under the job. Safe to call when setup failed.</summary>
    public bool Assign(IntPtr processHandle)
    {
        if (_handle == IntPtr.Zero) return false;
        return AssignProcessToJobObject(_handle, processHandle);
    }

    public void Dispose()
    {
        // Closing this handle is what triggers the kill, so it is deliberately
        // NOT closed while the child is meant to be running.
        if (_handle != IntPtr.Zero)
        {
            CloseHandle(_handle);
            _handle = IntPtr.Zero;
        }
    }

    // ── interop ──────────────────────────────────────────────────────────────

    private const int JobObjectExtendedLimitInformation = 9;
    private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;

    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
        public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string? lpName);

    [DllImport("kernel32.dll")]
    private static extern bool SetInformationJobObject(IntPtr hJob, int infoClass,
                                                       IntPtr lpJobObjectInfo, uint cbJobObjectInfoLength);

    [DllImport("kernel32.dll")]
    private static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);
}
