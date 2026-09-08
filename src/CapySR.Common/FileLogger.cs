using System.Collections.Concurrent;
using Microsoft.Extensions.Logging;

namespace CapySR.Common;

// always writes logs/<name>.log so diagnosing never depends on shell redirection
public sealed class FileLoggerProvider : ILoggerProvider
{
    private readonly BlockingCollection<string> _queue = new(new ConcurrentQueue<string>(), 8192);
    private readonly StreamWriter _writer;
    private readonly Thread _worker;

    public FileLoggerProvider(string name)
    {
        var directory = Path.Combine(RepoPaths.Root, "logs");
        Directory.CreateDirectory(directory);

        var file = Path.Combine(directory, $"{name}.log");
        FilePath = file;
        _writer = new StreamWriter(file, append: false) { AutoFlush = false };

        _worker = new Thread(Drain) { IsBackground = true, Name = $"{name}-log" };
        _worker.Start();
    }

    public string FilePath { get; }

    public ILogger CreateLogger(string categoryName) => new FileLogger(this, categoryName);

    internal void Write(string line)
    {
        // dropping a line beats blocking the game loop on disk
        _queue.TryAdd(line);
    }

    private void Drain()
    {
        foreach (var line in _queue.GetConsumingEnumerable())
        {
            _writer.WriteLine(line);

            if (_queue.Count == 0)
            {
                _writer.Flush();
            }
        }
    }

    public void Dispose()
    {
        _queue.CompleteAdding();
        _worker.Join(TimeSpan.FromSeconds(2));
        _writer.Flush();
        _writer.Dispose();
    }

    private sealed class FileLogger(FileLoggerProvider provider, string category) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => logLevel >= LogLevel.Information;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            if (!IsEnabled(logLevel))
            {
                return;
            }

            var name = category.Split('.')[^1];
            var line = $"{DateTime.Now:HH:mm:ss} {Short(logLevel)}: {name}: {formatter(state, exception)}";

            if (exception is not null)
            {
                line += Environment.NewLine + exception;
            }

            provider.Write(line);
        }

        private static string Short(LogLevel level) => level switch
        {
            LogLevel.Trace => "trce",
            LogLevel.Debug => "dbug",
            LogLevel.Information => "info",
            LogLevel.Warning => "WARN",
            LogLevel.Error => "FAIL",
            LogLevel.Critical => "CRIT",
            _ => "????",
        };
    }
}
