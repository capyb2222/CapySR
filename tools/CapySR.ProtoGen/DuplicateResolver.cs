using System.Text.RegularExpressions;

namespace CapySR.ProtoGen;

// the dump's translation table isn't injective: 11 real names are claimed by two classes each.
// one keeps the name, the rest fall back to their obf name. picking wrong means answering on the
// wrong cmdid, so score it against independent signals instead of guessing.
public sealed class DuplicateResolver
{
    // deliberate calls, keyed by name. add one when the client proves which cmdid is real.
    private static readonly Dictionary<string, ushort> Overrides = new(StringComparer.Ordinal)
    {
        // 321 sits in CmdAvatarType next to DressRelicAvatarCsReq(364); rival 4515 is CmdPlayerReturnType
        ["DressRelicAvatarScRsp"] = 321,

        // UNVERIFIED: 716 { retcode=6 } vs 719 { unk=7, retcode=9 }, both CmdLineupType.
        // we only ever answer retcode 0, which proto3 omits, so both encode empty. flip if joining hangs.
        ["JoinLineupScRsp"] = 716,
    };

    private static readonly Regex ServiceEnumPattern = new(@"^enum\s+Cmd(?<service>\w+)Type\s*\{", RegexOptions.Compiled);
    private static readonly Regex EnumMemberPattern = new(@"=\s*(?<id>\d+)\s*;", RegexOptions.Compiled);

    private readonly Dictionary<ushort, List<string>> _services = [];
    private readonly HashSet<(ushort Id, string Name)> _packetIds = [];

    public DuplicateResolver(List<ProtoDeclaration> declarations, string packetIdsPath)
    {
        foreach (var declaration in declarations.Where(d => d.Kind == DeclarationKind.Enum))
        {
            var match = ServiceEnumPattern.Match(declaration.Body[0]);
            if (!match.Success)
            {
                continue;
            }

            var service = match.Groups["service"].Value;

            foreach (var line in declaration.Body.Skip(1))
            {
                var member = EnumMemberPattern.Match(line);
                if (member.Success && ushort.TryParse(member.Groups["id"].Value, out var id) && id != 0)
                {
                    if (!_services.TryGetValue(id, out var list))
                    {
                        _services[id] = list = [];
                    }

                    list.Add(service);
                }
            }
        }

        foreach (var entry in PacketIdFile.Read(packetIdsPath))
        {
            _packetIds.Add(entry);
        }
    }

    public ProtoDeclaration PickWinner(List<ProtoDeclaration> candidates) =>
        candidates.OrderByDescending(Score).ThenBy(c => c.CmdId ?? ushort.MaxValue).First();

    public string Explain(ProtoDeclaration declaration)
    {
        var services = declaration.CmdId is { } id && _services.TryGetValue(id, out var list)
            ? string.Join('/', list)
            : "-";

        return $"CmdID={declaration.CmdId?.ToString() ?? "?"} Type={declaration.PacketType ?? "?"} " +
               $"service={services} score={Score(declaration)}";
    }

    private int Score(ProtoDeclaration declaration)
    {
        if (declaration.CmdId is not { } id)
        {
            return 0;
        }

        var score = 0;

        if (Overrides.TryGetValue(declaration.Name, out var chosen) && chosen == id)
        {
            score += 8;
        }

        // a message in service "Avatar" should say Avatar in its name
        if (_services.TryGetValue(id, out var services) &&
            services.Any(s => declaration.Name.Contains(s, StringComparison.Ordinal)))
        {
            score += 4;
        }

        if (SuffixMatchesType(declaration))
        {
            score += 2;
        }

        if (_packetIds.Contains((id, declaration.Name)))
        {
            score += 1;
        }

        return score;
    }

    private static bool SuffixMatchesType(ProtoDeclaration declaration)
    {
        var expected = declaration.Name switch
        {
            var n when n.EndsWith("CsReq", StringComparison.Ordinal) => "Req",
            var n when n.EndsWith("ScRsp", StringComparison.Ordinal) => "Rsp",
            var n when n.EndsWith("Notify", StringComparison.Ordinal) => "Notify",
            _ => null,
        };

        return expected is not null && declaration.PacketType == expected;
    }
}

// "34 = PlayerLoginCsReq  // obf: ..."
public static class PacketIdFile
{
    public static IEnumerable<(ushort Id, string Name)> Read(string path)
    {
        if (!File.Exists(path))
        {
            yield break;
        }

        foreach (var line in File.ReadLines(path))
        {
            var trimmed = line.Trim();
            if (trimmed.Length == 0 || trimmed.StartsWith("//", StringComparison.Ordinal))
            {
                continue;
            }

            var eq = trimmed.IndexOf('=');
            if (eq < 0 || !ushort.TryParse(trimmed[..eq].Trim(), out var id))
            {
                continue;
            }

            var rest = trimmed[(eq + 1)..];
            var comment = rest.IndexOf("//", StringComparison.Ordinal);
            var name = (comment >= 0 ? rest[..comment] : rest).Trim();

            if (name.Length > 0)
            {
                yield return (id, name);
            }
        }
    }
}
