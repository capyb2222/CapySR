using System.Reflection;
using CapySR.Protocol;
using Google.Protobuf;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Net;

// marks a class whose static methods are packet handlers
[AttributeUsage(AttributeTargets.Class)]
public sealed class HandlersAttribute : Attribute;

public delegate Task PacketHandler(PlayerSession session, IMessage request);

// handlers are static methods taking (PlayerSession, TRequest); the cmdid comes from TRequest
public sealed class HandlerRegistry
{
    private readonly Dictionary<ushort, PacketHandler> _handlers = [];

    public int HandlerCount => _handlers.Count;

    public static HandlerRegistry Build(ILogger logger)
    {
        var registry = new HandlerRegistry();

        foreach (var type in Assembly.GetExecutingAssembly().GetTypes())
        {
            if (type.GetCustomAttribute<HandlersAttribute>() is not null)
            {
                registry.RegisterHandlers(type, logger);
            }
        }

        return registry;
    }

    private void RegisterHandlers(Type type, ILogger logger)
    {
        foreach (var method in type.GetMethods(BindingFlags.Public | BindingFlags.Static))
        {
            var parameters = method.GetParameters();

            if (parameters.Length != 2 || parameters[0].ParameterType != typeof(PlayerSession))
            {
                continue;
            }

            var requestType = parameters[1].ParameterType;
            if (!typeof(IMessage).IsAssignableFrom(requestType))
            {
                continue;
            }

            var cmdId = ProtocolRegistry.GetCmdId((IMessage)Activator.CreateInstance(requestType)!);
            if (cmdId is null)
            {
                logger.LogWarning("{Type}.{Method} takes {Request}, which has no cmdid",
                    type.Name, method.Name, requestType.Name);
                continue;
            }

            var target = method;

            // two handlers for one request would silently shadow each other
            if (_handlers.ContainsKey(cmdId.Value))
            {
                logger.LogWarning("{Type}.{Method} re-registers {Request}; the earlier handler is replaced",
                    type.Name, method.Name, requestType.Name);
            }

            _handlers[cmdId.Value] = (session, request) =>
            {
                var result = target.Invoke(null, [session, request]);
                return result as Task ?? Task.CompletedTask;
            };
        }
    }

    public bool TryGetHandler(ushort cmdId, out PacketHandler handler) =>
        _handlers.TryGetValue(cmdId, out handler!);
}
