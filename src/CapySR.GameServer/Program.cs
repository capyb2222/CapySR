using CapySR.Common;
using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

var config = ServerConfig.Load();

var builder = Host.CreateApplicationBuilder(args);
builder.Logging.ClearProviders();
builder.Logging.AddSimpleConsole(o =>
{
    o.SingleLine = true;
    o.TimestampFormat = "HH:mm:ss ";
});
builder.Logging.AddProvider(new FileLoggerProvider("game"));

builder.Services.AddSingleton(config);
builder.Services.AddSingleton<KcpGateway>();

var host = builder.Build();
var logger = host.Services.GetRequiredService<ILoggerFactory>().CreateLogger("CapySR");

ProtocolRegistry.EnsureInitialized();
logger.LogInformation("logging to {Path}", Path.Combine(RepoPaths.Root, "logs", "game.log"));
logger.LogInformation("protocol: {Count} commands", ProtocolRegistry.Count);

var handlers = HandlerRegistry.Build(logger);
logger.LogInformation("handlers: {Handlers} implemented", handlers.HandlerCount);

GameWorld world;

try
{
    world = new GameWorld(config, logger);
}
catch (DirectoryNotFoundException e)
{
    // a fresh clone has no game data yet; say so plainly instead of dumping a stack trace
    logger.LogError("{Message}", e.Message);
    return 1;
}

var gateway = new KcpGateway(
    config,
    handlers,
    world,
    host.Services.GetRequiredService<ILoggerFactory>().CreateLogger<KcpGateway>());

using var cancellation = new CancellationTokenSource();
Console.CancelKeyPress += (_, e) =>
{
    e.Cancel = true;
    cancellation.Cancel();
};

await gateway.RunAsync(cancellation.Token);

return 0;
