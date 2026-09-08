using System.Net;
using CapySR.Common;

namespace CapySR.SdkServer;

// Clients sometimes resolve autopatchcn.bhsr.com to a China-mainland edge that is
// unreachable from elsewhere, so the design-data download times out and the game hangs
// on the loading bar. We can reach the CDN, so proxy it and hand the client our own url.
public sealed class ResourceMirror(HotfixCache cache, ILogger<ResourceMirror> logger)
{
    public const string Route = "/mirror/{version}/{kind}/{**path}";

    private static readonly HttpClient Upstream = new(new HttpClientHandler
    {
        UseProxy = false,
        AutomaticDecompression = DecompressionMethods.All,
    })
    {
        Timeout = TimeSpan.FromMinutes(10),
    };

    public static string UrlFor(DispatchConfig dispatch, string version, string kind) =>
        $"http://{dispatch.PublicHost}:{dispatch.Port}/mirror/{Uri.EscapeDataString(version)}/{kind}";

    public async Task HandleAsync(HttpContext context, string version, string kind, string? path)
    {
        var hotfix = await cache.GetAsync(version, string.Empty);

        var baseUrl = hotfix is null ? null : kind switch
        {
            "asb" => hotfix.AssetBundleUrl,
            "ex" => hotfix.ExResourceUrl,
            "lua" => hotfix.LuaUrl,
            "ifix" => hotfix.IfixUrl,
            _ => null,
        };

        if (string.IsNullOrEmpty(baseUrl))
        {
            logger.LogWarning("mirror: no {Kind} url for {Version}", kind, version);
            context.Response.StatusCode = StatusCodes.Status404NotFound;
            return;
        }

        var target = $"{baseUrl.TrimEnd('/')}/{path}";

        using var request = new HttpRequestMessage(HttpMethod.Get, target);

        // the client resumes partial downloads, so range headers have to survive the hop
        if (context.Request.Headers.Range.Count > 0)
        {
            request.Headers.TryAddWithoutValidation("Range", (string?)context.Request.Headers.Range!);
        }

        try
        {
            using var response = await Upstream.SendAsync(
                request, HttpCompletionOption.ResponseHeadersRead, context.RequestAborted);

            context.Response.StatusCode = (int)response.StatusCode;

            foreach (var header in response.Content.Headers)
            {
                if (header.Key is "Content-Length" or "Content-Type" or "Content-Range" or "Last-Modified")
                {
                    context.Response.Headers[header.Key] = header.Value.ToArray();
                }
            }

            if (response.Headers.AcceptRanges.Count > 0)
            {
                context.Response.Headers.AcceptRanges = "bytes";
            }

            await response.Content.CopyToAsync(context.Response.Body, context.RequestAborted);
        }
        catch (OperationCanceledException)
        {
            // client gave up on the download; nothing to report
        }
        catch (HttpRequestException e)
        {
            logger.LogError("mirror: {Target} failed: {Message}", target, e.Message);

            if (!context.Response.HasStarted)
            {
                context.Response.StatusCode = StatusCodes.Status502BadGateway;
            }
        }
    }
}
