namespace PolymarketUI.Models;

public class AccountInfo
{
    public string Name { get; set; } = "";
    public string Address { get; set; } = "";
    public string FilePath { get; set; } = "";

    public string DisplayName => !string.IsNullOrWhiteSpace(Name)
        ? Name
        : System.IO.Path.GetFileNameWithoutExtension(FilePath);

    public string ShortAddress
    {
        get
        {
            if (string.IsNullOrEmpty(Address) || Address.Length < 12)
                return Address;
            return $"{Address[..6]}...{Address[^4..]}";
        }
    }
}
