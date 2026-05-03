using System.IO;
using System.Text.Json;
using PolymarketUI.Models;

namespace PolymarketUI.Services;

public class AccountService
{
    public List<AccountInfo> LoadAccounts()
    {
        var accounts = new List<AccountInfo>();
        var accountsDir = FindAccountsDirectory();
        if (accountsDir == null || !Directory.Exists(accountsDir))
            return accounts;

        foreach (var file in Directory.GetFiles(accountsDir, "*.json"))
        {
            try
            {
                var json = File.ReadAllText(file);
                using var doc = JsonDocument.Parse(json);
                var root = doc.RootElement;

                var info = new AccountInfo
                {
                    FilePath = file,
                    Name = root.TryGetProperty("name", out var nameProp)
                        ? nameProp.GetString() ?? "" : "",
                    Address = root.TryGetProperty("address", out var addrProp)
                        ? addrProp.GetString() ?? "" : ""
                };
                accounts.Add(info);
            }
            catch
            {
                // Skip malformed files silently
            }
        }

        return accounts;
    }

    private static string? FindAccountsDirectory()
    {
        // Walk up from CWD looking for accounts/ next to CMakeLists.txt + src/
        var dir = Directory.GetCurrentDirectory();
        for (int i = 0; i < 10; i++)
        {
            var candidate = Path.Combine(dir, "accounts");
            if (Directory.Exists(candidate) &&
                File.Exists(Path.Combine(dir, "CMakeLists.txt")) &&
                Directory.Exists(Path.Combine(dir, "src")))
            {
                return candidate;
            }
            var parent = Directory.GetParent(dir)?.FullName;
            if (parent == null) break;
            dir = parent;
        }

        // Fallback: accounts/ relative to CWD
        var fallback = Path.Combine(Directory.GetCurrentDirectory(), "accounts");
        return Directory.Exists(fallback) ? fallback : null;
    }
}
