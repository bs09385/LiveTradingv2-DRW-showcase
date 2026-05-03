using System.Diagnostics;
using System.IO;

namespace PolymarketUI.Services;

public class EngineProcessService : IDisposable
{
    private Process? _process;
    private bool _disposed;

    public bool IsRunning => _process != null && !_process.HasExited;

    public event Action<string>? OutputReceived;
    public event Action<string>? ErrorOccurred;
    public event Action? EngineExited;

    public void Start(string configPath, string? accountPath = null)
    {
        if (IsRunning)
        {
            ErrorOccurred?.Invoke("Engine is already running");
            return;
        }

        var repoRoot = FindRepoRoot();
        if (repoRoot == null)
        {
            ErrorOccurred?.Invoke("Cannot find repo root (CMakeLists.txt + src/)");
            return;
        }

        var enginePath = Path.Combine(repoRoot, "build", "engine.exe");
        if (!File.Exists(enginePath))
        {
            ErrorOccurred?.Invoke($"Engine not built: {enginePath}");
            return;
        }

        var args = configPath;
        if (!string.IsNullOrEmpty(accountPath))
        {
            args += $" \"{accountPath}\"";
        }

        var psi = new ProcessStartInfo
        {
            FileName = enginePath,
            Arguments = args,
            WorkingDirectory = repoRoot,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true
        };

        // Set PATH for OpenSSL/MinGW DLLs
        var path = psi.Environment.ContainsKey("PATH") ? psi.Environment["PATH"] : "";
        psi.Environment["PATH"] =
            @"C:\ProgramData\mingw64\mingw64\opt\bin;C:\ProgramData\mingw64\mingw64\bin;" + path;

        try
        {
            _process = new Process { StartInfo = psi, EnableRaisingEvents = true };
            _process.OutputDataReceived += (_, e) =>
            {
                if (e.Data != null) OutputReceived?.Invoke(e.Data);
            };
            _process.ErrorDataReceived += (_, e) =>
            {
                if (e.Data != null) OutputReceived?.Invoke($"[stderr] {e.Data}");
            };
            _process.Exited += (_, _) => EngineExited?.Invoke();

            _process.Start();
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();

            OutputReceived?.Invoke($"Engine started (PID {_process.Id})");
        }
        catch (Exception ex)
        {
            ErrorOccurred?.Invoke($"Failed to start engine: {ex.Message}");
            _process = null;
        }
    }

    public void Stop()
    {
        if (_process == null || _process.HasExited)
            return;

        try
        {
            // Wait briefly for graceful shutdown (e.g., UI sent shutdown command via WS)
            _process.WaitForExit(2000);

            if (!_process.HasExited)
            {
                // Force kill — taskkill without /F doesn't work for
                // console apps started with CreateNoWindow=true
                _process.Kill(entireProcessTree: true);
            }

            OutputReceived?.Invoke("Engine stopped");
        }
        catch (Exception ex)
        {
            ErrorOccurred?.Invoke($"Error stopping engine: {ex.Message}");
        }
    }

    public static string? FindRepoRoot()
    {
        // Try from CWD first (e.g., dotnet run from project dir)
        var result = WalkUpForRepo(Directory.GetCurrentDirectory());
        if (result != null) return result;

        // Fallback: try from executable's directory (e.g., double-click .exe)
        var exeDir = AppDomain.CurrentDomain.BaseDirectory;
        return WalkUpForRepo(exeDir);
    }

    private static string? WalkUpForRepo(string startDir)
    {
        var dir = startDir;
        for (int i = 0; i < 10; i++)
        {
            if (File.Exists(Path.Combine(dir, "CMakeLists.txt")) &&
                Directory.Exists(Path.Combine(dir, "src")))
            {
                return dir;
            }
            var parent = Directory.GetParent(dir)?.FullName;
            if (parent == null) break;
            dir = parent;
        }
        return null;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Stop();
        _process?.Dispose();
    }
}
