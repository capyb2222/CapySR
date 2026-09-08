using System.Collections.Frozen;
using Google.Protobuf;
using Google.Protobuf.Reflection;

namespace CapySR.Protocol;

// cmdid <-> message type. the generator emits only names+ids; descriptors are
// resolved here so a renamed message fails at startup instead of silently vanishing.
public static class ProtocolRegistry
{
    private static readonly FrozenDictionary<ushort, MessageDescriptor> DescriptorsById;
    private static readonly FrozenDictionary<string, ushort> IdsByTypeName;
    private static readonly FrozenDictionary<ushort, string> NamesById;
    private static readonly FrozenDictionary<string, ushort> ResponseIdsByName;

    static ProtocolRegistry()
    {
        var file = StarRailReflection.Descriptor;

        var byId = new Dictionary<ushort, MessageDescriptor>(CmdIdTable.Names.Length);
        var byName = new Dictionary<string, ushort>(CmdIdTable.Names.Length, StringComparer.Ordinal);
        var names = new Dictionary<ushort, string>(CmdIdTable.Names.Length);

        var ids = CmdIdTable.Ids;
        var unresolved = new List<string>();

        for (var i = 0; i < CmdIdTable.Names.Length; i++)
        {
            var id = ids[i];
            var name = CmdIdTable.Names[i];
            names[id] = name;

            // no package in the cleaned proto, so full name == simple name
            var descriptor = file.FindTypeByName<MessageDescriptor>(name);
            if (descriptor is null)
            {
                unresolved.Add(name);
                continue;
            }

            byId[id] = descriptor;
            byName[descriptor.ClrType.FullName!] = id;
        }

        if (unresolved.Count > 0)
        {
            throw new InvalidOperationException(
                $"{unresolved.Count} cmdid(s) reference missing messages (rerun CapySR.ProtoGen): " +
                string.Join(", ", unresolved.Take(10)));
        }

        var responses = new Dictionary<string, ushort>(StringComparer.Ordinal);

        for (var i = 0; i < CmdIdTable.Names.Length; i++)
        {
            if (CmdIdTable.Names[i].EndsWith("ScRsp", StringComparison.Ordinal))
            {
                responses.TryAdd(CmdIdTable.Names[i], CmdIdTable.Ids[i]);
            }
        }

        ResponseIdsByName = responses.ToFrozenDictionary(StringComparer.Ordinal);
        DescriptorsById = byId.ToFrozenDictionary();
        IdsByTypeName = byName.ToFrozenDictionary(StringComparer.Ordinal);
        NamesById = names.ToFrozenDictionary();
    }

    public static int Count => DescriptorsById.Count;

    public static void EnsureInitialized() => _ = DescriptorsById.Count;

    public static string GetName(ushort cmdId) =>
        NamesById.TryGetValue(cmdId, out var name) ? name : $"Unknown({cmdId})";

    public static string GetName(CmdId cmdId) => GetName((ushort)cmdId);

    public static bool TryGetDescriptor(ushort cmdId, out MessageDescriptor descriptor) =>
        DescriptorsById.TryGetValue(cmdId, out descriptor!);

    public static ushort? GetCmdId(IMessage message) =>
        IdsByTypeName.TryGetValue(message.GetType().FullName!, out var id) ? id : null;

    public static ushort GetCmdId<T>() where T : IMessage<T> => CmdIdOf<T>.Value;

    // GetBagCsReq -> GetBagScRsp. the dump is not perfectly regular, so a few variants are
    // tried too: UpdateServerPrefsCsReq answers as UpdateServerPrefsDataScRsp, and some
    // XxxGetDataCsReq pair with GetXxxDataScRsp.
    public static bool TryGetResponseFor(ushort requestCmdId, out ushort responseCmdId)
    {
        responseCmdId = 0;

        if (!NamesById.TryGetValue(requestCmdId, out var name) ||
            !name.EndsWith("CsReq", StringComparison.Ordinal))
        {
            return false;
        }

        var stem = name[..^5];

        foreach (var candidate in ResponseCandidates(stem))
        {
            if (ResponseIdsByName.TryGetValue(candidate, out var id))
            {
                responseCmdId = id;
                return true;
            }
        }

        return false;
    }

    private static IEnumerable<string> ResponseCandidates(string stem)
    {
        yield return stem + "ScRsp";
        yield return stem + "DataScRsp";

        if (stem.EndsWith("Data", StringComparison.Ordinal))
        {
            yield return stem[..^4] + "ScRsp";
        }

        // XxxGetData -> GetXxxData
        if (stem.EndsWith("GetData", StringComparison.Ordinal))
        {
            yield return "Get" + stem[..^7] + "DataScRsp";
        }

        if (stem.StartsWith("Get", StringComparison.Ordinal))
        {
            yield return stem[3..] + "GetScRsp";
        }
    }

    public static IMessage? Parse(ushort cmdId, ReadOnlySpan<byte> body) =>
        DescriptorsById.TryGetValue(cmdId, out var descriptor) ? descriptor.Parser.ParseFrom(body) : null;

    private static class CmdIdOf<T> where T : IMessage<T>
    {
        public static readonly ushort Value = IdsByTypeName.TryGetValue(typeof(T).FullName!, out var id)
            ? id
            : throw new InvalidOperationException($"{typeof(T).Name} is not a top-level packet type.");
    }
}
