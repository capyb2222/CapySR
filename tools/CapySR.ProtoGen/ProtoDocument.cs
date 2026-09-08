using System.Text;
using System.Text.RegularExpressions;

namespace CapySR.ProtoGen;

// one top-level message/enum plus the metadata comments the dumper put above it
public sealed class ProtoDeclaration
{
    public required DeclarationKind Kind { get; init; }

    public required string Name { get; set; }

    public required List<string> Comments { get; init; }

    // body[0] is always "message Foo {"
    public required List<string> Body { get; init; }

    public required int SourceLine { get; init; }

    public ushort? CmdId { get; init; }

    public string? PacketType { get; init; }

    public string? ObfuscatedName { get; init; }

    public bool WasRenamed { get; private set; }

    public string OriginalName { get; private set; } = string.Empty;

    public void Rename(string newName)
    {
        if (OriginalName.Length == 0)
        {
            OriginalName = Name;
        }

        Body[0] = $"{(Kind == DeclarationKind.Message ? "message" : "enum")} {newName} {{";
        Name = newName;
        WasRenamed = true;
    }

    public void Write(StringBuilder sb)
    {
        foreach (var comment in Comments)
        {
            sb.AppendLine(comment);
        }

        if (WasRenamed)
        {
            sb.AppendLine($"// renamed from duplicate '{OriginalName}'");
        }

        foreach (var line in Body)
        {
            sb.AppendLine(line);
        }

        sb.AppendLine();
    }
}

public enum DeclarationKind
{
    Message,
    Enum,
}

// the dump has no package/imports/options, so everything at depth 0 is a declaration
public static class ProtoParser
{
    private static readonly Regex DeclarationPattern =
        new(@"^(?<kind>message|enum)\s+(?<name>[A-Za-z_][A-Za-z0-9_]*)\s*\{", RegexOptions.Compiled);

    private static readonly Regex CmdIdPattern = new(@"^//\s*CmdID:\s*(?<id>\d+)\s*$", RegexOptions.Compiled);
    private static readonly Regex TypePattern = new(@"^//\s*Type:\s*(?<type>\w+)\s*$", RegexOptions.Compiled);
    private static readonly Regex ObfPattern = new(@"^//\s*obf:\s*(?<obf>[A-Za-z_][A-Za-z0-9_]*)\s*$", RegexOptions.Compiled);

    public static List<ProtoDeclaration> Parse(string[] lines)
    {
        var declarations = new List<ProtoDeclaration>();
        var pendingComments = new List<string>();

        for (var i = 0; i < lines.Length; i++)
        {
            var trimmed = lines[i].Trim();

            if (trimmed.Length == 0)
            {
                pendingComments.Clear();
                continue;
            }

            if (trimmed.StartsWith("//", StringComparison.Ordinal))
            {
                pendingComments.Add(trimmed);
                continue;
            }

            var match = DeclarationPattern.Match(trimmed);
            if (!match.Success)
            {
                pendingComments.Clear();
                continue;
            }

            var start = i;
            var depth = 0;
            var body = new List<string>();

            // no string literals in this dump, so raw brace counting is safe
            do
            {
                var current = lines[i];
                body.Add(current);
                depth += current.Count(c => c == '{');
                depth -= current.Count(c => c == '}');
                i++;
            }
            while (depth > 0 && i < lines.Length);

            i--;

            declarations.Add(new ProtoDeclaration
            {
                Kind = match.Groups["kind"].Value == "message" ? DeclarationKind.Message : DeclarationKind.Enum,
                Name = match.Groups["name"].Value,
                Comments = [.. pendingComments],
                Body = body,
                SourceLine = start + 1,
                CmdId = ExtractCmdId(pendingComments),
                PacketType = ExtractSingle(pendingComments, TypePattern, "type"),
                ObfuscatedName = ExtractSingle(pendingComments, ObfPattern, "obf"),
            });

            pendingComments.Clear();
        }

        return declarations;
    }

    private static ushort? ExtractCmdId(List<string> comments) =>
        ushort.TryParse(ExtractSingle(comments, CmdIdPattern, "id"), out var value) ? value : null;

    private static string? ExtractSingle(List<string> comments, Regex pattern, string group)
    {
        foreach (var comment in comments)
        {
            var match = pattern.Match(comment);
            if (match.Success)
            {
                return match.Groups[group].Value;
            }
        }

        return null;
    }
}
