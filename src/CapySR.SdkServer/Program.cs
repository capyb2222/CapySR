using CapySR.Common;
using CapySR.Protocol;
using CapySR.SdkServer;
using Google.Protobuf;

var config = ServerConfig.Load();

var builder = WebApplication.CreateBuilder(args);
builder.Logging.ClearProviders();
builder.Logging.AddSimpleConsole(o =>
{
    o.SingleLine = true;
    o.TimestampFormat = "HH:mm:ss ";
});
builder.Logging.AddProvider(new FileLoggerProvider("sdk"));

builder.WebHost.UseUrls($"http://{config.Dispatch.Host}:{config.Dispatch.Port}");
builder.Services.AddSingleton(config);
builder.Services.AddSingleton(config.Hotfix);
builder.Services.AddSingleton<HotfixCache>();
builder.Services.AddSingleton<ResourceMirror>();
builder.Services.AddCors(o => o.AddDefaultPolicy(p => p.AllowAnyOrigin().AllowAnyMethod().AllowAnyHeader()));

var app = builder.Build();
app.UseCors();

var log = app.Logger;
var cache = app.Services.GetRequiredService<HotfixCache>();
log.LogInformation("cached hotfix versions: {Versions}",
    cache.KnownVersions.Count > 0 ? string.Join(", ", cache.KnownVersions) : "none");

app.MapMethods("/query_dispatch", ["GET", "HEAD"], () =>
{
    var dispatch = new Dispatch
    {
        Retcode = 0,
        RegionList =
        {
            new RegionInfo
            {
                Name = config.Dispatch.RegionName,
                Title = config.Dispatch.RegionTitle,
                DisplayName = config.Dispatch.RegionTitle,
                EnvType = "9",
                DispatchUrl = config.Dispatch.QueryGatewayUrl,
            },
        },
    };

    return Results.Text(Convert.ToBase64String(dispatch.ToByteArray()));
});

app.MapMethods("/query_gateway", ["GET", "HEAD"], async (HttpContext context, HotfixCache hotfixCache) =>
{
    var version = context.Request.Query["version"].ToString();
    var seed = context.Request.Query["dispatch_seed"].ToString();

    var hotfix = await hotfixCache.GetAsync(version, seed);

    if (hotfix is null || !hotfix.IsUsable)
    {
        log.LogError(
            "no usable hotfix for '{Version}'. add it to config/versions.json or the client will hang at 99%",
            version);
    }

    var sendUrls = config.Hotfix.SendResourceUrls;
    var mirror = sendUrls && config.Hotfix.MirrorResources && version.Length > 0;

    string Resource(string kind, string? direct) =>
        !sendUrls ? string.Empty
        : mirror ? ResourceMirror.UrlFor(config.Dispatch, version, kind)
        : direct ?? string.Empty;

    var gateway = new GateServer
    {
        Ip = config.Dispatch.GatewayIp,
        Port = (uint)config.Dispatch.GatewayPort,
        RegionName = config.Dispatch.RegionName,
        AssetBundleUrl = Resource("asb", hotfix?.AssetBundleUrl),
        AssetBundleUrlAndroid = Resource("asb", hotfix?.AssetBundleUrl),
        ExResourceUrl = Resource("ex", hotfix?.ExResourceUrl),
        LuaUrl = Resource("lua", hotfix?.LuaUrl),
        IfixUrl = Resource("ifix", hotfix?.IfixUrl),
        IFixPatchRevision = "0",
        ServerDescription = config.Dispatch.RegionTitle,
        LoginWhiteMsg = config.Dispatch.RegionTitle,

        EnableDesignDataBundleVersionUpdate = config.Hotfix.EnableDesignDataUpdate,
        EnableVideoBundleVersionUpdate = config.Hotfix.EnableVideoUpdate,
        EnableSaveReplayFile = true,
        EnableUploadBattleLog = true,
        NetworkDiagnostic = true,
        EventTrackingOpen = true,
        ForbidRecharge = true,
        CloseRedeemCode = true,
        WatermarkEnable = true,
        AndroidMiddlePackageEnable = true,
        MtpSwitch = true,
        FtcSwitch = true,
        IosExam = true,
        NNFLHCGDGJM = true,
    };

    log.LogInformation(
        "gateway for {Version}: {Ip}:{Port}, design-data update {Update}, resources {Mode}",
        version, gateway.Ip, gateway.Port,
        config.Hotfix.EnableDesignDataUpdate ? "ON" : "off",
        mirror ? "mirrored" : sendUrls ? "direct" : "off");

    return Results.Text(Convert.ToBase64String(gateway.ToByteArray()));
});

app.MapPost("/{product}/mdk/shield/api/login", () => Results.Json(SdkResponses.Login));
app.MapPost("/{product}/mdk/shield/api/verify", () => Results.Json(SdkResponses.Login));
app.MapPost("/{product}/combo/granter/login/v2/login", () => Results.Json(SdkResponses.Granter));
app.MapPost("/account/risky/api/check", () => Results.Json(SdkResponses.RiskyCheck));
app.MapPost("/account/ma-cn-passport/app/loginByPassword", () => Results.Json(SdkResponses.PassportLogin));
app.MapPost("/account/ma-cn-session/app/verify", () => Results.Json(SdkResponses.PassportVerify));

app.MapGet(ResourceMirror.Route, (HttpContext context, ResourceMirror resourceMirror,
        string version, string kind, string? path) =>
    resourceMirror.HandleAsync(context, version, kind, path));

app.MapPost("/srtools", async (HttpContext context) =>
{
    var body = await new StreamReader(context.Request.Body).ReadToEndAsync();
    var result = SrToolsImport.Save(body, log);

    return Results.Json(new { message = result.Message, status = result.Status });
});

app.MapFallback((HttpContext context) =>
{
    log.LogWarning("unhandled {Method} {Path}", context.Request.Method, context.Request.Path);
    return Results.Json(new { retcode = 0, message = "OK" });
});

log.LogInformation("sdkserver listening on http://{Host}:{Port}", config.Dispatch.Host, config.Dispatch.Port);
app.Run();
